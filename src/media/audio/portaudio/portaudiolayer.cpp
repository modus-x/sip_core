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

#include <portaudio.h>
#include <algorithm>
#include <cmath>

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

    int paOutputCallback(PortAudioLayer& parent,
                         const AudioSample* inputBuffer,
                         AudioSample* outputBuffer,
                         unsigned long framesPerBuffer,
                         const PaStreamCallbackTimeInfo* timeInfo,
                         PaStreamCallbackFlags statusFlags);

    int paInputCallback(PortAudioLayer& parent,
                        const AudioSample* inputBuffer,
                        AudioSample* outputBuffer,
                        unsigned long framesPerBuffer,
                        const PaStreamCallbackTimeInfo* timeInfo,
                        PaStreamCallbackFlags statusFlags);

    int paIOCallback(PortAudioLayer& parent,
                     const AudioSample* inputBuffer,
                     AudioSample* outputBuffer,
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

    const double         requested_rate   = device_info->defaultSampleRate;
    const PaSampleFormat requested_format = paInt16;

    bool               found_exact_pair = false;
    bool               found_exact_fmt  = false;
    double             selected_rate    = 0.0;
    PaSampleFormat     selected_format  = 0;
    double             fallback_rate    = 0.0;
    PaSampleFormat     fallback_format  = 0;

    // 1) First pass: see if any combination of rate and fmt is what we trying to find
    for (const auto& [fmt, rate] : supportedCombinations) {
        SIP_CORE_INFO() << "PortAudioLayer: - Format: " << formatToString(fmt) 
                        << ", Rate: " << rate;

        if (rate == requested_rate && fmt == requested_format && !found_exact_pair) {
            selected_rate   = rate;
            selected_format = fmt;
            found_exact_pair = true;
            SIP_CORE_INFO() << "PortAudioLayer: Found exact rate match!";
        }

        // Meanwhile remember the first time we see the requested_format
        if (!found_exact_fmt && fmt == requested_format && !found_exact_pair) {
            fallback_format = fmt;
            fallback_rate   = rate;
            found_exact_fmt = true;
        }
    }

    // 2) If we didn�t find any entry at the requested_rate, try format == requested_format.
    if (!found_exact_pair) {
        if (found_exact_fmt) {
            selected_rate   = fallback_rate;
            selected_format = fallback_format;
        }
    }

    // 3) If we still have neither a matching rate nor a matching format, we cannot open.
    if (selected_format == 0 || selected_rate == 0.0) {
        SIP_CORE_WARN("PortAudioLayer: Neither requested sample rate (%.0f) nor "
                      "requested sample format (%s) is supported by device %d (%s). "
                      "Skipping stream open.",
                      requested_rate,
                      formatToString(requested_format),
                      device,
                      device_info->name);
        return { 0, 0.0 };
    }

    // Log which combination we�re actually going to use:
    SIP_CORE_INFO() << "PortAudioLayer: Selecting format " 
                    << formatToString(selected_format) 
                    << " @ rate " << selected_rate;

    PaStreamParameters params;
    params.device = device;
    params.channelCount = is_out ? device_info->maxOutputChannels
                                 : device_info->maxInputChannels;
    params.sampleFormat = selected_format;
    params.suggestedLatency = is_out ? device_info->defaultLowOutputLatency
                                     : device_info->defaultLowInputLatency;
    params.hostApiSpecificStreamInfo = nullptr;

    if (!is_out) {
        SIP_CORE_INFO() << "PortAudioLayer: Is format supported (input): "
                        << Pa_IsFormatSupported(&params, nullptr, selected_rate);
    }

    PaError err = Pa_OpenStream(stream,
                                is_out ? nullptr         : &params,
                                is_out ? &params         : nullptr,
                                selected_rate,
                                paFramesPerBufferUnspecified,
                                paNoFlag,
                                callback,
                                user_data);

    if (err != paNoError) {
        const char* errorText = Pa_GetErrorText(err);
        SIP_CORE_ERR("PortAudioLayer error: %s. Reporting it!", errorText);
        emitSignal<libsip_core::ConfigurationSignal::DeviceOpenError>(errorText, is_out);
    }

    return { selected_format, selected_rate };
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
    SIP_CORE_DBG("Open PortAudio Input Stream");
    auto& stream = streams_[Direction::Input];
    auto apiIndex = getApiIndexByType(AudioDeviceType::CAPTURE);
    if (apiIndex != paNoDevice) {
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
                                                      static_cast<const AudioSample*>(inputBuffer),
                                                      static_cast<AudioSample*>(outputBuffer),
                                                      framesPerBuffer,
                                                      timeInfo,
                                                      statusFlags);
            },
            &parent);
            parent.audioInputFormat_.sampleFormat = AV_SAMPLE_FMT_S16;
            parent.audioInputFormat_.sample_rate = sample_rate;
    } else {
        SIP_CORE_ERR("Error: No valid input device. There will be no mic.");
        return false;
    }

    SIP_CORE_DBG("Starting PortAudio Input Stream");
    auto err = Pa_StartStream(stream);
    if (err != paNoError) {
        SIP_CORE_ERR("PortAudioLayer error : %s", Pa_GetErrorText(err));
        return false;
    }

    parent.recordChanged(true);
    return true;
}

bool
PortAudioLayer::PortAudioLayerImpl::initOutputStream(PortAudioLayer& parent, bool ringtone)
{
    SIP_CORE_DBG("Open PortAudio Output Stream");
    auto& stream = streams_[Direction::Output];
    auto apiIndex = getApiIndexByType(ringtone == false ? AudioDeviceType::PLAYBACK : AudioDeviceType::RINGTONE);
    if (apiIndex != paNoDevice) {
        openStreamDevice(
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
                                                       static_cast<const AudioSample*>(inputBuffer),
                                                       static_cast<AudioSample*>(outputBuffer),
                                                       framesPerBuffer,
                                                       timeInfo,
                                                       statusFlags);
            },
            &parent);
    } else {
        SIP_CORE_ERR("Error: No valid output device. There will be no sound.");
        return false;
    }

    SIP_CORE_DBG("Starting PortAudio Output Stream");
    auto err = Pa_StartStream(stream);
    if (err != paNoError) {
        SIP_CORE_ERR("PortAudioLayer error : %s", Pa_GetErrorText(err));
        return false;
    }

    parent.playbackChanged(true);
    return true;
}

bool
PortAudioLayer::PortAudioLayerImpl::initFullDuplexStream(PortAudioLayer& parent)
{
    auto apiIndexRecord = getApiIndexByType(AudioDeviceType::CAPTURE);
    auto apiIndexPlayback = getApiIndexByType(AudioDeviceType::PLAYBACK);
    if (apiIndexRecord == paNoDevice || apiIndexPlayback == paNoDevice) {
        SIP_CORE_ERR("Error: Invalid input/output devices. There will be no audio.");
        return false;
    }

    parent.dcblocker_.reset();

    SIP_CORE_DBG("Open PortAudio Full-duplex input/output stream");
    auto& stream = streams_[Direction::IO];
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
                                               static_cast<const AudioSample*>(inputBuffer),
                                               static_cast<AudioSample*>(outputBuffer),
                                               framesPerBuffer,
                                               timeInfo,
                                               statusFlags);
        },
        &parent);

    SIP_CORE_DBG("Start PortAudio I/O Streams");
    auto err = Pa_StartStream(stream);
    if (err != paNoError) {
        SIP_CORE_ERR("PortAudioLayer error : %s", Pa_GetErrorText(err));
        return false;
    }

    parent.recordChanged(true);
    parent.playbackChanged(true);
    return true;
}

int
PortAudioLayer::PortAudioLayerImpl::paOutputCallback(PortAudioLayer& parent,
                                                     const AudioSample* inputBuffer,
                                                     AudioSample* outputBuffer,
                                                     unsigned long framesPerBuffer,
                                                     const PaStreamCallbackTimeInfo* timeInfo,
                                                     PaStreamCallbackFlags statusFlags)
{
    // unused arguments
    (void) inputBuffer;
    (void) timeInfo;
    (void) statusFlags;

    auto toPlay = parent.getPlayback(parent.audioFormat_, framesPerBuffer);
    if (!toPlay) {
        std::fill_n(outputBuffer, framesPerBuffer * parent.audioFormat_.nb_channels, 0);
        return paContinue;
    }

    auto nFrames = toPlay->pointer()->nb_samples * toPlay->pointer()->ch_layout.nb_channels;
    std::copy_n((AudioSample*) toPlay->pointer()->extended_data[0], nFrames, outputBuffer);
    return paContinue;
}

int
PortAudioLayer::PortAudioLayerImpl::paInputCallback(PortAudioLayer& parent,
                                                    const AudioSample* inputBuffer,
                                                    AudioSample* outputBuffer,
                                                    unsigned long framesPerBuffer,
                                                    const PaStreamCallbackTimeInfo* timeInfo,
                                                    PaStreamCallbackFlags statusFlags)
{
    // unused arguments
    (void) outputBuffer;
    (void) timeInfo;
    (void) statusFlags;

    if (framesPerBuffer == 0) {
        SIP_CORE_WARN("No frames for input.");
        return paContinue;
    }

    auto inBuff = std::make_shared<AudioFrame>(parent.audioInputFormat_, framesPerBuffer);
    auto nFrames = framesPerBuffer * parent.audioInputFormat_.nb_channels;
    if (parent.isCaptureMuted_)
        libav_utils::fillWithSilence(inBuff->pointer());
    else
        std::copy_n(inputBuffer, nFrames, (AudioSample*) inBuff->pointer()->extended_data[0]);
    parent.putRecorded(std::move(inBuff));
    return paContinue;
}

int
PortAudioLayer::PortAudioLayerImpl::paIOCallback(PortAudioLayer& parent,
                                                 const AudioSample* inputBuffer,
                                                 AudioSample* outputBuffer,
                                                 unsigned long framesPerBuffer,
                                                 const PaStreamCallbackTimeInfo* timeInfo,
                                                 PaStreamCallbackFlags statusFlags)
{
    paInputCallback(parent, inputBuffer, nullptr, framesPerBuffer, timeInfo, statusFlags);
    paOutputCallback(parent, nullptr, outputBuffer, framesPerBuffer, timeInfo, statusFlags);
    return paContinue;
}

} // namespace sip_core
