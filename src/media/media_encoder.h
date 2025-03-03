/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
 *  Author: Guillaume Roguez <Guillaume.Roguez@savoirfairelinux.com>
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "audio/audio_sender.h"

#ifdef ENABLE_VIDEO
#include "video/video_base.h"
#include "video/video_scaler.h"
#endif

#include "noncopyable.h"
#include "media_buffer.h"
#include "media_codec.h"
#include "media_stream.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

extern "C" {
struct AVCodecContext;
struct AVFormatContext;
struct AVDictionary;
struct AVCodec;
}

namespace sip_core {

struct MediaDescription;
struct AccountCodecInfo;

#ifdef RING_ACCEL
namespace video {
class HardwareAccel;
}
#endif

class MediaEncoderException : public std::runtime_error
{
public:
    MediaEncoderException(const char* msg)
        : std::runtime_error(msg)
    {}
};

class MediaEncoder
{
public:
    MediaEncoder();
    ~MediaEncoder();

    void openOutput(const std::string& filename, const std::string& format = "");
    void setMetadata(const std::string& title, const std::string& description);

    // set media stream parameters (video width, height, audio sample rate, etc)
    void setOptions(const MediaStream& opts);

    // set media description parameters (payload type, video mode)
    void setOptions(const MediaDescription& args);

    // add steam to context with some predefined codec info

    int addStream(const SystemCodecInfo& codec);
    void setIOContext(AVIOContext* ioctx) { ioCtx_ = ioctx; }
    void resetStreams(int width, int height);

    void encodeAndSendDtmf(dtmf* data);

    bool send(AVPacket& packet, int streamIdx = -1);

    // send raw data
    bool sendBuffer(uint8_t *buf1, int len, bool m, int flags);

#ifdef ENABLE_VIDEO
    int encode(const std::shared_ptr<VideoFrame>& input, bool is_keyframe, int64_t frame_number);
#endif // ENABLE_VIDEO

    int encodeAudio(AudioFrame& frame);

    // frame should be ready to be sent to the encoder at this point
    int encode(AVFrame* frame, int streamIdx, bool is_keyframe);

    int flush();
    std::string print_sdp();

    /* getWidth and getHeight return size of the encoded frame.
     * Values have meaning only after openLiveOutput call.
     */
    int getWidth() const { return videoOpts_.width; };
    int getHeight() const { return videoOpts_.height; };

    void setInitSeqVal(uint16_t seqVal);
    uint16_t getLastSeqValue();

    const std::string& getAudioCodec() const { return audioCodec_; }
    const std::string& getVideoCodec() const { return videoCodec_; }

    int setBitrate(uint64_t br);
    int setPacketLoss(uint64_t pl);

#ifdef RING_ACCEL
    void enableAccel(bool enableAccel);
#endif

    static std::string testH265Accel();

    unsigned getStreamCount() const;
    MediaStream getStream(const std::string& name, int streamIdx = -1) const;
    void sendDummyPacket();

private:
    NON_COPYABLE(MediaEncoder);

    AVCodecContext* prepareEncoderContext(const AVCodec* outputCodec, bool is_video);
    void forcePresetX2645(AVCodecContext* encoderCtx);
    void extractProfileLevelID(const std::string& parameters, AVCodecContext* ctx);
    int initStream(const std::string& codecName, AVBufferRef* framesCtx = {});
    int initStream(const SystemCodecInfo& systemCodecInfo, AVBufferRef* framesCtx = {});
    void openIOContext();
    void startIO();

    AVCodecContext* getCurrentVideoAVCtx();
    AVCodecContext* getCurrentAudioAVCtx();
    void stopEncoder();
    AVCodecContext* initCodec(AVMediaType mediaType, AVCodecID avcodecId, uint64_t br);
    void initH264(AVCodecContext* encoderCtx, uint64_t br);
    void initH265(AVCodecContext* encoderCtx, uint64_t br);
    void initVP8(AVCodecContext* encoderCtx, uint64_t br);
    void initMPEG4(AVCodecContext* encoderCtx, uint64_t br);
    void initH263(AVCodecContext* encoderCtx, uint64_t br);
    void initOpus(AVCodecContext* encoderCtx);
    bool isDynBitrateSupported(AVCodecID codecid);
    bool isDynPacketLossSupported(AVCodecID codecid);
    void initAccel(AVCodecContext* encoderCtx, uint64_t br);
#ifdef ENABLE_VIDEO
    int getHWFrame(const std::shared_ptr<VideoFrame>& input, std::shared_ptr<VideoFrame>& output);
    std::shared_ptr<VideoFrame> getUnlinkedHWFrame(const VideoFrame& input);
    std::shared_ptr<VideoFrame> getHWFrameFromSWFrame(const VideoFrame& input);
    std::shared_ptr<VideoFrame> getScaledSWFrame(const VideoFrame& input);
#endif

    // encode data into h264 / something another
    std::vector<AVCodecContext*> encoders_;

    // output from encoder. it may be rtp or file
    AVFormatContext* outputCtx_ = nullptr;

    // output to mp4. only local file url
    AVFormatContext* mp4Ctx_ = nullptr;

    // codec for mp4
    AVCodecContext* mp4CodecContext_ = nullptr;

    // stream for mp4
    AVStream *mp4Stream_ = nullptr;

    std::string mp4File_;

    AVDictionary *mp4Opts_;

    AVIOContext* ioCtx_ = nullptr;
    int currentStreamIdx_ = -1;
    unsigned sent_samples = 0;
    bool initialized_ {false};
    bool fileIO_ {false};
    unsigned int currentVideoCodecID_ {0};
    const AVCodec* outputCodec_ = nullptr;
    std::mutex encMutex_;
    bool linkableHW_ {false};
    RateMode mode_ {RateMode::CRF_CONSTRAINED};
    bool fecEnabled_ {true};

#ifdef ENABLE_VIDEO
    video::VideoScaler scaler_;
    std::shared_ptr<VideoFrame> scaledFrame_;
#endif // ENABLE_VIDEO

    std::vector<uint8_t> scaledFrameBuffer_;
    int scaledFrameBufferSize_ = 0;

#ifdef RING_ACCEL
    bool enableAccel_ {false};
    std::unique_ptr<video::HardwareAccel> accel_;
#endif

protected:
    void readConfig(AVCodecContext* encoderCtx);
    AVDictionary* options_ = nullptr;
    MediaStream videoOpts_;
    MediaStream audioOpts_;
    std::string videoCodec_;
    std::string audioCodec_;
};

} // namespace sip_core
