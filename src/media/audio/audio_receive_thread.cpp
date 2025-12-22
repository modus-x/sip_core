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

#include "audio_receive_thread.h"
#include "libav_deps.h"
#include "logger.h"
#include "manager.h"
#include "media_decoder.h"
#include "media_io_handle.h"
#include "media_recorder.h"
#include "ringbuffer.h"
#include "ringbufferpool.h"

#include <memory>

namespace sip_core {

AudioReceiveThread::AudioReceiveThread(const std::string& id,
                                       const AudioFormat& format,
                                       const std::string& sdp,
                                       const uint16_t mtu)
    : id_(id)
    , format_(format)
    , stream_(sdp)
    , sdpContext_(new MediaIOHandle(sdp.size(), false, &readFunction, 0, 0, this))
    , mtu_(mtu)
    , loop_(std::bind(&AudioReceiveThread::setup, this),
            std::bind(&AudioReceiveThread::process, this),
            std::bind(&AudioReceiveThread::cleanup, this),
            ThreadLoop::ThreadPriority::HIGH)
{}

AudioReceiveThread::~AudioReceiveThread()
{
    onSuccessfulSetup_ = nullptr;
    loop_.join();
}

bool
AudioReceiveThread::setup()
{
    if(sip_core::Manager::instance().audioPreference.getVadEnabled())
        createAudioProcessor();

    std::lock_guard lk(mutex_);
    audioDecoder_.reset(new MediaDecoder([this](std::shared_ptr<MediaFrame>&& frame) mutable {
        if (!muteState_) {
            std::lock_guard<std::mutex> lock(audioProcessorMutex_);
            if (audioProcessor_) {
                // we need it for some reason
                auto silence = std::make_shared<AudioFrame>(format_, frame->pointer()->nb_samples);
                libav_utils::fillWithSilence(silence->pointer());
                audioProcessor_->putPlayback(silence);
                
                audioProcessor_->putRecorded(std::static_pointer_cast<AudioFrame>(frame));
            }
            else {
                notify(frame);
            }
            
            ringbuffer_->put(std::move(std::static_pointer_cast<AudioFrame>(frame)));
        }
    }));
    audioDecoder_->setContextCallback([this]() {
        if (recorderCallback_)
            recorderCallback_(getInfo());
    });
    audioDecoder_->setInterruptCallback(interruptCb, this);

    // custom_io so the SDP demuxer will not open any UDP connections
    args_.input = SDP_FILENAME;
    args_.format = "sdp";
    args_.sdp_flags = "custom_io";

    if (stream_.str().empty()) {
        SIP_CORE_ERR("No SDP loaded");
        return false;
    }

    audioDecoder_->setIOContext(sdpContext_.get());
    if (audioDecoder_->openInput(args_)) {
        SIP_CORE_ERR("Could not open input \"%s\"", SDP_FILENAME);
        return false;
    }

    // Now replace our custom AVIOContext with one that will read packets
    audioDecoder_->setIOContext(demuxContext_.get());
    if (audioDecoder_->setupAudio()) {
        SIP_CORE_ERR("decoder IO startup failed");
        return false;
    }

    ringbuffer_ = Manager::instance().getRingBufferPool().getRingBuffer(id_);
    Manager::instance().getRingBufferPool().bindHalfDuplexOut(RingBufferPool::DEFAULT_ID, id_);


    if (onSuccessfulSetup_)
        onSuccessfulSetup_(MEDIA_AUDIO, 1);

    return true;
}

void
AudioReceiveThread::process()
{
    audioDecoder_->decode();

    std::lock_guard<std::mutex> lock(audioProcessorMutex_);
    if (audioProcessor_) {
        while (auto rec = audioProcessor_->getProcessed()) {
            if (voice_ != rec->has_voice) {
                voice_ = rec->has_voice;
                voiceCallback_(voice_);
            }

            notify(std::static_pointer_cast<MediaFrame>(rec));
        }
    }
}

void
AudioReceiveThread::cleanup()
{
    std::lock_guard lk(mutex_);
    audioDecoder_.reset();
    demuxContext_.reset();

    destroyAudioProcessor();
}

int
AudioReceiveThread::readFunction(void* opaque, uint8_t* buf, int buf_size)
{
    std::istream& is = static_cast<AudioReceiveThread*>(opaque)->stream_;
    is.read(reinterpret_cast<char*>(buf), buf_size);

    auto count = is.gcount();
    return count ? count : AVERROR_EOF;
}

void
AudioReceiveThread::createAudioProcessor()
{
    std::lock_guard<std::mutex> lock(audioProcessorMutex_);

    if (audioProcessor_) {
        return;
    }

    unsigned int frame_size;
    if (sip_core::Manager::instance().audioPreference.getAudioProcessor() == "speex") {
        // TODO: maybe force this to be equivalent to 20ms? as expected by speex
        frame_size = format_.sample_rate / 50u;
    } else {
        frame_size = format_.sample_rate / 100u;
    }

    SIP_CORE_WARN("Starting audio processor with: {%d Hz, %d channels, %d samples/frame}",
                  format_.sample_rate,
                  format_.nb_channels,
                  frame_size);

    if (sip_core::Manager::instance().audioPreference.getAudioProcessor() == "webrtc") {
#if HAVE_WEBRTC_AP
        SIP_CORE_WARN("[audio_receive_thread] using WebRTCAudioProcessor");
        audioProcessor_.reset(new WebRTCAudioProcessor(format_,
                                                      frame_size,
                                                      false));

        WebRTCAudioProcessor* proc = static_cast<WebRTCAudioProcessor*>(audioProcessor_.get());
        proc->setWebRtcParams(sip_core::Manager::instance().audioPreference.getWebRtcParams());

#else
        SIP_CORE_ERR("[audio_receive_thread] audioProcessor preference is webrtc, but library not linked! "
                     "using NullAudioProcessor instead");
        audioProcessor_.reset(new NullAudioProcessor(format_, frame_size));
#endif
    } else if (sip_core::Manager::instance().audioPreference.getAudioProcessor() == "speex") {
#if HAVE_SPEEXDSP
        SIP_CORE_WARN("[audio_receive_thread] using SpeexAudioProcessor");
        audioProcessor_.reset(new SpeexAudioProcessor(format_, frame_size));
#else
        SIP_CORE_ERR("[audio_receive_thread] audioProcessor preference is speex, but library not linked! "
                     "using NullAudioProcessor instead");
        audioProcessor_.reset(new NullAudioProcessor(format_, frame_size));
#endif
    } else if (sip_core::Manager::instance().audioPreference.getAudioProcessor() == "null") {
        SIP_CORE_WARN("[audio_receive_thread] using NullAudioProcessor");
        audioProcessor_.reset(new NullAudioProcessor(format_, frame_size));
    } else {
        SIP_CORE_ERR(
            "[audio_receive_thread] audioProcessor preference not recognized, using NullAudioProcessor "
            "instead");
        audioProcessor_.reset(new NullAudioProcessor(format_, frame_size));
    }
    
    audioProcessor_->enableVoiceActivityDetection(true);
}

void
AudioReceiveThread::destroyAudioProcessor()
{
    std::lock_guard<std::mutex> lock(audioProcessorMutex_);
    audioProcessor_.reset();
}

// This callback is used by libav internally to break out of blocking calls
int
AudioReceiveThread::interruptCb(void* data)
{
    auto context = static_cast<AudioReceiveThread*>(data);
    return not context->loop_.isRunning();
}

void
AudioReceiveThread::setVoiceCallback(std::function<void(bool)> cb)
{
    if (cb) {
        voiceCallback_ = std::move(cb);
    } else {
        SIP_CORE_ERR("AudioReceiveThread trying to set invalid voice callback");
    }
}

void
AudioReceiveThread::addIOContext(SocketPair& socketPair)
{
    demuxContext_.reset(socketPair.createIOContext(mtu_));
}

void
AudioReceiveThread::setRecorderCallback(
    const std::function<void(const MediaStream& ms)>& cb)
{
    recorderCallback_ = cb;
    if (audioDecoder_)
        audioDecoder_->setContextCallback([this]() {
            if (recorderCallback_)
                recorderCallback_(getInfo());
        });
}

MediaStream
AudioReceiveThread::getInfo() const
{
    return audioDecoder_->getStream("a:remote");
}

void
AudioReceiveThread::startReceiver()
{
    loop_.start();
}

void
AudioReceiveThread::stopReceiver()
{
    loop_.stop();
}

void
AudioReceiveThread::setMuted(bool muted)
{
    muteState_ = muted;
}

void 
AudioReceiveThread::setVAD(bool active)
{
    if(active)
        createAudioProcessor();
    else
        destroyAudioProcessor();
}

}; // namespace sip_core
