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

#ifndef CORE_LAYER_H_
#define CORE_LAYER_H_

#include "audio/audiolayer.h"
#include "device_signature.h"
#include <AudioToolbox/AudioToolbox.h>
#include <atomic>
#include <cstdint>
#include <string>

#define checkErr(err) \
    if (err) { \
        OSStatus error = static_cast<OSStatus>(err); \
        fprintf(stdout, "CoreAudio Error: %ld ->  %s:  %d\n", (long) error, __FILE__, __LINE__); \
        fflush(stdout); \
    }

/**
 * @file  CoreLayer.h
 * @brief Main OSX sound class. Manages the data transfers between the application and the hardware.
 */

namespace sip_core {

class RingBuffer;
class AudioDevice;

class CoreLayer : public AudioLayer
{
public:
    CoreLayer(const AudioPreference& pref);
    ~CoreLayer();

    /**
     * Scan the sound card available on the system
     * @return std::vector<std::string> The vector containing the string description of the card
     */
    virtual std::vector<std::string> getCaptureDeviceList() const;
    virtual std::vector<std::string> getPlaybackDeviceList() const;

    virtual int getAudioDeviceIndex(const std::string& name, AudioDeviceType type) const;
    virtual std::string getAudioDeviceName(int index, AudioDeviceType type) const;

    /**
     * Get the index of the audio card for capture
     * @return int The index of the card used for capture
     */
    virtual int getIndexCapture() const { return indexIn_; }

    /**
     * Get the index of the audio card for playback
     * @return int The index of the card used for playback
     */
    virtual int getIndexPlayback() const { return indexOut_; }

    /**
     * Get the index of the audio card for ringtone (could be differnet from playback)
     * @return int The index of the card used for ringtone
     */
    virtual int getIndexRingtone() const { return indexRing_; }

    /**
     * Configure the AudioUnit
     */
    void initAudioLayerIO(AudioDeviceType stream);

    /**
     * Start the capture stream and prepare the playback stream.
     * The playback starts accordingly to its threshold
     * CoreAudio Library API
     */

    virtual void startStream(AudioDeviceType stream = AudioDeviceType::ALL);

    void destroyAudioLayer();

    /**
     * Stop the playback and capture streams.
     * Drops the pending frames and put the capture and playback handles to PREPARED state
     * CoreAudio Library API
     */
    virtual void stopStream(AudioDeviceType stream = AudioDeviceType::ALL);

private:
    NON_COPYABLE(CoreLayer);

    void initAudioFormat();

    static OSStatus outputCallback(void* inRefCon,
                                   AudioUnitRenderActionFlags* ioActionFlags,
                                   const AudioTimeStamp* inTimeStamp,
                                   UInt32 inBusNumber,
                                   UInt32 inNumberFrames,
                                   AudioBufferList* ioData);

    void write(AudioUnitRenderActionFlags* ioActionFlags,
               const AudioTimeStamp* inTimeStamp,
               UInt32 inBusNumber,
               UInt32 inNumberFrames,
               AudioBufferList* ioData);

    static OSStatus inputCallback(void* inRefCon,
                                  AudioUnitRenderActionFlags* ioActionFlags,
                                  const AudioTimeStamp* inTimeStamp,
                                  UInt32 inBusNumber,
                                  UInt32 inNumberFrames,
                                  AudioBufferList* ioData);
    static OSStatus deviceIsAliveCallback(AudioObjectID inObjectID,
                                          UInt32 inNumberAddresses,
                                          const AudioObjectPropertyAddress inAddresses[],
                                          void* inRefCon);
    static OSStatus devicesChangedCallback(AudioObjectID inObjectID,
                                           UInt32 inNumberAddresses,
                                           const AudioObjectPropertyAddress inAddresses[],
                                           void* inRefCon);

    void read(AudioUnitRenderActionFlags* ioActionFlags,
              const AudioTimeStamp* inTimeStamp,
              UInt32 inBusNumber,
              UInt32 inNumberFrames,
              AudioBufferList* ioData);

    virtual void updatePreference(AudioPreference& pref, int index, AudioDeviceType type);

    /**
     * Number of audio cards on which capture stream has been opened
     */
    int indexIn_;

    /**
     * Number of audio cards on which playback stream has been opened
     */
    int indexOut_;

    /**
     * Number of audio cards on which ringtone stream has been opened
     */
    int indexRing_;

    AudioUnit ioUnit_;

    Float64 inSampleRate_;
    UInt32 inChannelsPerFrame_;
    Float64 outSampleRate_;
    UInt32 outChannelsPerFrame_;

    /** Guard flag to prevent infinite restart loop caused by VoiceProcessingIO
     *  creating/destroying its internal VPAUAggregateAudioDevice. */
    std::atomic<bool> restartingAudio_ {false};

    /** Stored device IDs so we can remove property listeners in destroyAudioLayer. */
    AudioDeviceID inputDeviceID_ {0};
    AudioDeviceID playbackDeviceID_ {0};

    /** Stored device names for name-based re-selection after hot-plug.
     *  On restart, we first try to find the device by name (stable across
     *  index shifts) before falling back to the preference index. */
    std::string captureDeviceName_;
    std::string playbackDeviceName_;

    /** Hash of the user-visible device topology captured at the end of the
     *  most recent successful startStream lambda. Device-change callbacks
     *  compare against this to suppress spurious events that CoreAudio
     *  raises when VoiceProcessingIO creates/destroys its internal
     *  VPAUAggregateAudioDevice.
     *
     *  Stored as a single atomic so the writer (audio configuration queue)
     *  and the readers (CoreAudio HAL listener thread) need no further
     *  synchronisation. A sentinel value of 0 means "no valid snapshot";
     *  real hashes always have the low bit forced to 1 to avoid colliding
     *  with the sentinel. */
    static constexpr std::uint64_t kNoDeviceSignature = 0;
    std::atomic<std::uint64_t> deviceSignatureHash_ {kNoDeviceSignature};

    std::vector<AudioDevice> getDeviceList(bool getCapture) const;

    /** Compute the current user-visible device signature hash by
     *  enumerating capture and playback devices and filtering internal
     *  helpers. Always returns a non-sentinel value. */
    std::uint64_t computeDeviceSignatureHash() const;
};

} // namespace sip_core

#endif // CORE_LAYER_H_
