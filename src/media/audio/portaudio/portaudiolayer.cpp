/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
 *  Author: Edric Ladent-Milaret <edric.ladent-milaret@savoirfairelinux.com>
 *  Author: Guillaume Roguez <guillaume.roguez@savoirfairelinux.com>
 *  Author: Andreas Traczyk <andreas.traczyk@savoirfairelinux.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA.
 */

#include "portaudiolayer.h"
#include "manager.h"
#include "noncopyable.h"
#include "audio/resampler.h"
#include "audio/ringbufferpool.h"
#include "audio/ringbuffer.h"
#include "audio/audioloop.h"
#include "libav_deps.h"

#include <portaudio.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace sip_core {

enum Direction { Input = 0, Output = 1, IO = 2, End = 3 };

struct PortAudioLayer::PortAudioLayerImpl
{
    PortAudioLayerImpl(PortAudioLayer&, const AudioPreference&);
    ~PortAudioLayerImpl();

    void init(PortAudioLayer&);
    void initInput(PortAudioLayer&);
    void initOutput(PortAudioLayer&);
    void terminate() const;
    bool initInputStream(PortAudioLayer&);
    bool initOutputStream(PortAudioLayer&, bool ringtone = false);
    bool initFullDuplexStream(PortAudioLayer&);
    bool apiInitialised_ {false};

    std::vector<std::string> getDevicesByType(AudioDeviceType type) const;
    int getIndexByType(AudioDeviceType type);
    std::string getDeviceNameByType(const int index, AudioDeviceType type);
    PaDeviceIndex getApiIndexByType(AudioDeviceType type);
    std::string getApiDefaultDeviceName(AudioDeviceType type, bool commDevice) const;

    std::string deviceRecord_ {};
    std::string devicePlayback_ {};
    std::string deviceRingtone_ {};

    static constexpr const int defaultIndex_ {0};

    bool inputInitialized_ {false};
    bool outputInitialized_ {false};

    std::array<PaStream*, static_cast<int>(Direction::End)> streams_;

    // Track the actual PortAudio format opened for input/output streams
    // This is critical for proper format conversion in callbacks
    PaSampleFormat inputStreamFormat_ {paInt16};
    PaSampleFormat outputStreamFormat_ {paInt16};
    double inputStreamSampleRate_ {48000.0};
    double outputStreamSampleRate_ {48000.0};

    int paOutputCallback(PortAudioLayer& parent,
                         const void* inputBuffer,
                         void* outputBuffer,
                         unsigned long framesPerBuffer,
                         const PaStreamCallbackTimeInfo* timeInfo,
                         PaStreamCallbackFlags statusFlags);

    int paInputCallback(PortAudioLayer& parent,
                        const void* inputBuffer,
                        void* outputBuffer,
                        unsigned long framesPerBuffer,
                        const PaStreamCallbackTimeInfo* timeInfo,
                        PaStreamCallbackFlags statusFlags);

    int paIOCallback(PortAudioLayer& parent,
                     const void* inputBuffer,
                     void* outputBuffer,
                     unsigned long framesPerBuffer,
                     const PaStreamCallbackTimeInfo* timeInfo,
                     PaStreamCallbackFlags statusFlags);
};

//##################################################################################################

PortAudioLayer::PortAudioLayer(const AudioPreference& pref)
    : AudioLayer {pref}
    , pimpl_ {new PortAudioLayerImpl(*this, pref)}
{
    setHasNativeAEC(false);
    setHasNativeNS(false);

    auto numDevices = Pa_GetDeviceCount();
    if (numDevices < 0) {
        SIP_CORE_ERR("Pa_CountDevices returned 0x%x", numDevices);
        return;
    }
    const PaDeviceInfo* deviceInfo;
    for (auto i = 0; i < numDevices; i++) {
        deviceInfo = Pa_GetDeviceInfo(i);
        SIP_CORE_DBG("PortAudio device: %d, %s", i, deviceInfo->name);
    }
}

PortAudioLayer::~PortAudioLayer()
{
    stopStream();
}

std::vector<std::string>
PortAudioLayer::getCaptureDeviceList() const
{
    return pimpl_->getDevicesByType(AudioDeviceType::CAPTURE);
}

std::vector<std::string>
PortAudioLayer::getPlaybackDeviceList() const
{
    return pimpl_->getDevicesByType(AudioDeviceType::PLAYBACK);
}

int
PortAudioLayer::getAudioDeviceIndex(const std::string& name, AudioDeviceType type) const
{
    auto devices = pimpl_->getDevicesByType(type);
    auto it = std::find_if(devices.cbegin(), devices.cend(), [&name](const auto& deviceName) {
        return deviceName == name;
    });
    return it != devices.end() ? std::distance(devices.cbegin(), it) : -1;
}

std::string
PortAudioLayer::getAudioDeviceName(int index, AudioDeviceType type) const
{
    (void) index;
    (void) type;
    return {};
}

int
PortAudioLayer::getIndexCapture() const
{
    return pimpl_->getIndexByType(AudioDeviceType::CAPTURE);
}

int
PortAudioLayer::getIndexPlayback() const
{
    auto index = pimpl_->getIndexByType(AudioDeviceType::PLAYBACK);
    return index;
}

int
PortAudioLayer::getIndexRingtone() const
{
    return pimpl_->getIndexByType(AudioDeviceType::RINGTONE);
}

void
PortAudioLayer::startStream(AudioDeviceType stream)
{
    if (!pimpl_->apiInitialised_) {
        SIP_CORE_WARN("PortAudioLayer API not initialised");
        return;
    }

    auto startPlayback = [this](bool fullDuplexMode = false, bool ringtone = false) -> bool {
        std::unique_lock<std::mutex> lock(mutex_);
        if (status_.load() != Status::Idle)
            return false;
        bool ret {false};
        if (fullDuplexMode)
            ret = pimpl_->initFullDuplexStream(*this);
        else
            ret = pimpl_->initOutputStream(*this, ringtone);
        if (ret) {
            status_.store(Status::Started);
            lock.unlock();
            flushUrgent();
            flushMain();
        }
        return ret;
    };

    switch (stream) {
    case AudioDeviceType::ALL:
        if (!startPlayback(true)) {
            pimpl_->initInputStream(*this);
            startPlayback();
        }
        break;
    case AudioDeviceType::CAPTURE:
        pimpl_->initInputStream(*this);
        break;
    case AudioDeviceType::PLAYBACK:
        startPlayback();
        break;
    case AudioDeviceType::RINGTONE:
        startPlayback(false, true);
        break;
    }
}

void
PortAudioLayer::stopStream(AudioDeviceType stream)
{
    auto stopPaStream = [](PaStream* stream) -> bool {
        if (!stream || Pa_IsStreamStopped(stream) != paNoError)
            return false;
        auto err = Pa_StopStream(stream);
        if (err != paNoError) {
            SIP_CORE_ERR("Pa_StopStream error : %s", Pa_GetErrorText(err));
            return false;
        }
        err = Pa_CloseStream(stream);
        if (err != paNoError) {
            SIP_CORE_ERR("Pa_CloseStream error : %s", Pa_GetErrorText(err));
            return false;
        }
        return true;
    };

    auto stopPlayback = [this, &stopPaStream](bool fullDuplexMode = false) -> bool {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.load() != Status::Started)
            return false;
        bool stopped = false;
        if (fullDuplexMode)
            stopped = stopPaStream(pimpl_->streams_[Direction::IO]);
        else
            stopped = stopPaStream(pimpl_->streams_[Direction::Output]);
        if (stopped)
            status_.store(Status::Idle);
        return stopped;
    };

    bool stopped = false;
    switch (stream) {
    case AudioDeviceType::ALL:
        if (pimpl_->streams_[Direction::IO]) {
            stopped = stopPlayback(true);
        } else {
            stopped = stopPaStream(pimpl_->streams_[Direction::Input]) && stopPlayback();
        }
        if (stopped) {
            recordChanged(false);
            playbackChanged(false);
            SIP_CORE_DBG("PortAudioLayer I/O streams stopped");
        } else
            return;
        break;
    case AudioDeviceType::CAPTURE:
        if (stopPaStream(pimpl_->streams_[Direction::Input])) {
            recordChanged(false);
            SIP_CORE_DBG("PortAudioLayer input stream stopped");
        } else
            return;
        break;
    case AudioDeviceType::PLAYBACK:
    case AudioDeviceType::RINGTONE:
        if (stopPlayback()) {
            playbackChanged(false);
            SIP_CORE_DBG("PortAudioLayer output stream stopped");
        } else
            return;
        break;
    }

    // Flush the ring buffers
    flushUrgent();
    flushMain();
}

void
PortAudioLayer::updatePreference(AudioPreference& preference, int index, AudioDeviceType type)
{
    auto deviceName = pimpl_->getDeviceNameByType(index, type);
    switch (type) { 
    case AudioDeviceType::PLAYBACK:
        preference.setPortAudioDevicePlayback(deviceName);
        break;
    case AudioDeviceType::CAPTURE:
        preference.setPortAudioDeviceRecord(deviceName);
        break;
    case AudioDeviceType::RINGTONE:
        preference.setPortAudioDeviceRingtone(deviceName);
        break;
    default:
        break;
    }
}

//##################################################################################################

PortAudioLayer::PortAudioLayerImpl::PortAudioLayerImpl(PortAudioLayer& parent,
                                                       const AudioPreference& pref)
    : deviceRecord_ {pref.getPortAudioDeviceRecord()}
    , devicePlayback_ {pref.getPortAudioDevicePlayback()}
    , deviceRingtone_ {pref.getPortAudioDeviceRingtone()}
{
    SIP_CORE_INFO() << "PortAudioLayerImpl: prefs are " << deviceRecord_ << " ; " << devicePlayback_
                    << "; " << deviceRingtone_;
    init(parent);
}

PortAudioLayer::PortAudioLayerImpl::~PortAudioLayerImpl()
{
    terminate();
}

void
PortAudioLayer::PortAudioLayerImpl::initInput(PortAudioLayer& parent)
{
    // convert out preference to an api index
    auto apiIndex = getApiIndexByType(AudioDeviceType::CAPTURE);

    // Pa_GetDefault[Comm]InputDevice returned paNoDevice or we already initialized the device
    if (apiIndex == paNoDevice || inputInitialized_)
        return;

    const auto inputDeviceInfo = Pa_GetDeviceInfo(apiIndex);
    if (!inputDeviceInfo) {
        // this represents complete failure after attempting a fallback to default
        SIP_CORE_WARN("PortAudioLayer could not initialize input");
        deviceRecord_.clear();
        inputInitialized_ = true;
        return;
    }

    // if the device index is somehow no longer a device of the correct type, reset the
    // internal index to paNoDevice and reenter in an attempt to set the default
    // communications device
    if (inputDeviceInfo->maxInputChannels <= 0) {
        SIP_CORE_WARN("PortAudioLayer could not initialize input, falling back to default device");
        deviceRecord_.clear();
        return initInput(parent);
    }

    // at this point, the device is of the correct type and can be opened
    parent.audioInputFormat_.sample_rate = inputDeviceInfo->defaultSampleRate;
    parent.audioInputFormat_.nb_channels = inputDeviceInfo->maxInputChannels;
    parent.hardwareInputFormatAvailable(parent.audioInputFormat_);
    SIP_CORE_DBG("PortAudioLayer initialized input: %s {%d Hz, %d channels}",
             inputDeviceInfo->name,
             parent.audioInputFormat_.sample_rate,
             parent.audioInputFormat_.nb_channels);
    inputInitialized_ = true;
}

void
PortAudioLayer::PortAudioLayerImpl::initOutput(PortAudioLayer& parent)
{
    // convert out preference to an api index
    auto apiIndex = getApiIndexByType(AudioDeviceType::PLAYBACK);

    // Pa_GetDefault[Comm]OutputDevice returned paNoDevice or we already initialized the device
    if (apiIndex == paNoDevice || outputInitialized_)
        return;

    const auto outputDeviceInfo = Pa_GetDeviceInfo(apiIndex);
    if (!outputDeviceInfo) {
        // this represents complete failure after attempting a fallback to default
        SIP_CORE_WARN("PortAudioLayer could not initialize output");
        devicePlayback_.clear();
        outputInitialized_ = true;
        return;
    }

    // if the device index is somehow no longer a device of the correct type, reset the
    // internal index to paNoDevice and reenter in an attempt to set the default
    // communications device
    if (outputDeviceInfo->maxOutputChannels <= 0) {
        SIP_CORE_WARN("PortAudioLayer could not initialize output, falling back to default device");
        devicePlayback_.clear();
        return initOutput(parent);
    }

    // at this point, the device is of the correct type and can be opened
    parent.audioFormat_.sample_rate = outputDeviceInfo->defaultSampleRate;
    parent.audioFormat_.nb_channels = outputDeviceInfo->maxOutputChannels;
    parent.hardwareFormatAvailable(parent.audioFormat_);
    SIP_CORE_DBG("PortAudioLayer initialized output: %s {%d Hz, %d channels}",
             outputDeviceInfo->name,
             parent.audioFormat_.sample_rate,
             parent.audioFormat_.nb_channels);
    outputInitialized_ = true;
}

void
PortAudioLayer::PortAudioLayerImpl::init(PortAudioLayer& parent)
{
    SIP_CORE_DBG("PortAudioLayer Init");
    const auto err = Pa_Initialize();
    auto apiIndex = Pa_GetDefaultHostApi();
    auto apiInfo = Pa_GetHostApiInfo(apiIndex);
    if (err != paNoError || apiInfo == nullptr) {
        SIP_CORE_ERR("PortAudioLayer error : %s", Pa_GetErrorText(err));
        terminate();
        return;
    }

    apiInitialised_ = true;
    SIP_CORE_DBG() << "Portaudio initialized using: " << apiInfo->name;

    initInput(parent);
    initOutput(parent);

    std::fill(std::begin(streams_), std::end(streams_), nullptr);
}

std::vector<std::string>
PortAudioLayer::PortAudioLayerImpl::getDevicesByType(AudioDeviceType type) const
{
    std::vector<std::string> devices;
    auto numDevices = Pa_GetDeviceCount();
    if (numDevices < 0)
        SIP_CORE_ERR("PortAudioLayer error : %s", Pa_GetErrorText(numDevices));
    else {
        for (int i = 0; i < numDevices; i++) {
            const auto deviceInfo = Pa_GetDeviceInfo(i);
            if (type == AudioDeviceType::CAPTURE) {
                if (deviceInfo->maxInputChannels > 0)
                    devices.push_back(deviceInfo->name);
            } else if (deviceInfo->maxOutputChannels > 0)
                devices.push_back(deviceInfo->name);
        }
        // add the default device aliases if requested and if there are any devices of this type
        if (!devices.empty()) {
            // default comm (index:0)
            auto defaultDeviceName = getApiDefaultDeviceName(type, true);
            devices.insert(devices.begin(), "{{Default}} - " + defaultDeviceName);
        }
    }
    return devices;
}

int
PortAudioLayer::PortAudioLayerImpl::getIndexByType(AudioDeviceType type)
{
    auto devices = getDevicesByType(type);
    if (!devices.size()) {
        return 0;
    }
    std::string_view toMatch = (type == AudioDeviceType::CAPTURE
                                    ? deviceRecord_
                                    : (type == AudioDeviceType::PLAYBACK ? devicePlayback_
                                                                         : deviceRingtone_));
    auto it = std::find_if(devices.cbegin(), devices.cend(), [&toMatch](const auto& deviceName) {
        return deviceName == toMatch;
    });
    return it != devices.end() ? std::distance(devices.cbegin(), it) : 0;
}

std::string
PortAudioLayer::PortAudioLayerImpl::getDeviceNameByType(const int index, AudioDeviceType type)
{
    if (index == defaultIndex_)
        return {};

    auto devices = getDevicesByType(type);
    if (!devices.size() || index >= devices.size())
        return {};

    return devices.at(index);
}

PaDeviceIndex
PortAudioLayer::PortAudioLayerImpl::getApiIndexByType(AudioDeviceType type)
{
    auto numDevices = Pa_GetDeviceCount();
    if (numDevices < 0)
        SIP_CORE_ERR("PortAudioLayer error : %s", Pa_GetErrorText(numDevices));
    else {
        std::string_view toMatch = (type == AudioDeviceType::CAPTURE
                                        ? deviceRecord_
                                        : (type == AudioDeviceType::PLAYBACK ? devicePlayback_
                                                                             : deviceRingtone_));
        if (toMatch.empty())
            return type == AudioDeviceType::CAPTURE ? Pa_GetDefaultInputDevice()
                                                    : Pa_GetDefaultOutputDevice();
        for (int i = 0; i < numDevices; ++i) {
            if (const auto deviceInfo = Pa_GetDeviceInfo(i)) {
                if (deviceInfo->name == toMatch)
                    return i;
            }
        }
    }
    return paNoDevice;
}

std::string
PortAudioLayer::PortAudioLayerImpl::getApiDefaultDeviceName(AudioDeviceType type,
                                                            bool commDevice) const
{
    std::string deviceName {};
    PaDeviceIndex deviceIndex {paNoDevice};
    if (type == AudioDeviceType::CAPTURE) {
        deviceIndex = Pa_GetDefaultInputDevice();
    } else {
        deviceIndex = Pa_GetDefaultOutputDevice();
    }
    if (const auto deviceInfo = Pa_GetDeviceInfo(deviceIndex)) {
        deviceName = deviceInfo->name;
    }
    return deviceName;
}

void
PortAudioLayer::PortAudioLayerImpl::terminate() const
{
    SIP_CORE_DBG("PortAudioLayer terminate.");
    auto err = Pa_Terminate();
    if (err != paNoError)
        SIP_CORE_ERR("PortAudioLayer error : %s", Pa_GetErrorText(err));
}

#include <vector>
#include <utility>

using FormatRatePair = std::pair<PaSampleFormat, double>;

std::vector<FormatRatePair> getSupportedFormatSampleRates(PaDeviceIndex device, Direction direction) {
    std::vector<FormatRatePair> supported;

    auto* deviceInfo = Pa_GetDeviceInfo(device);
    if (!deviceInfo) return supported;

    bool isOut = direction == Direction::Output;
    int maxChannels = isOut ? deviceInfo->maxOutputChannels : deviceInfo->maxInputChannels;

    PaStreamParameters params;
    params.device = device;
    params.channelCount = maxChannels;
    params.hostApiSpecificStreamInfo = nullptr;
    params.suggestedLatency = isOut ? deviceInfo->defaultLowOutputLatency
                                    : deviceInfo->defaultLowInputLatency;

    const std::vector<PaSampleFormat> formats = {
        paFloat32, paInt32, paInt24, paInt16, paInt8, paUInt8
    };

    const std::vector<double> sampleRates = {
        8000.0, 16000.0, 22050.0, 32000.0,
        44100.0, 48000.0, 88200.0, 96000.0, 192000.0
    };
    
    for (auto fmt : formats) {
        params.sampleFormat = fmt;

        for (auto rate : sampleRates) {
            PaError err = Pa_IsFormatSupported(
                isOut ? nullptr : &params,
                isOut ? &params : nullptr,
                rate);

            if (err == paFormatIsSupported) {
                supported.emplace_back(fmt, rate);
            }
        }
    }

    return supported;
}

const char* formatToString(PaSampleFormat fmt) {
    switch (fmt) {
        case paFloat32: return "Float32";
        case paInt32:   return "Int32";
        case paInt24:   return "Int24";
        case paInt16:   return "Int16";
        case paInt8:    return "Int8";
        case paUInt8:   return "UInt8";
        default:        return "Unknown";
    }
}

// Get the size in bytes of a single sample for a given PortAudio format
static size_t getBytesPerSample(PaSampleFormat fmt) {
    switch (fmt) {
        case paFloat32: return 4;
        case paInt32:   return 4;
        case paInt24:   return 3;
        case paInt16:   return 2;
        case paInt8:    return 1;
        case paUInt8:   return 1;
        default:        return 2; // fallback to Int16 size
    }
}

// Convert PortAudio format to FFmpeg AVSampleFormat
static AVSampleFormat paFormatToAVFormat(PaSampleFormat paFmt) {
    switch (paFmt) {
        case paFloat32: return AV_SAMPLE_FMT_FLT;
        case paInt32:   return AV_SAMPLE_FMT_S32;
        case paInt16:   return AV_SAMPLE_FMT_S16;
        // paInt24, paInt8, paUInt8 don't have direct AVSampleFormat equivalents
        // We'll handle these by converting to S16 or S32 in the conversion functions
        case paInt24:   return AV_SAMPLE_FMT_S32; // Will need to pack/unpack
        case paInt8:    return AV_SAMPLE_FMT_S16; // Will need to scale
        case paUInt8:   return AV_SAMPLE_FMT_S16; // Will need to scale and offset
        default:        return AV_SAMPLE_FMT_S16;
    }
}

// Convert PortAudio input buffer to int16_t samples (AudioSample)
// This handles all PortAudio formats and converts them to 16-bit PCM
static void convertPaInputToInt16(const void* paBuffer, 
                                   int16_t* outBuffer,
                                   size_t frameCount,
                                   int channels,
                                   PaSampleFormat paFormat) {
    const size_t sampleCount = frameCount * channels;
    
    switch (paFormat) {
        case paFloat32: {
            const float* src = static_cast<const float*>(paBuffer);
            for (size_t i = 0; i < sampleCount; ++i) {
                // Clamp float to [-1.0, 1.0] and convert to int16
                float sample = src[i];
                sample = std::max(-1.0f, std::min(1.0f, sample));
                outBuffer[i] = static_cast<int16_t>(sample * 32767.0f);
            }
            break;
        }
        case paInt32: {
            const int32_t* src = static_cast<const int32_t*>(paBuffer);
            for (size_t i = 0; i < sampleCount; ++i) {
                // Shift right by 16 bits to convert 32-bit to 16-bit
                outBuffer[i] = static_cast<int16_t>(src[i] >> 16);
            }
            break;
        }
        case paInt24: {
            // Int24 is packed as 3 bytes per sample (little-endian)
            const uint8_t* src = static_cast<const uint8_t*>(paBuffer);
            for (size_t i = 0; i < sampleCount; ++i) {
                // Read 24-bit sample (little-endian: low, mid, high)
                int32_t sample = src[i * 3] | (src[i * 3 + 1] << 8) | (src[i * 3 + 2] << 16);
                // Sign extend if negative (bit 23 is sign bit)
                if (sample & 0x800000) {
                    sample |= 0xFF000000;
                }
                // Shift right by 8 to convert 24-bit to 16-bit
                outBuffer[i] = static_cast<int16_t>(sample >> 8);
            }
            break;
        }
        case paInt16: {
            // Direct copy - no conversion needed
            const int16_t* src = static_cast<const int16_t*>(paBuffer);
            std::copy_n(src, sampleCount, outBuffer);
            break;
        }
        case paInt8: {
            const int8_t* src = static_cast<const int8_t*>(paBuffer);
            for (size_t i = 0; i < sampleCount; ++i) {
                // Scale 8-bit to 16-bit (multiply by 256)
                outBuffer[i] = static_cast<int16_t>(src[i]) << 8;
            }
            break;
        }
        case paUInt8: {
            const uint8_t* src = static_cast<const uint8_t*>(paBuffer);
            for (size_t i = 0; i < sampleCount; ++i) {
                // Convert unsigned 8-bit [0, 255] to signed 16-bit [-32768, 32767]
                // First convert to signed by subtracting 128, then scale
                outBuffer[i] = (static_cast<int16_t>(src[i]) - 128) << 8;
            }
            break;
        }
        default:
            SIP_CORE_ERR("[PortAudio Format Convert] Unknown format %d, zeroing output", paFormat);
            std::fill_n(outBuffer, sampleCount, static_cast<int16_t>(0));
            break;
    }
}

// Convert int16_t samples (AudioSample) to PortAudio output buffer
// This handles all PortAudio formats
static void convertInt16ToPaOutput(const int16_t* inBuffer,
                                    void* paBuffer,
                                    size_t frameCount,
                                    int channels,
                                    PaSampleFormat paFormat) {
    const size_t sampleCount = frameCount * channels;
    
    switch (paFormat) {
        case paFloat32: {
            float* dst = static_cast<float*>(paBuffer);
            for (size_t i = 0; i < sampleCount; ++i) {
                // Convert int16 to float [-1.0, 1.0]
                dst[i] = static_cast<float>(inBuffer[i]) / 32768.0f;
            }
            break;
        }
        case paInt32: {
            int32_t* dst = static_cast<int32_t*>(paBuffer);
            for (size_t i = 0; i < sampleCount; ++i) {
                // Shift left by 16 bits to convert 16-bit to 32-bit
                dst[i] = static_cast<int32_t>(inBuffer[i]) << 16;
            }
            break;
        }
        case paInt24: {
            // Int24 is packed as 3 bytes per sample (little-endian)
            uint8_t* dst = static_cast<uint8_t*>(paBuffer);
            for (size_t i = 0; i < sampleCount; ++i) {
                // Shift left by 8 to convert 16-bit to 24-bit
                int32_t sample = static_cast<int32_t>(inBuffer[i]) << 8;
                // Write 24-bit sample (little-endian: low, mid, high)
                dst[i * 3] = sample & 0xFF;
                dst[i * 3 + 1] = (sample >> 8) & 0xFF;
                dst[i * 3 + 2] = (sample >> 16) & 0xFF;
            }
            break;
        }
        case paInt16: {
            // Direct copy - no conversion needed
            int16_t* dst = static_cast<int16_t*>(paBuffer);
            std::copy_n(inBuffer, sampleCount, dst);
            break;
        }
        case paInt8: {
            int8_t* dst = static_cast<int8_t*>(paBuffer);
            for (size_t i = 0; i < sampleCount; ++i) {
                // Scale 16-bit to 8-bit (divide by 256)
                dst[i] = static_cast<int8_t>(inBuffer[i] >> 8);
            }
            break;
        }
        case paUInt8: {
            uint8_t* dst = static_cast<uint8_t*>(paBuffer);
            for (size_t i = 0; i < sampleCount; ++i) {
                // Convert signed 16-bit to unsigned 8-bit [0, 255]
                // Scale down and add offset of 128
                dst[i] = static_cast<uint8_t>((inBuffer[i] >> 8) + 128);
            }
            break;
        }
        default:
            SIP_CORE_ERR("[PortAudio Format Convert] Unknown format %d, zeroing output", paFormat);
            std::memset(paBuffer, 0, sampleCount * getBytesPerSample(paFormat));
            break;
    }
}

// Fill PortAudio output buffer with silence in the appropriate format
static void fillPaBufferWithSilence(void* paBuffer,
                                     size_t frameCount,
                                     int channels,
                                     PaSampleFormat paFormat) {
    const size_t sampleCount = frameCount * channels;
    
    switch (paFormat) {
        case paFloat32: {
            float* dst = static_cast<float*>(paBuffer);
            std::fill_n(dst, sampleCount, 0.0f);
            break;
        }
        case paInt32: {
            int32_t* dst = static_cast<int32_t*>(paBuffer);
            std::fill_n(dst, sampleCount, static_cast<int32_t>(0));
            break;
        }
        case paInt24: {
            // Zero out all bytes
            std::memset(paBuffer, 0, sampleCount * 3);
            break;
        }
        case paInt16: {
            int16_t* dst = static_cast<int16_t*>(paBuffer);
            std::fill_n(dst, sampleCount, static_cast<int16_t>(0));
            break;
        }
        case paInt8: {
            int8_t* dst = static_cast<int8_t*>(paBuffer);
            std::fill_n(dst, sampleCount, static_cast<int8_t>(0));
            break;
        }
        case paUInt8: {
            // Silence for unsigned 8-bit is 128 (midpoint)
            uint8_t* dst = static_cast<uint8_t*>(paBuffer);
            std::fill_n(dst, sampleCount, static_cast<uint8_t>(128));
            break;
        }
        default:
            std::memset(paBuffer, 0, sampleCount * getBytesPerSample(paFormat));
            break;
    }
}

static std::pair<PaSampleFormat, double>
openStreamDevice(PaStream**      stream,
                 PaDeviceIndex   device,
                 Direction       direction,
                 PaStreamCallback* callback,
                 void*           user_data)
{
    auto is_out = (direction == Direction::Output);

    const PaDeviceInfo* device_info = Pa_GetDeviceInfo(device);
    if (!device_info) {
        SIP_CORE_ERR("PortAudioLayer error: Invalid device info.");
        return { 0, 0.0 };
    }

    SIP_CORE_INFO() << "PortAudioLayer: openStreamDevice " << (is_out ? "OUTPUT" : "INPUT")
                    << ", device info : name " << device_info->name;

    // Get all (format, rate) combinations that this device actually supports.
    auto supportedCombinations = getSupportedFormatSampleRates(device, direction);
    SIP_CORE_INFO() << "PortAudioLayer: Supported format/sample rate combinations:";

    if (supportedCombinations.empty()) {
        SIP_CORE_ERR("PortAudioLayer: No supported format/sample rate combinations found for device %d (%s).",
                     device, device_info->name);
        return { 0, 0.0 };
    }

    const double         requested_rate   = device_info->defaultSampleRate;
    const PaSampleFormat requested_format = paInt16;

    // Build a prioritized list of format/rate pairs to try:
    // 1. Exact match (requested_format @ requested_rate)
    // 2. Requested format at any rate
    // 3. Any format at requested rate
    // 4. Any other combination
    std::vector<FormatRatePair> prioritizedCombinations;
    std::vector<FormatRatePair> formatMatchOnly;
    std::vector<FormatRatePair> rateMatchOnly;
    std::vector<FormatRatePair> noMatch;

    for (const auto& [fmt, rate] : supportedCombinations) {
        SIP_CORE_INFO() << "PortAudioLayer: - Format: " << formatToString(fmt) 
                        << ", Rate: " << rate;

        if (rate == requested_rate && fmt == requested_format) {
            // Exact match - highest priority
            prioritizedCombinations.insert(prioritizedCombinations.begin(), {fmt, rate});
            SIP_CORE_INFO() << "PortAudioLayer: Found exact match (preferred)!";
        } else if (fmt == requested_format) {
            // Format matches, different rate
            formatMatchOnly.push_back({fmt, rate});
        } else if (rate == requested_rate) {
            // Rate matches, different format
            rateMatchOnly.push_back({fmt, rate});
        } else {
            // No match
            noMatch.push_back({fmt, rate});
        }
    }

    // Append in priority order
    prioritizedCombinations.insert(prioritizedCombinations.end(), formatMatchOnly.begin(), formatMatchOnly.end());
    prioritizedCombinations.insert(prioritizedCombinations.end(), rateMatchOnly.begin(), rateMatchOnly.end());
    prioritizedCombinations.insert(prioritizedCombinations.end(), noMatch.begin(), noMatch.end());

    PaStreamParameters params;
    params.device = device;
    params.channelCount = is_out ? device_info->maxOutputChannels
                                 : device_info->maxInputChannels;
    params.suggestedLatency = is_out ? device_info->defaultLowOutputLatency
                                     : device_info->defaultLowInputLatency;
    params.hostApiSpecificStreamInfo = nullptr;

    // Try each combination until one works
    for (const auto& [fmt, rate] : prioritizedCombinations) {
        params.sampleFormat = fmt;

        SIP_CORE_INFO() << "PortAudioLayer: Attempting to open stream with format "
                        << formatToString(fmt) << " @ rate " << rate;

        if (!is_out) {
            PaError supportErr = Pa_IsFormatSupported(&params, nullptr, rate);
            SIP_CORE_INFO() << "PortAudioLayer: Is format supported (input): " << supportErr;
        }

        PaError err = Pa_OpenStream(stream,
                                    is_out ? nullptr         : &params,
                                    is_out ? &params         : nullptr,
                                    rate,
                                    paFramesPerBufferUnspecified,
                                    paNoFlag,
                                    callback,
                                    user_data);

        if (err == paNoError) {
            SIP_CORE_INFO() << "PortAudioLayer: Successfully opened stream with format "
                            << formatToString(fmt) << " @ rate " << rate;
            return { fmt, rate };
        }

        // Log the error and continue trying other combinations
        const char* errorText = Pa_GetErrorText(err);
        SIP_CORE_WARN("PortAudioLayer: Failed to open stream with format %s @ rate %.0f: %s. Trying next combination...",
                      formatToString(fmt), rate, errorText);
    }

    // All combinations failed
    SIP_CORE_ERR("PortAudioLayer: Failed to open stream with any supported format/rate combination for device %d (%s).",
                 device, device_info->name);
    emitSignal<libsip_core::ConfigurationSignal::DeviceOpenError>("All format/rate combinations failed", is_out);

    return { 0, 0.0 };
}

static void
openFullDuplexStream(PaStream** stream,
                     PaDeviceIndex inputDeviceIndex,
                     PaDeviceIndex ouputDeviceIndex,
                     PaStreamCallback* callback,
                     void* user_data)
{
    auto input_device_info = Pa_GetDeviceInfo(inputDeviceIndex);
    auto output_device_info = Pa_GetDeviceInfo(ouputDeviceIndex);

    PaStreamParameters inputParams;
    inputParams.device = inputDeviceIndex;
    inputParams.channelCount = input_device_info->maxInputChannels;
    inputParams.sampleFormat = paInt16;
    inputParams.suggestedLatency = input_device_info->defaultLowInputLatency;
    inputParams.hostApiSpecificStreamInfo = nullptr;

    PaStreamParameters outputParams;
    outputParams.device = ouputDeviceIndex;
    outputParams.channelCount = output_device_info->maxOutputChannels;
    outputParams.sampleFormat = paInt16;
    outputParams.suggestedLatency = output_device_info->defaultLowOutputLatency;
    outputParams.hostApiSpecificStreamInfo = nullptr;

    auto err = Pa_OpenStream(stream,
                             &inputParams,
                             &outputParams,
                             std::min(input_device_info->defaultSampleRate,
                                      input_device_info->defaultSampleRate),
                             paFramesPerBufferUnspecified,
                             paNoFlag,
                             callback,
                             user_data);

    if (err != paNoError)
        SIP_CORE_ERR("PortAudioLayer error : %s", Pa_GetErrorText(err));
}

bool
PortAudioLayer::PortAudioLayerImpl::initInputStream(PortAudioLayer& parent)
{
    SIP_CORE_INFO("[PortAudio Input] Opening PortAudio Input Stream");
    auto& stream = streams_[Direction::Input];
    auto apiIndex = getApiIndexByType(AudioDeviceType::CAPTURE);
    
    if (apiIndex != paNoDevice) {
        SIP_CORE_INFO("[PortAudio Input] Found input device at index %d", apiIndex);
        
        auto [ format, sample_rate ] = openStreamDevice(
            &streams_[Direction::Input],
            apiIndex,
            Direction::Input,
            [](const void* inputBuffer,
               void* outputBuffer,
               unsigned long framesPerBuffer,
               const PaStreamCallbackTimeInfo* timeInfo,
               PaStreamCallbackFlags statusFlags,
               void* userData) -> int {
                auto layer = static_cast<PortAudioLayer*>(userData);
                return layer->pimpl_->paInputCallback(*layer,
                                                      inputBuffer,
                                                      outputBuffer,
                                                      framesPerBuffer,
                                                      timeInfo,
                                                      statusFlags);
            },
            &parent);
        
        // Check if stream opened successfully
        if (format == 0 || sample_rate == 0.0) {
            SIP_CORE_ERR("[PortAudio Input] Failed to open input stream - format=%d, rate=%.0f",
                         format, sample_rate);
            return false;
        }
        
        // Store the actual format for use in callbacks
        inputStreamFormat_ = format;
        inputStreamSampleRate_ = sample_rate;
        
        SIP_CORE_INFO("[PortAudio Input] Stream opened successfully:");
        SIP_CORE_INFO("[PortAudio Input]   - PortAudio Format: %s (%d)", formatToString(format), format);
        SIP_CORE_INFO("[PortAudio Input]   - Sample Rate: %.0f Hz", sample_rate);
        SIP_CORE_INFO("[PortAudio Input]   - Channels: %d", parent.audioInputFormat_.nb_channels);
        
        // AudioFrame always uses AV_SAMPLE_FMT_S16 internally - we convert in the callback
        parent.audioInputFormat_.sampleFormat = AV_SAMPLE_FMT_S16;
        parent.audioInputFormat_.sample_rate = static_cast<unsigned int>(sample_rate);
        
        SIP_CORE_INFO("[PortAudio Input] AudioInputFormat set to: {rate=%d, channels=%d, fmt=S16}",
                     parent.audioInputFormat_.sample_rate,
                     parent.audioInputFormat_.nb_channels);
    } else {
        SIP_CORE_ERR("[PortAudio Input] Error: No valid input device (paNoDevice). There will be no mic.");
        return false;
    }

    SIP_CORE_INFO("[PortAudio Input] Starting PortAudio Input Stream");
    auto err = Pa_StartStream(stream);
    if (err != paNoError) {
        SIP_CORE_ERR("[PortAudio Input] Pa_StartStream error: %s", Pa_GetErrorText(err));
        return false;
    }

    SIP_CORE_INFO("[PortAudio Input] Input stream started successfully!");
    parent.recordChanged(true);
    return true;
}

bool
PortAudioLayer::PortAudioLayerImpl::initOutputStream(PortAudioLayer& parent, bool ringtone)
{
    SIP_CORE_INFO("[PortAudio Output] Opening PortAudio Output Stream (ringtone=%d)", ringtone);
    auto& stream = streams_[Direction::Output];
    auto apiIndex = getApiIndexByType(ringtone == false ? AudioDeviceType::PLAYBACK : AudioDeviceType::RINGTONE);
    
    if (apiIndex != paNoDevice) {
        SIP_CORE_INFO("[PortAudio Output] Found output device at index %d", apiIndex);
        
        auto [ format, sample_rate ] = openStreamDevice(
            &stream,
            apiIndex,
            Direction::Output,
            [](const void* inputBuffer,
               void* outputBuffer,
               unsigned long framesPerBuffer,
               const PaStreamCallbackTimeInfo* timeInfo,
               PaStreamCallbackFlags statusFlags,
               void* userData) -> int {
                auto layer = static_cast<PortAudioLayer*>(userData);
                return layer->pimpl_->paOutputCallback(*layer,
                                                       inputBuffer,
                                                       outputBuffer,
                                                       framesPerBuffer,
                                                       timeInfo,
                                                       statusFlags);
            },
            &parent);
        
        // Check if stream opened successfully
        if (format == 0 || sample_rate == 0.0) {
            SIP_CORE_ERR("[PortAudio Output] Failed to open output stream - format=%d, rate=%.0f",
                         format, sample_rate);
            return false;
        }
        
        // Store the actual format for use in callbacks
        outputStreamFormat_ = format;
        outputStreamSampleRate_ = sample_rate;
        
        SIP_CORE_INFO("[PortAudio Output] Stream opened successfully:");
        SIP_CORE_INFO("[PortAudio Output]   - PortAudio Format: %s (%d)", formatToString(format), format);
        SIP_CORE_INFO("[PortAudio Output]   - Sample Rate: %.0f Hz", sample_rate);
        SIP_CORE_INFO("[PortAudio Output]   - Channels: %d", parent.audioFormat_.nb_channels);
    } else {
        SIP_CORE_ERR("[PortAudio Output] Error: No valid output device (paNoDevice). There will be no sound.");
        return false;
    }

    SIP_CORE_INFO("[PortAudio Output] Starting PortAudio Output Stream");
    auto err = Pa_StartStream(stream);
    if (err != paNoError) {
        SIP_CORE_ERR("[PortAudio Output] Pa_StartStream error: %s", Pa_GetErrorText(err));
        return false;
    }

    SIP_CORE_INFO("[PortAudio Output] Output stream started successfully!");
    parent.playbackChanged(true);
    return true;
}

bool
PortAudioLayer::PortAudioLayerImpl::initFullDuplexStream(PortAudioLayer& parent)
{
    SIP_CORE_INFO("[PortAudio FullDuplex] Initializing full-duplex stream");
    
    auto apiIndexRecord = getApiIndexByType(AudioDeviceType::CAPTURE);
    auto apiIndexPlayback = getApiIndexByType(AudioDeviceType::PLAYBACK);
    
    SIP_CORE_INFO("[PortAudio FullDuplex] Record device index: %d, Playback device index: %d",
                  apiIndexRecord, apiIndexPlayback);
    
    if (apiIndexRecord == paNoDevice || apiIndexPlayback == paNoDevice) {
        SIP_CORE_ERR("[PortAudio FullDuplex] Error: Invalid input/output devices (paNoDevice). There will be no audio.");
        return false;
    }

    parent.dcblocker_.reset();

    SIP_CORE_INFO("[PortAudio FullDuplex] Opening PortAudio Full-duplex input/output stream");
    auto& stream = streams_[Direction::IO];
    
    // Full-duplex mode currently hardcoded to paInt16 - set the stream formats
    // Note: openFullDuplexStream doesn't support format fallback yet
    inputStreamFormat_ = paInt16;
    outputStreamFormat_ = paInt16;
    
    auto input_device_info = Pa_GetDeviceInfo(apiIndexRecord);
    auto output_device_info = Pa_GetDeviceInfo(apiIndexPlayback);
    
    if (input_device_info) {
        inputStreamSampleRate_ = input_device_info->defaultSampleRate;
        SIP_CORE_INFO("[PortAudio FullDuplex] Input device: %s, rate: %.0f Hz",
                      input_device_info->name, inputStreamSampleRate_);
    }
    if (output_device_info) {
        outputStreamSampleRate_ = output_device_info->defaultSampleRate;
        SIP_CORE_INFO("[PortAudio FullDuplex] Output device: %s, rate: %.0f Hz",
                      output_device_info->name, outputStreamSampleRate_);
    }
    
    openFullDuplexStream(
        &stream,
        apiIndexRecord,
        apiIndexPlayback,
        [](const void* inputBuffer,
           void* outputBuffer,
           unsigned long framesPerBuffer,
           const PaStreamCallbackTimeInfo* timeInfo,
           PaStreamCallbackFlags statusFlags,
           void* userData) -> int {
            auto layer = static_cast<PortAudioLayer*>(userData);
            return layer->pimpl_->paIOCallback(*layer,
                                               inputBuffer,
                                               outputBuffer,
                                               framesPerBuffer,
                                               timeInfo,
                                               statusFlags);
        },
        &parent);

    SIP_CORE_INFO("[PortAudio FullDuplex] Starting PortAudio I/O Streams");
    auto err = Pa_StartStream(stream);
    if (err != paNoError) {
        SIP_CORE_ERR("[PortAudio FullDuplex] Pa_StartStream error: %s", Pa_GetErrorText(err));
        return false;
    }

    SIP_CORE_INFO("[PortAudio FullDuplex] Full-duplex stream started successfully!");
    SIP_CORE_INFO("[PortAudio FullDuplex]   - Input Format: %s @ %.0f Hz",
                  formatToString(inputStreamFormat_), inputStreamSampleRate_);
    SIP_CORE_INFO("[PortAudio FullDuplex]   - Output Format: %s @ %.0f Hz",
                  formatToString(outputStreamFormat_), outputStreamSampleRate_);
    
    parent.recordChanged(true);
    parent.playbackChanged(true);
    return true;
}

int
PortAudioLayer::PortAudioLayerImpl::paOutputCallback(PortAudioLayer& parent,
                                                     const void* inputBuffer,
                                                     void* outputBuffer,
                                                     unsigned long framesPerBuffer,
                                                     const PaStreamCallbackTimeInfo* timeInfo,
                                                     PaStreamCallbackFlags statusFlags)
{
    // unused arguments
    (void) inputBuffer;
    (void) timeInfo;
    (void) statusFlags;

    // Log callback invocation periodically (every ~1000 calls to avoid spam)
    static unsigned long callCount = 0;
    if (++callCount % 1000 == 1) {
        SIP_CORE_DBG("[PortAudio Output CB] Processing %lu frames, format=%s, channels=%d",
                     framesPerBuffer, 
                     formatToString(outputStreamFormat_),
                     parent.audioFormat_.nb_channels);
    }

    auto toPlay = parent.getPlayback(parent.audioFormat_, framesPerBuffer);
    if (!toPlay) {
        // No audio to play - fill with silence in the appropriate format
        fillPaBufferWithSilence(outputBuffer,
                                framesPerBuffer,
                                parent.audioFormat_.nb_channels,
                                outputStreamFormat_);
        return paContinue;
    }

    // Convert from S16 (AudioFrame internal format) to PortAudio output format
    auto nFrames = toPlay->pointer()->nb_samples;
    convertInt16ToPaOutput(reinterpret_cast<const int16_t*>(toPlay->pointer()->extended_data[0]),
                           outputBuffer,
                           nFrames,
                           toPlay->pointer()->ch_layout.nb_channels,
                           outputStreamFormat_);
    return paContinue;
}

int
PortAudioLayer::PortAudioLayerImpl::paInputCallback(PortAudioLayer& parent,
                                                    const void* inputBuffer,
                                                    void* outputBuffer,
                                                    unsigned long framesPerBuffer,
                                                    const PaStreamCallbackTimeInfo* timeInfo,
                                                    PaStreamCallbackFlags statusFlags)
{
    // unused arguments
    (void) outputBuffer;
    (void) timeInfo;
    (void) statusFlags;

    if (framesPerBuffer == 0) {
        SIP_CORE_WARN("[PortAudio Input CB] No frames for input (framesPerBuffer=0).");
        return paContinue;
    }

    // Log callback invocation periodically (every ~1000 calls to avoid spam)
    static unsigned long callCount = 0;
    if (++callCount % 1000 == 1) {
        SIP_CORE_DBG("[PortAudio Input CB] Processing %lu frames, format=%s, channels=%d",
                     framesPerBuffer, 
                     formatToString(inputStreamFormat_),
                     parent.audioInputFormat_.nb_channels);
    }

    // Create AudioFrame with S16 format (internal standard)
    auto inBuff = std::make_shared<AudioFrame>(parent.audioInputFormat_, framesPerBuffer);
    
    if (parent.isCaptureMuted_) {
        libav_utils::fillWithSilence(inBuff->pointer());
    } else {
        // Convert from PortAudio format to S16 (AudioSample)
        // The conversion function handles all supported PortAudio formats
        convertPaInputToInt16(inputBuffer,
                              reinterpret_cast<int16_t*>(inBuff->pointer()->extended_data[0]),
                              framesPerBuffer,
                              parent.audioInputFormat_.nb_channels,
                              inputStreamFormat_);
    }
    
    parent.putRecorded(std::move(inBuff));
    return paContinue;
}

int
PortAudioLayer::PortAudioLayerImpl::paIOCallback(PortAudioLayer& parent,
                                                 const void* inputBuffer,
                                                 void* outputBuffer,
                                                 unsigned long framesPerBuffer,
                                                 const PaStreamCallbackTimeInfo* timeInfo,
                                                 PaStreamCallbackFlags statusFlags)
{
    paInputCallback(parent, inputBuffer, nullptr, framesPerBuffer, timeInfo, statusFlags);
    paOutputCallback(parent, nullptr, outputBuffer, framesPerBuffer, timeInfo, statusFlags);
    return paContinue;
}

} // namespace sip_core
