/*
 *  Copyright (C) 2004-2023 Savoir-faire Linux Inc.
 *
 *  Author: Philippe Groarke <philippe.groarke@savoirfairelinux.com>
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

#include "corelayer.h"
#include "manager.h"
#include <AVFoundation/AVAudioSession.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

const int MAX_IO_INIT_TRIES = 5;

enum AVSampleFormat
getFormatFromStreamDescription(const AudioStreamBasicDescription& descr) {
    if(descr.mFormatID == kAudioFormatLinearPCM) {
        BOOL isPlanar = descr.mFormatFlags & kAudioFormatFlagIsNonInterleaved;
        if(descr.mBitsPerChannel == 16) {
            if(descr.mFormatFlags & kAudioFormatFlagIsSignedInteger) {
                return isPlanar ? AV_SAMPLE_FMT_S16P : AV_SAMPLE_FMT_S16;
            }
        }
        else if(descr.mBitsPerChannel == 32) {
            if(descr.mFormatFlags & kAudioFormatFlagIsFloat) {
                return isPlanar ? AV_SAMPLE_FMT_FLTP : AV_SAMPLE_FMT_FLT;
            }
            else if(descr.mFormatFlags & kAudioFormatFlagIsSignedInteger) {
                return isPlanar ? AV_SAMPLE_FMT_S32P : AV_SAMPLE_FMT_S32;
            }
        }
    }
    NSLog(@"Unsupported core audio format");
    return AV_SAMPLE_FMT_NONE;
}

sip_core::AudioFormat
audioFormatFromDescription(const AudioStreamBasicDescription& descr) {
    return sip_core::AudioFormat {static_cast<unsigned int>(descr.mSampleRate),
                        static_cast<unsigned int>(descr.mChannelsPerFrame),
                        getFormatFromStreamDescription(descr)};
}


namespace sip_core {
dispatch_queue_t
audioConfigurationQueueIOS()
{
    static dispatch_once_t queueCreationGuard;
    static dispatch_queue_t queue;
    dispatch_once(&queueCreationGuard, ^{
        queue = dispatch_queue_create("audioConfigurationQueueIOS", DISPATCH_QUEUE_SERIAL);
    });
    return queue;
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

// AudioLayer implementation.
CoreLayer::CoreLayer(const AudioPreference& pref)
    : AudioLayer(pref)
    , indexIn_(pref.getAlsaCardin())
    , indexOut_(pref.getAlsaCardout())
    , indexRing_(pref.getAlsaCardRingtone())
    , playbackBuff_(0, audioFormat_)
{
    audioConfigurationQueue = dispatch_queue_create("ru.svetets.audioConfigurationQueueIOS",
                                                    DISPATCH_QUEUE_SERIAL);
}

CoreLayer::~CoreLayer()
{
    stopStream(AudioDeviceType::ALL);
}

std::vector<std::string>
CoreLayer::getCaptureDeviceList() const
{
    std::vector<std::string> ret;
    return ret;
}

std::vector<std::string>
CoreLayer::getPlaybackDeviceList() const
{
    std::vector<std::string> ret;
    // No need to enumerate devices for iOS.
    // The notion of input devices can be ignored, and output devices can describe
    // input/output pairs.
    // Unavailable options like the receiver on iPad can be ignored by the client.
    ret.assign({"built_in_spk", "bluetooth", "headphones", "receiver"});

    return ret;
}

int
CoreLayer::getAudioDeviceIndex(const std::string& name, AudioDeviceType type) const
{
    (void) name;
    (void) index;
    (void) type;
    return 0;
}

std::string
CoreLayer::getAudioDeviceName(int index, AudioDeviceType type) const
{
    (void) index;
    (void) type;
    return "";
}

bool
CoreLayer::initAudioLayerIO()
{
    SIP_CORE_DBG("iOS CoreLayer initAudioLayerIO started");

    AudioComponentDescription outputUnitDescription;
    outputUnitDescription.componentType = kAudioUnitType_Output;
    outputUnitDescription.componentSubType = kAudioUnitSubType_VoiceProcessingIO;
    outputUnitDescription.componentManufacturer = kAudioUnitManufacturer_Apple;
    outputUnitDescription.componentFlags = 0;
    outputUnitDescription.componentFlagsMask = 0;

    auto comp = AudioComponentFindNext(nullptr, &outputUnitDescription);
    if (comp == nullptr) {
        SIP_CORE_ERR("iOS CoreLayer initAudioLayerIO - can't find default output audio component.");
        return false;
    }

    checkErr(AudioComponentInstanceNew(comp, &ioUnit_));

    NSError* error = nil;
    AVAudioSessionCategory audioCategory = AVAudioSessionCategoryPlayAndRecord;
    AVAudioSessionMode mode = AVAudioSessionModeVoiceChat;
    AVAudioSessionCategoryOptions options = AVAudioSessionCategoryOptionAllowBluetooth;
    [[AVAudioSession sharedInstance] setCategory:audioCategory
                                            mode:mode
                                         options:options
                                           error:&error];
    if (error) {
        const char* errorDesc = [[error localizedDescription] UTF8String];
        SIP_CORE_ERR() << "iOS CoreLayer initAudioLayerIO cannot setCategory: error code "
                       << int([error code]) << ", description: " << errorDesc;
        return false;
    }

    auto playBackDeviceList = getPlaybackDeviceList();
    SIP_CORE_DBG("iOS CoreLayer initAudioLayerIO setting playback device: %s",
                 playBackDeviceList[indexOut_].c_str());
    switch (indexOut_) {
    case 0:
        [[AVAudioSession sharedInstance] overrideOutputAudioPort:AVAudioSessionPortOverrideSpeaker
                                                           error:nil];
        break;
    case 1:
    case 2:
        break;
    case 3:
        [[AVAudioSession sharedInstance] overrideOutputAudioPort:AVAudioSessionPortOverrideNone
                                                           error:nil];
        break;
    default:
        break;
    }
    setupOutputBus();
    setupInputBus();
    bindCallbacks();
    SIP_CORE_DBG("iOS CoreLayer initAudioLayerIO finished");

    return true;
}

void
CoreLayer::setupOutputBus()
{
    SIP_CORE_DBG("iOS CoreLayer - initializing output bus STARTED");

    AudioUnitScope outputBus = 0;
    UInt32 size;

    AudioStreamBasicDescription outputASBD;
    size = sizeof(outputASBD);

    Float64 outSampleRate;
    size = sizeof(outSampleRate);
    AudioSessionGetProperty(kAudioSessionProperty_CurrentHardwareSampleRate, &size, &outSampleRate);
    outputASBD.mSampleRate = outSampleRate;
    outSampleRate_ = outputASBD.mSampleRate;

    size = sizeof(outputASBD);
    checkErr(AudioUnitGetProperty(ioUnit_,
                                  kAudioUnitProperty_StreamFormat,
                                  kAudioUnitScope_Input,
                                  outputBus,
                                  &outputASBD,
                                  &size));

    // Only change sample rate.
    outputASBD.mSampleRate = outSampleRate_;
    outputASBD.mFormatID = kAudioFormatLinearPCM;
    outputASBD.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;

    outSampleRate_ = outputASBD.mSampleRate;
    outChannelsPerFrame_ = outputASBD.mChannelsPerFrame;

    // Set output steam format
    checkErr(AudioUnitSetProperty(ioUnit_,
                                  kAudioUnitProperty_StreamFormat,
                                  kAudioUnitScope_Input,
                                  outputBus,
                                  &outputASBD,
                                  size));

    hardwareFormatAvailable({static_cast<unsigned int>(outputASBD.mSampleRate),
                             static_cast<unsigned int>(outputASBD.mChannelsPerFrame)});
    SIP_CORE_DBG("iOS CoreLayer - initializing output bus FINISHED");
}

void
CoreLayer::setupInputBus()
{
    SIP_CORE_DBG("iOS CoreLayer - initializing input bus STARTED");

    AudioUnitScope inputBus = 1;
    UInt32 size;

    AudioStreamBasicDescription inputASBD;
    size = sizeof(inputASBD);

    // Enable input
    UInt32 flag = 1;
    checkErr(AudioUnitSetProperty(ioUnit_,
                                  kAudioOutputUnitProperty_EnableIO,
                                  kAudioUnitScope_Input,
                                  inputBus,
                                  &flag,
                                  sizeof(flag)));

    // Setup audio formats
    checkErr(AudioUnitGetProperty(ioUnit_,
                                  kAudioUnitProperty_StreamFormat,
                                  kAudioUnitScope_Input,
                                  inputBus,
                                  &inputASBD,
                                  &size));

    AVAudioSession* session = [AVAudioSession sharedInstance];
    // Replace AudioSessionGetProperty with AVAudioSession
    Float64 inSampleRate = session.sampleRate;

    inputASBD.mSampleRate = inSampleRate;
    inputASBD.mFormatID = kAudioFormatLinearPCM;
    inputASBD.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;

    // Set format on output *SCOPE* in input *BUS*.
    checkErr(AudioUnitGetProperty(ioUnit_,
                                  kAudioUnitProperty_StreamFormat,
                                  kAudioUnitScope_Output,
                                  inputBus,
                                  &inputASBD,
                                  &size));
    inputASBD.mSampleRate = inSampleRate;
    audioInputFormat_ = {static_cast<unsigned int>(inputASBD.mSampleRate),
                         static_cast<unsigned int>(inputASBD.mChannelsPerFrame),
                         getFormatFromStreamDescription(inputASBD)};
    hardwareInputFormatAvailable(audioInputFormat_);

    // Keep some values to not ask them every time the read callback is fired up
    inSampleRate_ = inputASBD.mSampleRate;
    inChannelsPerFrame_ = inputASBD.mChannelsPerFrame;

    size = sizeof(inputASBD);
    checkErr(AudioUnitSetProperty(ioUnit_,
                                  kAudioUnitProperty_StreamFormat,
                                  kAudioUnitScope_Output,
                                  inputBus,
                                  &inputASBD,
                                  size));

    // Input buffer setup. Note that ioData is empty and we have to store data
    // in another buffer.
    flag = 0;
    AudioUnitSetProperty(ioUnit_,
                         kAudioUnitProperty_ShouldAllocateBuffer,
                         kAudioUnitScope_Output,
                         inputBus,
                         &flag,
                         sizeof(flag));
}

void
CoreLayer::bindCallbacks()
{
    AURenderCallbackStruct callback;
    AudioUnitScope outputBus = 0;
    AudioUnitScope inputBus = 1;

    // Output callback setup
    callback.inputProc = outputCallback;
    callback.inputProcRefCon = this;

    checkErr(AudioUnitSetProperty(ioUnit_,
                                  kAudioUnitProperty_SetRenderCallback,
                                  kAudioUnitScope_Global,
                                  outputBus,
                                  &callback,
                                  sizeof(AURenderCallbackStruct)));

    // Input callback setup
    AURenderCallbackStruct inputCall;
    inputCall.inputProc = inputCallback;
    inputCall.inputProcRefCon = this;

    checkErr(AudioUnitSetProperty(ioUnit_,
                                  kAudioOutputUnitProperty_SetInputCallback,
                                  kAudioUnitScope_Global,
                                  inputBus,
                                  &inputCall,
                                  sizeof(AURenderCallbackStruct)));
}

// called by core to start audio stream; the type is tracked in
// activeStreamMask_ so stopStream() knows when the last user is gone
void
CoreLayer::startStream(AudioDeviceType stream)
{
    dispatch_async(audioConfigurationQueueIOS(), ^{
        SIP_CORE_DBG("iOS CoreLayer startStream [type=%d]", (int) stream);
        const std::lock_guard<std::mutex> lock(layerLock_);

        activeStreamMask_ |= streamTypeMask(stream);

        // if started, exit — the PlayAndRecord unit already serves
        // every stream type, no per-type reconfiguration is needed
        if (status_ == Status::Started) {
            SIP_CORE_DBG("iOS CoreLayer startStream already started, exiting");
            return;
        }

        // if idle, configure
        if (status_ == Status::Idle) {
            SIP_CORE_DBG("iOS CoreLayer startStream layer is not configured, fix it");
            bool result = initAudioLayerIO();

            if (result) {
                SIP_CORE_DBG("iOS CoreLayer startStream initAudioLayer OK");
            } else {
                SIP_CORE_ERR("iOS CoreLayer startStream initAudioLayer ERROR, exiting");
                return;
            }

            status_ = Status::Configured;
        }

        // if starting, try to load ioUnit
        if (AudioUnitInitialize(ioUnit_) || AudioOutputUnitStart(ioUnit_)) {
            SIP_CORE_ERR("iOS CoreLayer - startStream ERROR! Deleting ioUnit_, exiting");
            AudioOutputUnitStop(ioUnit_);
            AudioUnitUninitialize(ioUnit_);
            AudioComponentInstanceDispose(ioUnit_);
            status_ = Status::Idle;
            return;
        } else {
            SIP_CORE_DBG("iOS CoreLayer - startStream OK");
        }

        status_ = Status::Started;
    });
}

void
CoreLayer::destroyAudioLayer()
{
    const std::lock_guard<std::mutex> lock(layerLock_);

    // if not started, exit
    if (status_ == Status::Idle) {
        SIP_CORE_DBG("iOS CoreLayer - destroyAudioLayer: core layer is in idle state, exiting");
        return;
    }

    AudioOutputUnitStop(ioUnit_);
    AudioUnitUninitialize(ioUnit_);
    AudioComponentInstanceDispose(ioUnit_);
    status_ = Status::Idle;
    SIP_CORE_DBG("iOS CoreLayer - destroyAudioLayer now in idle state");
}

void
CoreLayer::stopStream(AudioDeviceType stream)
{
    // dispatch_sync (not async): ~CoreLayer() relies on stopStream(ALL)
    // having fully torn the unit down before the object is destroyed.
    dispatch_sync(audioConfigurationQueueIOS(), ^{
        SIP_CORE_DBG("iOS CoreLayer stopStream [type=%d]", (int) stream);

        activeStreamMask_ &= ~streamTypeMask(stream);

        // Drop leftover tone/ringtone samples in any case.
        flushUrgent();

        // Not running (e.g. a failed start left the retry state) — nothing
        // to tear down, and flushMain() must not run here: it would wipe
        // every RingBufferPool buffer, including live audio of unrelated
        // streams.
        if (status_ != Status::Started)
            return;

        if (activeStreamMask_ != 0) {
            // Other stream types still depend on the single full-duplex
            // VoiceProcessingIO unit — e.g. an answered call's CAPTURE /
            // PLAYBACK when the RINGTONE guard lingers out 750 ms after
            // pickup. Tearing the unit down here silenced the whole call
            // until the audio layer was recreated (same defect as the
            // macOS layer, fixed there first).
            SIP_CORE_DBG("iOS CoreLayer stopStream: unit kept alive, remaining mask=0x%x",
                         activeStreamMask_);
            return;
        }

        destroyAudioLayer();
        flushMain();
    });
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
    (void) ioActionFlags;
    (void) inTimeStamp;
    (void) inBusNumber;

    AudioFormat currentOutFormat {static_cast<unsigned>(outSampleRate_),
                                  static_cast<unsigned>(outChannelsPerFrame_),
                                  AV_SAMPLE_FMT_FLTP};

    if (auto toPlay = getPlayback(currentOutFormat, inNumberFrames)) {
        const auto& frame = *toPlay->pointer();
        for (int i = 0; i < frame.ch_layout.nb_channels; ++i) {
            std::copy_n((Float32*) frame.extended_data[i],
                        inNumberFrames,
                        (Float32*) ioData->mBuffers[i].mData);
        }
    } else {
        for (unsigned i = 0; i < currentOutFormat.nb_channels; ++i)
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
    (void) ioData;

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

    // Write the mic samples in our buffer
    checkErr(AudioUnitRender(ioUnit_,
            ioActionFlags,
            inTimeStamp,
            inBusNumber,
            inNumberFrames,
            &buffer));

    if (isCaptureMuted_) {
        libav_utils::fillWithSilence(inBuff->pointer());
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

} // namespace sip_core

#pragma GCC diagnostic pop
