/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
 *  Author: Guillaume Roguez <Guillaume.Roguez@savoirfairelinux.com>
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

#include "media_decoder_base.h"
#include "observer.h"

#ifdef ENABLE_VIDEO
#include "video/video_base.h"
#include "video/video_scaler.h"
#endif // ENABLE_VIDEO

#include "audio/audiobuffer.h"
#include "noncopyable.h"

#ifdef RING_ACCEL
#include "video/accel.h"
#endif
#include "logger.h"

#include <map>
#include <string>
#include <memory>
#include <chrono>
#include <queue>

namespace sip_core {

class MediaDemuxer
{
public:
    MediaDemuxer();
    ~MediaDemuxer();

    static const char* getStatusStr(DecodeStatus status);

    enum class CurrentState { Demuxing, Finished };
    using StreamCallback = std::function<DecodeStatus(AVPacket&)>;

    int openInput(const DeviceParams&);

    void setInterruptCallback(int (*cb)(void*), void* opaque);
    void setIOContext(MediaIOHandle* ioctx);

    int findStreamInfo();
    int selectStream(AVMediaType type);

    // this sets callbacks, in which compressed bytes from Packets will be decoded
    void setStreamCallback(unsigned stream, StreamCallback cb = {})
    {
        if (streams_.size() <= stream)
            streams_.resize(stream + 1);
        streams_[stream] = std::move(cb);
    }

    void updateCurrentState(MediaDemuxer::CurrentState state) { currentState_ = state; }

    void setFileFinishedCb(std::function<void(bool)> cb);

    MediaDemuxer::CurrentState getCurrentState() { return currentState_; }

    AVStream* getStream(unsigned stream)
    {
        if (stream >= inputCtx_->nb_streams) {
            SIP_CORE_ERR("Stream index is out of range: %u", stream);
            return {};
        }
        return inputCtx_->streams[stream];
    }

    DecodeStatus decode();
    DecodeStatus demuxe();

    int64_t getDuration() const;
    bool seekFrame(int stream_index, int64_t timestamp);
    void setNeedFrameCb(std::function<void()> cb);
    void emitFrame(bool isAudio);

    const char* getInputName() const { return inputCtx_->iformat->long_name; }

private:
    bool streamInfoFound_ {false};
    AVFormatContext* inputCtx_ = nullptr;
    std::vector<StreamCallback> streams_;
    int64_t startTime_;
    int64_t lastReadPacketTime_ {};

    // params that we get from SYSTEM layer
    DeviceParams inputParams_;

    //
    AVDictionary* options_ = nullptr;
    MediaDemuxer::CurrentState currentState_;

    // AvPackets will be written here!
    std::mutex audioBufferMutex_ {};
    std::mutex videoBufferMutex_ {};
    std::queue<std::unique_ptr<AVPacket, std::function<void(AVPacket*)>>> videoBuffer_ {};
    std::queue<std::unique_ptr<AVPacket, std::function<void(AVPacket*)>>> audioBuffer_ {};

    std::function<void()> needFrameCb_;
    std::function<void(bool)> fileFinishedCb_;
    void clearFrames();

    // push
    void pushFrameFrom(std::queue<std::unique_ptr<AVPacket, std::function<void(AVPacket*)>>>& buffer,
                       bool isAudio,
                       std::mutex& mutex);
    int baseWidth_ {};
    int baseHeight_ {};
    int (*interruptCb_)(void*) = nullptr;
    void* interruptOpaque_ = nullptr;
};

class MediaDecoder final : public MediaDecoderBase
{
public:
    MediaDecoder();
    MediaDecoder(MediaObserver observer);
    MediaDecoder(MediaObserver observer, int width, int height);
    MediaDecoder(const std::shared_ptr<MediaDemuxer>& demuxer, int index);
    MediaDecoder(const std::shared_ptr<MediaDemuxer>& demuxer, int index, MediaObserver observer);
    MediaDecoder(const std::shared_ptr<MediaDemuxer>& demuxer, AVMediaType type)
        : MediaDecoder(demuxer, demuxer->selectStream(type))
    {}
    ~MediaDecoder();

    void emulateRate() override { emulateRate_ = true; }

    /// just forward to demuxer
    int openInput(const DeviceParams&) override;
    /// just forward to demuxer
    void setInterruptCallback(int (*cb)(void*), void* opaque) override;
    /// just forward to demuxer
    void setIOContext(MediaIOHandle* ioctx) override;
    void enableLateFrameDrop(std::chrono::microseconds threshold) override;

    int setup(AVMediaType type) override;
    int setupAudio() override { return setup(AVMEDIA_TYPE_AUDIO); }
    int setupVideo() override { return setup(AVMEDIA_TYPE_VIDEO); }

    // forward to demuxer. at the end, if stream was setup correctly, MediaDecoder's decode method
    // will be called
    DecodeStatus decode() override;

    DecodeStatus flush() override;

    int getWidth() const override;
    int getHeight() const override;
    std::string getDecoderName() const override;

    rational<double> getFps() const override;
    AVPixelFormat getPixelFormat() const override;

    void updateStartTime(int64_t startTime) override;

    void emitFrame(bool isAudio) override;
    void flushBuffers() override;
    void setSeekTime(int64_t time) override;
#ifdef RING_ACCEL
    void enableAccel(bool enableAccel) override;
#endif

    MediaStream getStream(std::string name = "") const override;

    void setResolutionChangedCallback(std::function<void(int, int)> cb) override
    {
        resolutionChangedCallback_ = std::move(cb);
    }

    void setFEC(bool enable) override { fecEnabled_ = enable; }

    void setContextCallback(const std::function<void()>& cb) override
    {
        firstDecode_.exchange(true);
        contextCallback_ = cb;
    }

private:
    NON_COPYABLE(MediaDecoder);

    DecodeStatus decode(AVPacket&);

    rational<unsigned> getTimeBase() const;

    // demuxer which will get compressed frames from camera, it must be always created in every constructor
    std::shared_ptr<MediaDemuxer> demuxer_;

    const AVCodec* inputDecoder_ = nullptr;
    AVCodecContext* decoderCtx_ = nullptr;
    AVStream* avStream_ = nullptr;
    bool emulateRate_ = false;
    int64_t startTime_;
    int64_t lastTimestamp_ {0};

    int frameCount_ {0};

    DeviceParams inputParams_;
    bool dropLateFrames_ {false};
    int64_t lateFrameDropThresholdUs_ {0};
    int64_t firstCapturePtsUs_ {AV_NOPTS_VALUE};
    int64_t firstCaptureWallclockUs_ {0};
    uint64_t lateFrameDropCount_ {0};
    std::chrono::steady_clock::time_point lastLateFrameLog_ {};

    int correctPixFmt(int input_pix_fmt);
    int setupStream();

    bool fallback_ = false;

#ifdef RING_ACCEL
    bool enableAccel_ = true;
    std::unique_ptr<video::HardwareAccel> accel_;
    unsigned short accelFailures_ = 0;
#endif

    // report here x value of MediaFrame after demuxer
    MediaObserver callback_;
    int prepareDecoderContext();
    int64_t seekTime_ = -1;
    void resetSeekTime() { seekTime_ = -1; }
    std::function<void(int, int)> resolutionChangedCallback_;

    // what we actually got from selected demuxer's stream
    int width_ {0};
    int height_ {0};

    // for OPUS lol
    bool fecEnabled_ {false};

    std::function<void()> contextCallback_;
    std::atomic_bool firstDecode_ {true};

protected:
    AVDictionary* options_ = nullptr;
};

} // namespace sip_core
