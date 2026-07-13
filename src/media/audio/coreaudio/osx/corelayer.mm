/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
 *  Author: Philippe Groarke <philippe.groarke@savoirfairelinux.com>
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

#include "corelayer.h"
#include "manager.h"
#include "audiodevice.h"
#include "device_signature.h"
#include <Accelerate/Accelerate.h>
#include <functional>

namespace sip_core {

dispatch_queue_t
audioConfigurationQueueMacOS()
{
    static dispatch_once_t queueCreationGuard;
    static dispatch_queue_t queue;
    dispatch_once(&queueCreationGuard, ^{
        queue = dispatch_queue_create("audioConfigurationQueueMacOS", DISPATCH_QUEUE_SERIAL);
    });
    return queue;
}

enum AVSampleFormat
getFormatFromStreamDescription(const AudioStreamBasicDescription& descr)
{
    if (descr.mFormatID == kAudioFormatLinearPCM) {
        BOOL isPlanar = descr.mFormatFlags & kAudioFormatFlagIsNonInterleaved;
        if (descr.mBitsPerChannel == 16) {
            if (descr.mFormatFlags & kAudioFormatFlagIsSignedInteger) {
                return isPlanar ? AV_SAMPLE_FMT_S16P : AV_SAMPLE_FMT_S16;
            }
        } else if (descr.mBitsPerChannel == 32) {
            if (descr.mFormatFlags & kAudioFormatFlagIsFloat) {
                return isPlanar ? AV_SAMPLE_FMT_FLTP : AV_SAMPLE_FMT_FLT;
            } else if (descr.mFormatFlags & kAudioFormatFlagIsSignedInteger) {
                return isPlanar ? AV_SAMPLE_FMT_S32P : AV_SAMPLE_FMT_S32;
            }
        }
    }
    NSLog(@"Unsupported core audio format");
    return AV_SAMPLE_FMT_NONE;
}

AudioFormat
audioFormatFromDescription(const AudioStreamBasicDescription& descr)
{
    return AudioFormat {static_cast<unsigned int>(descr.mSampleRate),
                        static_cast<unsigned int>(descr.mChannelsPerFrame),
                        getFormatFromStreamDescription(descr)};
}

static constexpr unsigned
streamTypeMask(AudioDeviceType type)
{
    switch (type) {
    case AudioDeviceType::PLAYBACK:
        return 1u << 0;
    case AudioDeviceType::CAPTURE:
        return 1u << 1;
    case AudioDeviceType::RINGTONE:
        return 1u << 2;
    case AudioDeviceType::ALL:
    default:
        return (1u << 0) | (1u << 1) | (1u << 2);
    }
}

static AudioDeviceType
effectiveStreamType(unsigned mask)
{
    // RINGTONE-only: route output to the dedicated ringtone device; nothing
    // consumes capture while ringing. Any other combination involves a call,
    // so configure both buses with the user-selected call devices.
    if (mask == streamTypeMask(AudioDeviceType::RINGTONE))
        return AudioDeviceType::RINGTONE;
    return AudioDeviceType::ALL;
}

// AudioLayer implementation.
CoreLayer::CoreLayer(const AudioPreference& pref)
    : AudioLayer(pref)
    , indexIn_(pref.getAlsaCardin())
    , indexOut_(pref.getAlsaCardout())
    , indexRing_(pref.getAlsaCardRingtone())
{}

CoreLayer::~CoreLayer()
{
    dispatch_sync(audioConfigurationQueueMacOS(), ^{
        if (status_ != Status::Started)
            return;
        destroyAudioLayer();
        flushUrgent();
        flushMain();
    });
}

std::vector<std::string>
CoreLayer::getCaptureDeviceList() const
{
    auto list = getDeviceList(true);
    std::vector<std::string> ret;
    ret.reserve(list.size());
    for (auto& x : list)
        ret.emplace_back(std::move(x.name_));
    return ret;
}

std::vector<std::string>
CoreLayer::getPlaybackDeviceList() const
{
    auto list = getDeviceList(false);
    std::vector<std::string> ret;
    ret.reserve(list.size());
    for (auto& x : list)
        ret.emplace_back(std::move(x.name_));
    return ret;
}

int
CoreLayer::getAudioDeviceIndex(const std::string& name, AudioDeviceType type) const
{
    int i = 0;
    for (const auto& device : getDeviceList(type == AudioDeviceType::CAPTURE)) {
        if (device.name_ == name)
            return i;
        i++;
    }
    return 0;
}

std::string
CoreLayer::getAudioDeviceName(int index, AudioDeviceType type) const
{
    return "";
}

void
CoreLayer::initAudioLayerIO(AudioDeviceType stream)
{
    // OS X uses Audio Units for output. Steps:
    // 1) Create a description.
    // 2) Find the audio unit that fits that.
    // 3) Set the audio unit callback.
    // 4) Initialize everything.
    // 5) Profit...
    SIP_CORE_DBG("INIT AUDIO IO");

    AudioUnitScope outputBus = 0;
    AudioUnitScope inputBus = 1;
    AudioComponentDescription desc = {0};
    desc.componentType = kAudioUnitType_Output;
    // kAudioOutputUnitProperty_EnableIO is ON and read-only
    // for input and output SCOPE on this subtype
    desc.componentSubType = kAudioUnitSubType_VoiceProcessingIO;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    desc.componentFlags = 0;
    desc.componentFlagsMask = 0;

    auto comp = AudioComponentFindNext(nullptr, &desc);
    if (comp == nullptr) {
        SIP_CORE_ERR("Can't find default output audio component.");
        return;
    }

    auto initError = AudioComponentInstanceNew(comp, &ioUnit_);
    if (initError) {
        checkErr(initError);
        return;
    }

    UInt32 size = sizeof(AudioDeviceID);
    if (stream == AudioDeviceType::CAPTURE || stream == AudioDeviceType::ALL) {
        auto captureList = getDeviceList(true);
        bool useFallbackDevice = true;

        // First, try to find the previously active device by name.
        // This handles hot-plug scenarios where indices shift but the
        // device name remains stable (matches PulseAudio/PortAudio behaviour).
        if (!captureDeviceName_.empty()) {
            for (size_t i = 0; i < captureList.size(); ++i) {
                if (captureList[i].name_ == captureDeviceName_) {
                    inputDeviceID_ = captureList[i].id_;
                    auto error = AudioUnitSetProperty(ioUnit_,
                                                      kAudioOutputUnitProperty_CurrentDevice,
                                                      kAudioUnitScope_Global,
                                                      inputBus,
                                                      &inputDeviceID_,
                                                      size);
                    if (error == kAudioServicesNoError) {
                        useFallbackDevice = false;
                        SIP_CORE_DBG("Capture device re-selected by name: %s",
                                     captureDeviceName_.c_str());
                    }
                    break;
                }
            }
        }

        // Fall back to index-based selection (first launch or name not found).
        if (useFallbackDevice && indexIn_ < captureList.size()) {
            inputDeviceID_ = captureList[indexIn_].id_;
            auto error = AudioUnitSetProperty(ioUnit_,
                                              kAudioOutputUnitProperty_CurrentDevice,
                                              kAudioUnitScope_Global,
                                              inputBus,
                                              &inputDeviceID_,
                                              size);
            if (error == kAudioServicesNoError) {
                useFallbackDevice = false;
                captureDeviceName_ = captureList[indexIn_].name_;
            }
        }

        // get a fallback capture device id so we could listen when the device disconnect.
        if (useFallbackDevice) {
            const AudioObjectPropertyAddress inputInfo = {kAudioHardwarePropertyDefaultInputDevice,
                                                          kAudioObjectPropertyScopeGlobal,
                                                          kAudioObjectPropertyElementMaster};
            auto status = AudioObjectGetPropertyData(kAudioObjectSystemObject,
                                                     &inputInfo,
                                                     0,
                                                     NULL,
                                                     &size,
                                                     &inputDeviceID_);
            if (status != kAudioServicesNoError) {
                SIP_CORE_ERR() << "failed to set audio input device";
                return;
            }
        }
    }

    if (stream == AudioDeviceType::PLAYBACK || stream == AudioDeviceType::ALL
        || stream == AudioDeviceType::RINGTONE) {
        auto playbackList = getDeviceList(false);
        auto index = stream == AudioDeviceType::RINGTONE ? indexRing_ : indexOut_;
        bool useFallbackDevice = true;

        // First, try to find the previously active device by name.
        if (!playbackDeviceName_.empty() && stream != AudioDeviceType::RINGTONE) {
            for (size_t i = 0; i < playbackList.size(); ++i) {
                if (playbackList[i].name_ == playbackDeviceName_) {
                    playbackDeviceID_ = playbackList[i].id_;
                    auto error = AudioUnitSetProperty(ioUnit_,
                                                      kAudioOutputUnitProperty_CurrentDevice,
                                                      kAudioUnitScope_Global,
                                                      outputBus,
                                                      &playbackDeviceID_,
                                                      size);
                    if (error == kAudioServicesNoError) {
                        useFallbackDevice = false;
                        SIP_CORE_DBG("Playback device re-selected by name: %s",
                                     playbackDeviceName_.c_str());
                    }
                    break;
                }
            }
        }

        // Fall back to index-based selection.
        if (useFallbackDevice && index < playbackList.size()) {
            playbackDeviceID_ = playbackList[index].id_;
            auto error = AudioUnitSetProperty(ioUnit_,
                                              kAudioOutputUnitProperty_CurrentDevice,
                                              kAudioUnitScope_Global,
                                              outputBus,
                                              &playbackDeviceID_,
                                              size);
            if (error == kAudioServicesNoError) {
                useFallbackDevice = false;
                if (stream != AudioDeviceType::RINGTONE)
                    playbackDeviceName_ = playbackList[index].name_;
            }
        }

        // get fallback output device id.
        if (useFallbackDevice) {
            const AudioObjectPropertyAddress outputInfo = {kAudioHardwarePropertyDefaultOutputDevice,
                                                           kAudioObjectPropertyScopeGlobal,
                                                           kAudioObjectPropertyElementMaster};
            auto status = AudioObjectGetPropertyData(kAudioObjectSystemObject,
                                                     &outputInfo,
                                                     0,
                                                     NULL,
                                                     &size,
                                                     &playbackDeviceID_);
            if (status != kAudioServicesNoError) {
                SIP_CORE_ERR() << "failed to set audio output device";
                return;
            }
        }
    }

    // add listener for detecting when devices are removed
    const AudioObjectPropertyAddress aliveAddress = {kAudioDevicePropertyDeviceIsAlive,
                                                     kAudioObjectPropertyScopeGlobal,
                                                     kAudioObjectPropertyElementMaster};
    AudioObjectAddPropertyListener(playbackDeviceID_, &aliveAddress, &deviceIsAliveCallback, this);
    AudioObjectAddPropertyListener(inputDeviceID_, &aliveAddress, &deviceIsAliveCallback, this);

    // add listener to detect when devices changed
    const AudioObjectPropertyAddress changedAddress = {kAudioHardwarePropertyDevices,
                                                       kAudioObjectPropertyScopeGlobal,
                                                       kAudioObjectPropertyElementMaster};
    AudioObjectAddPropertyListener(kAudioObjectSystemObject,
                                   &changedAddress,
                                   &devicesChangedCallback,
                                   this);

    // Set stream format
    AudioStreamBasicDescription info;
    size = sizeof(info);

    // get properties of stream that will be played from AU to playback device
    checkErr(AudioUnitGetProperty(ioUnit_,
                                  kAudioUnitProperty_StreamFormat,
                                  kAudioUnitScope_Output,
                                  outputBus,
                                  &info,
                                  &size));

    // save sample rate that will be used
    outSampleRate_ = info.mSampleRate;

    // get properties of stream that our app will send to AU
    checkErr(AudioUnitGetProperty(ioUnit_,
                                  kAudioUnitProperty_StreamFormat,
                                  kAudioUnitScope_Input,
                                  outputBus,
                                  &info,
                                  &size));
    audioFormat_ = {static_cast<unsigned int>(outSampleRate_),
                    static_cast<unsigned int>(info.mChannelsPerFrame),
                    getFormatFromStreamDescription(info)};
    outChannelsPerFrame_ = info.mChannelsPerFrame;
    info.mSampleRate = audioFormat_.sample_rate; // Only change sample rate.

    checkErr(AudioUnitSetProperty(ioUnit_,
                                  kAudioUnitProperty_StreamFormat,
                                  kAudioUnitScope_Input,
                                  outputBus,
                                  &info,
                                  size));

    hardwareFormatAvailable(audioFormat_);

    // Setup audio formats
    size = sizeof(AudioStreamBasicDescription);
    checkErr(AudioUnitGetProperty(ioUnit_,
                                  kAudioUnitProperty_StreamFormat,
                                  kAudioUnitScope_Input,
                                  inputBus,
                                  &info,
                                  &size));

    inSampleRate_ = info.mSampleRate;

    // Set format on output *SCOPE* in input *BUS*.
    checkErr(AudioUnitGetProperty(ioUnit_,
                                  kAudioUnitProperty_StreamFormat,
                                  kAudioUnitScope_Output,
                                  inputBus,
                                  &info,
                                  &size));

    audioInputFormat_ = {static_cast<unsigned int>(inSampleRate_),
                         static_cast<unsigned int>(info.mChannelsPerFrame),
                         getFormatFromStreamDescription(info)};
    hardwareInputFormatAvailable(audioInputFormat_);
    // Keep everything else and change only sample rate (or else SPLOSION!!!)
    info.mSampleRate = audioInputFormat_.sample_rate;
    // Keep some values to not ask them every time the read callback is fired up
    inChannelsPerFrame_ = info.mChannelsPerFrame;

    checkErr(AudioUnitSetProperty(ioUnit_,
                                  kAudioUnitProperty_StreamFormat,
                                  kAudioUnitScope_Output,
                                  inputBus,
                                  &info,
                                  size));

    // Input buffer setup. Note that ioData is empty and we have to store data
    // in another buffer.
    UInt32 bufferSizeFrames = 0;
    size = sizeof(UInt32);
    checkErr(AudioUnitGetProperty(ioUnit_,
                                  kAudioDevicePropertyBufferFrameSize,
                                  kAudioUnitScope_Global,
                                  outputBus,
                                  &bufferSizeFrames,
                                  &size));

    // Input callback setup.
    AURenderCallbackStruct inputCall;
    inputCall.inputProc = inputCallback;
    inputCall.inputProcRefCon = this;

    checkErr(AudioUnitSetProperty(ioUnit_,
                                  kAudioOutputUnitProperty_SetInputCallback,
                                  kAudioUnitScope_Global,
                                  inputBus,
                                  &inputCall,
                                  sizeof(AURenderCallbackStruct)));

    // Output callback setup.
    AURenderCallbackStruct callback;
    callback.inputProc = outputCallback;
    callback.inputProcRefCon = this;

    checkErr(AudioUnitSetProperty(ioUnit_,
                                  kAudioUnitProperty_SetRenderCallback,
                                  kAudioUnitScope_Global,
                                  outputBus,
                                  &callback,
                                  sizeof(AURenderCallbackStruct)));
}

bool
CoreLayer::startAudioUnit()
{
    status_ = Status::Started;

    const auto effective = effectiveStreamType(activeStreamMask_);
    initAudioLayerIO(effective);
    // An ALL-configured unit has both buses set up with the call devices and
    // can serve every stream type, so later startStream() calls for other
    // types need no rebuild. A RINGTONE-only unit serves only ringing.
    configuredStreamMask_ = effective == AudioDeviceType::ALL
                                ? streamTypeMask(AudioDeviceType::ALL)
                                : activeStreamMask_;

    auto inputError = AudioUnitInitialize(ioUnit_);
    auto outputError = AudioOutputUnitStart(ioUnit_);
    if (inputError || outputError) {
        // Deliberate state on failure: status_ goes back to Idle while
        // activeStreamMask_ keeps the requested types. Manager's guards
        // still hold those types, so the next startStream() retries the
        // build and the eventual stopStream() drains the mask normally.
        // Do not "fix" by clearing the mask here — that would make a
        // retry impossible and desync the guard refcounts.
        status_ = Status::Idle;
        destroyAudioLayer();
        deviceSignatureHash_.store(kNoDeviceSignature, std::memory_order_release);
        restartingAudio_ = false;
        return false;
    }

    // Snapshot the user-visible device topology *after* the AudioUnit
    // is up. VoiceProcessingIO has by now created its internal
    // VPAUAggregateAudioDevice — getDeviceList() filters that out, so
    // the resulting hash represents only what the user actually sees.
    // devicesChangedCallback uses it to suppress spurious change
    // notifications fired by VPAggregate lifecycle events that arrive
    // after restartingAudio_ has been cleared (the main contributor to
    // the 3-5 s audio startup delay). The hash is a single atomic so
    // the HAL listener thread can read it without further locking.
    deviceSignatureHash_.store(computeDeviceSignatureHash(),
                               std::memory_order_release);

    restartingAudio_ = false;

    // Wire the capture/playback lifecycle into AudioLayer so the shared
    // audioProcessor is created and fed on macOS too (every other backend does
    // this in its run/start path). Without it, AudioLayer::putRecorded's
    // `audioProcessor && playbackStarted_ && recordStarted_` gate is permanently
    // false: the host's mic frames bypass voice-activity detection and reach the
    // ring buffer with has_voice=false forever, so the conference "speaking"
    // indicator never lights for the local host (remote indicators are computed
    // by the independent AudioReceiveThread and are unaffected).
    playbackChanged(true);
    recordChanged(true);
    return true;
}

void
CoreLayer::startStream(AudioDeviceType stream)
{
    // Set the guard BEFORE dispatch so that any devicesChangedCallback firing
    // on the system thread (e.g. from VoiceProcessingIO creating its internal
    // VPAUAggregateAudioDevice) is suppressed during the restart window.
    restartingAudio_ = true;

    dispatch_async(audioConfigurationQueueMacOS(), ^{
        SIP_CORE_DBG("START STREAM [type=%d]", (int) stream);

        activeStreamMask_ |= streamTypeMask(stream);

        if (status_ == Status::Started) {
            if ((activeStreamMask_ & ~configuredStreamMask_) == 0) {
                // Unit already serves every requested type.
                restartingAudio_ = false;
                return;
            }
            // The unit is running but was configured for a narrower scope —
            // typically RINGTONE-only ringing while a just-answered call now
            // needs the capture device and the call playback device. Rebuild
            // with the widened scope.
            SIP_CORE_DBG("START STREAM: rebuilding audio unit for wider scope (mask=0x%x)",
                         activeStreamMask_);
            destroyAudioLayer();
            status_ = Status::Idle;
        }

        if (status_ != Status::Idle) {
            restartingAudio_ = false;
            return;
        }

        startAudioUnit();
    });
}

void
CoreLayer::destroyAudioLayer()
{
    // Remove property listeners to prevent callbacks during/after teardown
    // and to avoid listener accumulation across restart cycles.
    const AudioObjectPropertyAddress aliveAddress = {kAudioDevicePropertyDeviceIsAlive,
                                                     kAudioObjectPropertyScopeGlobal,
                                                     kAudioObjectPropertyElementMaster};
    if (playbackDeviceID_)
        AudioObjectRemovePropertyListener(playbackDeviceID_, &aliveAddress, &deviceIsAliveCallback, this);
    if (inputDeviceID_)
        AudioObjectRemovePropertyListener(inputDeviceID_, &aliveAddress, &deviceIsAliveCallback, this);

    const AudioObjectPropertyAddress changedAddress = {kAudioHardwarePropertyDevices,
                                                       kAudioObjectPropertyScopeGlobal,
                                                       kAudioObjectPropertyElementMaster};
    AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &changedAddress, &devicesChangedCallback, this);

    AudioOutputUnitStop(ioUnit_);
    AudioUnitUninitialize(ioUnit_);
    AudioComponentInstanceDispose(ioUnit_);

    inputDeviceID_ = 0;
    playbackDeviceID_ = 0;
    configuredStreamMask_ = 0;
    deviceSignatureHash_.store(kNoDeviceSignature, std::memory_order_release);

    // Paired teardown for the recordChanged/playbackChanged wiring added in
    // startAudioUnit(): destroy the audioProcessor in lockstep with the
    // VoiceProcessingIO unit so it is recreated cleanly on the next rebuild.
    playbackChanged(false);
    recordChanged(false);
}

void
CoreLayer::stopStream(AudioDeviceType stream)
{
    dispatch_async(audioConfigurationQueueMacOS(), ^{
        SIP_CORE_DBG("STOP STREAM [type=%d]", (int) stream);

        activeStreamMask_ &= ~streamTypeMask(stream);

        // Drop leftover tone/ringtone samples in any case.
        flushUrgent();

        if (status_ != Status::Started)
            return;

        if (activeStreamMask_ != 0) {
            // Other stream types still depend on the single full-duplex
            // VoiceProcessingIO unit — e.g. an answered call's CAPTURE /
            // PLAYBACK when the RINGTONE guard lingers out 750 ms after
            // pickup. Tearing the unit down here used to silence the whole
            // first call (no capture, no playback) until the audio layer was
            // recreated by a manual device switch in settings.
            SIP_CORE_DBG("STOP STREAM: unit kept alive, remaining mask=0x%x", activeStreamMask_);
            return;
        }

        status_ = Status::Idle;
        destroyAudioLayer();
        flushMain();
    });
}

void
CoreLayer::restartStream()
{
    restartingAudio_ = true;

    dispatch_async(audioConfigurationQueueMacOS(), ^{
        SIP_CORE_DBG("RESTART STREAM (mask=0x%x)", activeStreamMask_);

        if (status_ == Status::Started) {
            destroyAudioLayer();
            status_ = Status::Idle;
        }
        flushUrgent();
        flushMain();

        if (activeStreamMask_ == 0) {
            // No guard holds any stream type — nothing to bring back up.
            restartingAudio_ = false;
            return;
        }

        startAudioUnit();
    });
}

//// PRIVATE /////

OSStatus
CoreLayer::deviceIsAliveCallback(AudioObjectID inObjectID,
                                 UInt32 inNumberAddresses,
                                 const AudioObjectPropertyAddress inAddresses[],
                                 void* inRefCon)
{
    auto* self = static_cast<CoreLayer*>(inRefCon);
    if (self->status_ != Status::Started)
        return kAudioServicesNoError;
    if (self->restartingAudio_.load())
        return kAudioServicesNoError;
    // Defence in depth: if VoiceProcessingIO momentarily flickers the
    // underlying device's alive flag while spinning up VPAggregate, the
    // user-visible device set is unchanged. Skip the restart in that case.
    auto stored = self->deviceSignatureHash_.load(std::memory_order_acquire);
    if (stored != kNoDeviceSignature && stored == self->computeDeviceSignatureHash())
        return kAudioServicesNoError;
    self->restartStream();
    return kAudioServicesNoError;
}

OSStatus
CoreLayer::devicesChangedCallback(AudioObjectID inObjectID,
                                  UInt32 inNumberAddresses,
                                  const AudioObjectPropertyAddress inAddresses[],
                                  void* inRefCon)
{
    auto* self = static_cast<CoreLayer*>(inRefCon);
    if (self->status_ != Status::Started)
        return kAudioServicesNoError;
    // Skip if we are already in the middle of a restart — VoiceProcessingIO
    // creates/destroys a VPAUAggregateAudioDevice which fires this callback.
    if (self->restartingAudio_.load())
        return kAudioServicesNoError;
    // Snapshot-based filter: VPAggregate creation/destruction during AU
    // lifecycle can fire kAudioHardwarePropertyDevices changes *after*
    // restartingAudio_ has been cleared. Comparing the user-visible device
    // signatures collapses those spurious events into a no-op and avoids
    // the cascading stopStream/startStream/recoverAudioDevices() restart
    // chain that previously produced a 3-5 s audio startup delay.
    auto stored = self->deviceSignatureHash_.load(std::memory_order_acquire);
    if (stored != kNoDeviceSignature && stored == self->computeDeviceSignatureHash())
        return kAudioServicesNoError;
    // Real device topology change — restart the audio stream so the
    // AudioUnit reinitialises with the current set of devices.
    self->restartStream();
    self->devicesChanged();
    return kAudioServicesNoError;
}

OSStatus
CoreLayer::outputCallback(void* inRefCon,
                          AudioUnitRenderActionFlags* ioActionFlags,
                          const AudioTimeStamp* inTimeStamp,
                          UInt32 inBusNumber,
                          UInt32 inNumberFrames,
                          AudioBufferList* ioData)
{
    static_cast<CoreLayer*>(inRefCon)->write(ioActionFlags,
                                             inTimeStamp,
                                             inBusNumber,
                                             inNumberFrames,
                                             ioData);
    return kAudioServicesNoError;
}

void
CoreLayer::write(AudioUnitRenderActionFlags* ioActionFlags,
                 const AudioTimeStamp* inTimeStamp,
                 UInt32 inBusNumber,
                 UInt32 inNumberFrames,
                 AudioBufferList* ioData)
{
    float desiredGain = playbackGain_;

    auto format = audioFormat_;
    format.sample_rate = outSampleRate_;
    format.nb_channels = outChannelsPerFrame_;
    format.sampleFormat = AV_SAMPLE_FMT_FLTP;
    if (auto toPlay = getPlayback(format, inNumberFrames)) {
        for (int i = 0; i < format.nb_channels; ++i) {
            // adjust volume
            vDSP_vsmul((float*) toPlay->pointer()->extended_data[i],
                       1,
                       &desiredGain,
                       (float*) toPlay->pointer()->extended_data[i],
                       1,
                       inNumberFrames);
            std::copy_n((Float32*) toPlay->pointer()->extended_data[i],
                        inNumberFrames,
                        (Float32*) ioData->mBuffers[i].mData);
        }
    } else {
        for (int i = 0; i < format.nb_channels; ++i)
            std::fill_n(reinterpret_cast<Float32*>(ioData->mBuffers[i].mData), inNumberFrames, 0);
    }
}

OSStatus
CoreLayer::inputCallback(void* inRefCon,
                         AudioUnitRenderActionFlags* ioActionFlags,
                         const AudioTimeStamp* inTimeStamp,
                         UInt32 inBusNumber,
                         UInt32 inNumberFrames,
                         AudioBufferList* ioData)
{
    static_cast<CoreLayer*>(inRefCon)->read(ioActionFlags,
                                            inTimeStamp,
                                            inBusNumber,
                                            inNumberFrames,
                                            ioData);
    return kAudioServicesNoError;
}

void
CoreLayer::read(AudioUnitRenderActionFlags* ioActionFlags,
                const AudioTimeStamp* inTimeStamp,
                UInt32 inBusNumber,
                UInt32 inNumberFrames,
                AudioBufferList* ioData)
{
    if (inNumberFrames <= 0) {
        SIP_CORE_WARN("No frames for input.");
        return;
    }

    auto format = audioInputFormat_;
    format.sampleFormat = AV_SAMPLE_FMT_FLTP;
    auto inBuff = std::make_shared<AudioFrame>(format, inNumberFrames);

    AudioBufferList buffer;
    UInt32 bufferSize = inNumberFrames * sizeof(Float32);
    buffer.mNumberBuffers = inChannelsPerFrame_;
    for (UInt32 i = 0; i < buffer.mNumberBuffers; ++i) {
        buffer.mBuffers[i].mNumberChannels = 1;
        buffer.mBuffers[i].mDataByteSize = bufferSize;
        buffer.mBuffers[i].mData = inBuff->pointer()->extended_data[i];
    }

    if (isCaptureMuted_) {
        libav_utils::fillWithSilence(inBuff->pointer());
    } else {
        // Write the mic samples in our buffer
        checkErr(AudioUnitRender(ioUnit_,
                                 ioActionFlags,
                                 inTimeStamp,
                                 inBusNumber,
                                 inNumberFrames,
                                 &buffer));
        float desiredGain = captureGain_;
        for (UInt32 bufferIndex = 0; bufferIndex < inChannelsPerFrame_; ++bufferIndex) {
            float* rawBuffer = (float*) buffer.mBuffers[bufferIndex].mData;
            vDSP_vsmul(rawBuffer, 1, &desiredGain, rawBuffer, 1, inNumberFrames);
        }
    }

    putRecorded(std::move(inBuff));
}

void
CoreLayer::updatePreference(AudioPreference& preference, int index, AudioDeviceType type)
{
    switch (type) {
    case AudioDeviceType::ALL:
    case AudioDeviceType::PLAYBACK:
        preference.setAlsaCardout(index);
        break;

    case AudioDeviceType::CAPTURE:
        preference.setAlsaCardin(index);
        break;

    case AudioDeviceType::RINGTONE:
        preference.setAlsaCardRingtone(index);
        break;

    default:
        break;
    }
}

std::vector<AudioDevice>
CoreLayer::getDeviceList(bool getCapture) const
{
    std::vector<AudioDevice> ret;
    UInt32 propsize;

    AudioObjectPropertyAddress theAddress = {kAudioHardwarePropertyDevices,
                                             kAudioObjectPropertyScopeGlobal,
                                             kAudioObjectPropertyElementMaster};

    __Verify_noErr(
        AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &theAddress, 0, nullptr, &propsize));

    std::size_t nDevices = propsize / sizeof(AudioDeviceID);
    auto devids = std::vector<AudioDeviceID>(nDevices);

    __Verify_noErr(AudioObjectGetPropertyData(kAudioObjectSystemObject,
                                              &theAddress,
                                              0,
                                              nullptr,
                                              &propsize,
                                              devids.data()));

    for (int i = 0; i < nDevices; ++i) {
        auto dev = AudioDevice {devids[i], getCapture};
        if (dev.channels_ > 0) { // Channels < 0 if inactive.
            // VoiceProcessingIO creates an internal aggregate device — keep
            // the filter in lockstep with the device-change snapshot logic
            // (see device_signature.h) so notifications about VPAggregate
            // appearing/disappearing don't show up as a topology change.
            if (coreaudio::shouldIgnoreDeviceName(dev.name_))
                continue;
            // for input device check if it not speaker
            // since the speaker device has input stream for echo cancellation.
            if (getCapture) {
                auto devOutput = AudioDevice {devids[i], !getCapture};
                // it is output device
                if (devOutput.channels_ > 0) {
                    continue;
                }
            }
            ret.push_back(std::move(dev));
        }
    }
    return ret;
}

std::uint64_t
CoreLayer::computeDeviceSignatureHash() const
{
    auto sig = coreaudio::makeDeviceSignature(getCaptureDeviceList(), getPlaybackDeviceList());
    auto h = std::hash<std::string> {}(sig);
    // Force the low bit so a legitimate hash can never collide with the
    // kNoDeviceSignature sentinel (0).
    return static_cast<std::uint64_t>(h) | 1ULL;
}

} // namespace sip_core
