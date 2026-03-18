/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
 *  Author: Tristan Matthews <tristan.matthews@savoirfairelinux.com>
 *  Author: Philippe Gorley <philippe.gorley@savoirfairelinux.com>
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
#pragma once

#include "audiobuffer.h"
#include "media_buffer.h"
#include "media_device.h"
#include "media_codec.h"
#include "noncopyable.h"
#include "observer.h"
#include "socket_pair.h"
#include "threadloop.h"
#include "media_decoder_base.h"

#include <functional>
#include <cstdint>
#include <sstream>

#include "audio-processing/null_audio_processor.h"
#if HAVE_WEBRTC_AP
#include "audio-processing/webrtc.h"
#endif

namespace sip_core {

class MediaDecoder;
class MediaIOHandle;
struct MediaStream;
class RingBuffer;

class AudioReceiveThread : public Observable<std::shared_ptr<MediaFrame>>
{
public:
    AudioReceiveThread(const std::string& id,
                       const AudioFormat& format,
                       const std::string& sdp,
                       const uint16_t mtu);
    ~AudioReceiveThread();

    MediaStream getInfo() const;

    void setVoiceCallback(std::function<void(bool)> cb);
    void addIOContext(SocketPair& socketPair);
    void startReceiver();
    void stopReceiver();
    void setMuted(bool muted);
    void setVAD(bool active);
    void setVadSensitivity(int32_t sensitivity);
    

    void setSuccessfulSetupCb(const std::function<void(MediaType, bool)>& cb)
    {
        onSuccessfulSetup_ = cb;
    }

    void setRecorderCallback(const std::function<void(const MediaStream& ms)>& cb);

private:
    NON_COPYABLE(AudioReceiveThread);

    static constexpr auto SDP_FILENAME = "dummyFilename";

    static int interruptCb(void* ctx);
    static int readFunction(void* opaque, uint8_t* buf, int buf_size);

    void createAudioProcessor();
    void destroyAudioProcessor();
    void applyVadSensitivityLocked();
    static int clampVadSensitivity(int32_t sensitivity);

    std::mutex audioProcessorMutex_ {};
    std::unique_ptr<AudioProcessor> audioProcessor_;

    /*-----------------------------------------------------------------*/
    /* These variables should be used in thread (i.e. process()) only! */
    /*-----------------------------------------------------------------*/
    // last voice activity state
    bool voice_ {false};
    std::function<void(bool)> voiceCallback_;

    const std::string id_;
    const AudioFormat& format_;

    DeviceParams args_;

    std::istringstream stream_;
    std::unique_ptr<MediaDecoderBase> audioDecoder_;
    std::unique_ptr<MediaIOHandle> sdpContext_;
    std::unique_ptr<MediaIOHandle> demuxContext_;

    std::shared_ptr<RingBuffer> ringbuffer_;

    uint16_t mtu_;

    ThreadLoop loop_;
    bool setup();
    void process();
    void cleanup();

    std::function<void(MediaType, bool)> onSuccessfulSetup_;
    std::function<void(const MediaStream& ms)> recorderCallback_;

    bool muteState_ {false};
    int vadSensitivity_ {3};
};

} // namespace sip_core
