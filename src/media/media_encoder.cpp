/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3, or (at your option)
 *  any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, see <https://www.gnu.org/licenses/>.
 */
#include "libav_deps.h" // MUST BE INCLUDED FIRST
#include "media_codec.h"
#include "media_encoder.h"
#include "media_buffer.h"

#include "client/ring_signal.h"
#include "fileutils.h"
#include "logger.h"
#include "manager.h"
#include "string_utils.h"
#include "system_codec_container.h"

#ifdef RING_ACCEL
#include "video/accel.h"
#endif

extern "C" {
#include <libavutil/parseutils.h>
}

#include <algorithm>
#include <fstream>
#include <json/json.h>
#include <sstream>
#include <thread> // hardware_concurrency
#include <string_view>
#include <cmath>
#include <chrono>

#define DEBUG_SDP          1
#ifdef RQM
#define MP4_IO_BUFFER_SIZE 1152
#endif

#ifndef AV_INPUT_BUFFER_MIN_SIZE
#define AV_INPUT_BUFFER_MIN_SIZE 16384
#endif

using namespace std::literals;

namespace sip_core {

    constexpr double LOGREG_PARAM_A {101};
    constexpr double LOGREG_PARAM_B {-5.};

    constexpr double LOGREG_PARAM_A_HEVC {96};
    constexpr double LOGREG_PARAM_B_HEVC {-5.};

    MediaEncoder::MediaEncoder()
            : outputCtx_(avformat_alloc_context())
    {

        // auto now = std::chrono::system_clock::now();
        // auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

        // mp4File_ = fmt::format("C:\\Users\\Admin\\code\\sip_core\\vids\\{}.mp4",
        //     std::to_string(timestamp));

        // mp4FileStream_.open(mp4File_, std::ios::binary | std::ios::app);

        // if (!mp4FileStream_) {
        //     throw std::runtime_error("Failed to open file: " + mp4File_);
        // }

#ifdef RQM
        // flush fragments as fast as possible
    mp4Ctx_->flags = AVFMT_FLAG_NOBUFFER | AVFMT_FLAG_FLUSH_PACKETS;

    auto buf = static_cast<uint8_t*>(av_malloc(MP4_IO_BUFFER_SIZE));

    if (!mp4Ctx_) {
        SIP_CORE_ERR() << "mp4_error: cannot create mp4Ctx_";
    }

    mp4IOCtx_ = avio_alloc_context(
        buf,
        MP4_IO_BUFFER_SIZE,
        true,
        reinterpret_cast<void*>(this),
        NULL,
        [](void* me, uint8_t* buf, int len) {
            // SIP_CORE_ERR() << "writeContainerToRtp " << len;
            return static_cast<MediaEncoder*>(me)->writeContainerToRtp(buf, len);
        },
        NULL);

    if (!mp4IOCtx_) {
        SIP_CORE_ERR() << "mp4_error: cannot create mp4IOCtx_";
    }


    avformat_alloc_output_context2(&mp4Ctx_, NULL, "mp4", NULL);
#endif

        SIP_CORE_DBG("[%p] New instance created", this);
    }


#ifdef RQM
    int
MediaEncoder::writeContainerToRtp(uint8_t* buf, int buf_size)
{

    // Ensure file is still open
    // if (mp4FileStream_.is_open()) {
    //     mp4FileStream_.write(reinterpret_cast<const char*>(buf), buf_size);
    //     if (!mp4FileStream_) {
    //         throw std::runtime_error("Error writing to file");
    //     }
    //     mp4FileStream_.flush();  // Ensure data is written to disk
    // }

    AVPacket pkt;
    av_init_packet(&pkt);

    pkt.data = buf;

    pkt.size = buf_size;
    pkt.dts = mp4SentPackets_;
    pkt.pts = mp4SentPackets_;

    mp4SentPackets_++;
    send(pkt, currentStreamIdx_);
    return buf_size;
}
#endif

    MediaEncoder::~MediaEncoder()
    {
        if (outputCtx_) {
            if (outputCtx_->priv_data && outputCtx_->pb)
                av_write_trailer(outputCtx_);
            if (fileIO_) {
                avio_close(outputCtx_->pb);
            }
            for (auto encoderCtx : encoders_) {
                if (encoderCtx) {

                    avcodec_free_context(&encoderCtx);
                }
            }
            avformat_free_context(outputCtx_);
        }

#ifdef RQM
        if (mp4Ctx_) {
        avformat_free_context(mp4Ctx_);
    }
#endif
        av_dict_free(&options_);

        SIP_CORE_DBG("[%p] Instance destroyed", this);
    }

    void
    MediaEncoder::setOptions(const MediaStream& opts)
    {
        if (!opts.isValid()) {
            SIP_CORE_ERR() << "Invalid options";
            return;
        }

        if (opts.isVideo) {
            videoOpts_ = opts;

            // if we have normal scale factor, scale it!
            int scaleFactor = opts.downScaleFactor;

            if (25 < scaleFactor && scaleFactor < 100) {
                videoOpts_.width = std::round(videoOpts_.width * (scaleFactor / 100.0));
                videoOpts_.height = std::round(videoOpts_.height * (scaleFactor / 100.0));
            }

            // Make sure width and height are even (required by x264)
            // This is especially for image/gif streaming, as video files and cameras usually have even
            // resolutions
            videoOpts_.width = ((videoOpts_.width >> 3) << 3);
            videoOpts_.height = ((videoOpts_.height >> 3) << 3);
            if (!videoOpts_.frameRate)
                videoOpts_.frameRate = 30;
            if (!videoOpts_.bitrate) {
                videoOpts_.bitrate = SystemCodecInfo::DEFAULT_VIDEO_BITRATE;
            }
        } else {
            audioOpts_ = opts;
        }
    }

    void
    MediaEncoder::setOptions(const MediaDescription& args)
    {
#ifdef RQM
        // no payload exists for fMP4. So we use some random value
    int payload_type = 111;
#else
        int payload_type = args.payload_type;
#endif

        av_opt_set_int(outputCtx_, "strict", -2, AV_OPT_SEARCH_CHILDREN);

        int ret;
        if (payload_type
            and (ret = av_opt_set_int(outputCtx_, "payload_type", payload_type, AV_OPT_SEARCH_CHILDREN) < 0))
            SIP_CORE_ERR() << "Failed to set payload type: " << libav_utils::getError(ret);

        if (not args.parameters.empty())
            libav_utils::setDictValue(&options_, "parameters", args.parameters);

        mode_ = args.mode;
        linkableHW_ = args.linkableHW;
        fecEnabled_ = args.fecEnabled;
    }

    void
    MediaEncoder::setMetadata(const std::string& title, const std::string& description)
    {
        if (not title.empty())
            libav_utils::setDictValue(&outputCtx_->metadata, "title", title);
        if (not description.empty())
            libav_utils::setDictValue(&outputCtx_->metadata, "description", description);
    }

    void
    MediaEncoder::setInitSeqVal(uint16_t seqVal)
    {
        // only set not default value (!=0)
        if (seqVal != 0)
            av_opt_set_int(outputCtx_, "seq", seqVal, AV_OPT_SEARCH_CHILDREN);
    }

    uint16_t
    MediaEncoder::getLastSeqValue()
    {
        int64_t retVal;
        if (av_opt_get_int(outputCtx_, "seq", AV_OPT_SEARCH_CHILDREN, &retVal) >= 0)
            return (uint16_t) retVal;
        else
            return 0;
    }

    void
    MediaEncoder::openOutput(const std::string& filename, const std::string& format)
    {
        avformat_free_context(outputCtx_);
        int result = avformat_alloc_output_context2(&outputCtx_,
                                                    nullptr,
                                                    format.empty() ? nullptr : format.c_str(),
                                                    filename.c_str());
        if (result < 0)
            SIP_CORE_ERR() << "Cannot open " << filename << ": " << libav_utils::getError(-result);
    }

    int
    MediaEncoder::addStream(const SystemCodecInfo& systemCodecInfo)
    {
        if (systemCodecInfo.mediaType == MEDIA_AUDIO) {
            audioCodec_ = systemCodecInfo.name;
        } else {
            videoCodec_ = systemCodecInfo.name;
        }

        auto stream = avformat_new_stream(outputCtx_, outputCodec_);

        if (stream == nullptr) {
            SIP_CORE_ERR("[%p] Failed to create coding instance for %s",
                         this,
                         systemCodecInfo.name.c_str());
            return -1;
        }

        SIP_CORE_DBG("[%p] Created new coding instance for %s @ index %d",
                     this,
                     systemCodecInfo.name.c_str(),
                     stream->index);
        // Only init audio now, video will be intialized when
        // encoding the first frame.
        if (systemCodecInfo.mediaType == MEDIA_AUDIO) {
            return initStream(systemCodecInfo);
        }

        // If audio options are valid, it means this session is used
        // for both audio and video streams, thus the video will be
        // at index 1, otherwise it will be at index 0.
        // TODO. Hacky, needs better solution.
        if (audioOpts_.isValid())
            return 1;
        else
            return 0;
    }

    int
    MediaEncoder::initStream(const std::string& codecName, AVBufferRef* framesCtx)
    {
        const auto codecInfo = getSystemCodecContainer()->searchCodecByName(codecName, MEDIA_ALL);
        if (codecInfo)
            return initStream(*codecInfo, framesCtx);
        else
            return -1;
    }

    int
    MediaEncoder::initStream(const SystemCodecInfo& systemCodecInfo, AVBufferRef* framesCtx)
    {
        SIP_CORE_DBG("[%p] Initializing stream: codec type %d, name %s, lib %s",
                     this,
                     systemCodecInfo.codecType,
                     systemCodecInfo.name.c_str(),
                     systemCodecInfo.libName.c_str());

        std::lock_guard<std::mutex> lk(encMutex_);

        if (!outputCtx_)
            throw MediaEncoderException("Cannot allocate stream");

        // Must already have codec instance(s)
        if (outputCtx_->nb_streams == 0) {
            SIP_CORE_ERR("[%p] Can not init, output context has no coding sessions!", this);
            throw MediaEncoderException("Can not init, output context has no coding sessions!");
        }

        AVCodecContext* encoderCtx = nullptr;
        AVMediaType mediaType;

        if (systemCodecInfo.mediaType == MEDIA_VIDEO)
            mediaType = AVMEDIA_TYPE_VIDEO;
        else if (systemCodecInfo.mediaType == MEDIA_AUDIO)
            mediaType = AVMEDIA_TYPE_AUDIO;
        else
            throw MediaEncoderException("Unsuported media type");

        AVStream* stream {nullptr};

        // Only supports one audio and one video streams at most per instance.
        for (unsigned i = 0; i < outputCtx_->nb_streams; i++) {
            stream = outputCtx_->streams[i];
            if (stream->codecpar->codec_type == mediaType) {
                if (mediaType == AVMEDIA_TYPE_VIDEO) {
                    stream->codecpar->width = videoOpts_.width;
                    stream->codecpar->height = videoOpts_.height;
                }
                break;
            }
        }

        if (stream == nullptr) {
            SIP_CORE_ERR("[%p] Can not init, output context has no coding sessions for %s",
                         this,
                         systemCodecInfo.name.c_str());
            throw MediaEncoderException("Cannot allocate stream");
        }

        currentStreamIdx_ = stream->index;
#ifdef RING_ACCEL
        // Get compatible list of Hardware API
    if (enableAccel_ && mediaType == AVMEDIA_TYPE_VIDEO) {
        auto APIs = video::HardwareAccel::getCompatibleAccel(static_cast<AVCodecID>(
                                                             systemCodecInfo.avcodecId),
                                                             videoOpts_.width,
                                                             videoOpts_.height,
                                                             CODEC_ENCODER);
        for (const auto& it : APIs) {
            accel_ = std::make_unique<video::HardwareAccel>(it); // save accel
            // Init codec need accel_ to init encoderCtx accelerated
            encoderCtx = initCodec(mediaType,
                                   static_cast<AVCodecID>(systemCodecInfo.avcodecId),
                                   videoOpts_.bitrate);
            encoderCtx->opaque = accel_.get();
            // Check if pixel format from encoder match pixel format from decoder frame context
            // if it mismatch, it means that we are using two different hardware API (nvenc and
            // vaapi for example) in this case we don't want link the APIs
            if (framesCtx) {
                auto hw = reinterpret_cast<AVHWFramesContext*>(framesCtx->data);
                if (encoderCtx->pix_fmt != hw->format)
                    linkableHW_ = false;
            }
            auto ret = accel_->initAPI(linkableHW_, framesCtx);
            if (ret < 0) {
                accel_.reset();
                encoderCtx = nullptr;
                continue;
            }
            accel_->setDetails(encoderCtx);
            if (avcodec_open2(encoderCtx, outputCodec_, &options_) < 0) {
                // Failed to open codec
                SIP_CORE_WARN("Fail to open hardware encoder %s with %s ",
                              avcodec_get_name(static_cast<AVCodecID>(systemCodecInfo.avcodecId)),
                              it.getName().c_str());
                avcodec_free_context(&encoderCtx);
                encoderCtx = nullptr;
                accel_ = nullptr;
                continue;
            } else {
                // Succeed to open codec
                SIP_CORE_WARN("Using hardware encoding for %s with %s ",
                              avcodec_get_name(static_cast<AVCodecID>(systemCodecInfo.avcodecId)),
                              it.getName().c_str());
                encoders_.emplace_back(encoderCtx);
                break;
            }
        }
    }
#endif

        if (!encoderCtx) {
            SIP_CORE_WARN("Not using hardware encoding for %s",
                          avcodec_get_name(static_cast<AVCodecID>(systemCodecInfo.avcodecId)));
            encoderCtx = initCodec(mediaType,
                                   static_cast<AVCodecID>(systemCodecInfo.avcodecId),
                                   videoOpts_.bitrate);

            encoderCtx->flags |= videoOpts_.flags;

            // readConfig(encoderCtx);

            encoders_.emplace_back(encoderCtx);
            if (avcodec_open2(encoderCtx, outputCodec_, &options_) < 0)
                throw MediaEncoderException("Could not open encoder");
        }

        avcodec_parameters_from_context(stream->codecpar, encoderCtx);

        // framerate is not copied from encoderCtx to stream
        stream->avg_frame_rate = encoderCtx->framerate;

#ifdef RQM

        avcodec_parameters_from_context(mp4Stream_->codecpar, encoderCtx);

    // framerate is not copied from encoderCtx to stream
    mp4Stream_->avg_frame_rate = encoderCtx->framerate;

#endif
#ifdef ENABLE_VIDEO
        if (systemCodecInfo.mediaType == MEDIA_VIDEO) {
            // allocate buffers for both scaled (pre-encoder) and encoded frames
            const int width = encoderCtx->width;
            const int height = encoderCtx->height;
            int format = encoderCtx->pix_fmt;
#ifdef RING_ACCEL
            if (accel_) {
            // hardware encoders require a specific pixel format
            auto desc = av_pix_fmt_desc_get(encoderCtx->pix_fmt);
            if (desc && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL))
                format = accel_->getSoftwareFormat();
        }
#endif
            scaledFrameBufferSize_ = videoFrameSize(format, width, height);
            if (scaledFrameBufferSize_ < 0)
                throw MediaEncoderException(
                        ("Could not compute buffer size: " + libav_utils::getError(scaledFrameBufferSize_))
                                .c_str());
            else if (scaledFrameBufferSize_ <= AV_INPUT_BUFFER_MIN_SIZE)
                throw MediaEncoderException("buffer too small");

            scaledFrameBuffer_.reserve(scaledFrameBufferSize_);
            scaledFrame_ = std::make_shared<VideoFrame>();
            scaledFrame_->setFromMemory(scaledFrameBuffer_.data(), format, width, height);

            if (videoOpts_.noColor) {
                grayScaledFrameBufferSize_ = videoFrameSize(AV_PIX_FMT_GRAY8, width, height);
                grayScaledFrameBuffer_.reserve(grayScaledFrameBufferSize_);
                grayScaledFrame_ = std::make_shared<VideoFrame>();
                grayScaledFrame_->setFromMemory(grayScaledFrameBuffer_.data(),
                                                AV_PIX_FMT_GRAY8,
                                                width,
                                                height);
            }
        }
#endif // ENABLE_VIDEO

        return stream->index;
    }

    void
    MediaEncoder::openIOContext()
    {
#ifdef RQM
        if (mp4IOCtx_) {
        mp4Ctx_->pb = mp4IOCtx_;
    }
#endif

        if (ioCtx_) {
            outputCtx_->pb = ioCtx_;
            outputCtx_->packet_size = outputCtx_->pb->buffer_size;
        } else {
            int ret = 0;
#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(58, 7, 100)
            const char* filename = outputCtx_->url;
#else
            const char* filename = outputCtx_->filename;
#endif
            if (!(outputCtx_->oformat->flags & AVFMT_NOFILE)) {
                fileIO_ = true;
                if ((ret = avio_open(&outputCtx_->pb, filename, AVIO_FLAG_WRITE)) < 0) {
                    std::stringstream ss;
                    ss << "Could not open IO context for '" << filename
                       << "': " << libav_utils::getError(ret);
                    throw MediaEncoderException(ss.str().c_str());
                }
            }
        }
    }

    void
    MediaEncoder::startIO()
    {

#ifdef RQM
        bool writeToMp4 = false;
#endif

        if (!outputCtx_->pb) {
            openIOContext();
#ifdef RQM
            writeToMp4 = true;
#endif
        }

        if (avformat_write_header(outputCtx_, options_ ? &options_ : nullptr)) {
            SIP_CORE_ERR("Could not write header for output file... check codec parameters");
            throw MediaEncoderException("Failed to write output file header");
        }

#ifdef RQM
        // as in OBS studio
    libav_utils::setDictValue(&mp4Opts_, "movflags", "+empty_moov+separate_moof+frag_every_frame");
#endif

        // av_dict_set_int(&mp4Opts_, "frag_size", 1152, AV_OPT_SEARCH_CHILDREN);

#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(58, 7, 100)
        av_dump_format(outputCtx_, 0, outputCtx_->url, 1);
#ifdef RQM
        av_dump_format(mp4Ctx_, 0, mp4Ctx_->url, 1);
#endif
#else
        av_dump_format(outputCtx_, 0, outputCtx_->filename, 1);
#ifdef RQM
    av_dump_format(mp4Ctx_, 0, mp4Ctx_->filename, 1);
#endif
#endif
        initialized_ = true;

#ifdef RQM
        if (writeToMp4) {
        if (avformat_write_header(mp4Ctx_, &mp4Opts_)) {
            SIP_CORE_ERR(
                "mp4_error: could not write header for output mp4... check codec parameters");
        }
    }
#endif
    }

#ifdef ENABLE_VIDEO
    int
    MediaEncoder::encode(const std::shared_ptr<VideoFrame>& input,
                         bool is_keyframe,
                         int64_t frame_number)
    {
        auto width = (input->width() >> 3) << 3;
        auto height = (input->height() >> 3) << 3;
        if (initialized_ && (getWidth() != width || getHeight() != height)) {
            resetStreams(width, height);
            is_keyframe = true;
        }

        if (!initialized_) {
            initStream(videoCodec_, input->pointer()->hw_frames_ctx);
            startIO();
        }

        std::shared_ptr<VideoFrame> output;
#ifdef RING_ACCEL
        if (getHWFrame(input, output) < 0) {
        SIP_CORE_ERR("Fail to get hardware frame");
        return -1;
    }
#else
        output = getScaledSWFrame(*input.get());
#endif // RING_ACCEL

        if (!output) {
            SIP_CORE_ERR("Fail to get frame");
            return -1;
        }
        auto avframe = output->pointer();

#ifdef RQM
        // for mp4 stream, we need to set pts to the same value as input frame (to preserve the original timestamp)
    avframe->pts = input->pointer()->pts;
    avframe->pkt_dts = input->pointer()->pkt_dts;
    avframe->pkt_duration = input->pointer()->pkt_duration;
#endif

        AVCodecContext* enc = encoders_[currentStreamIdx_];
#ifndef RQM
        // for rtp stream, we need to set pts just increasing by 1
        avframe->pts = frame_number;
#endif

        if (enc->framerate.num != enc->time_base.den || enc->framerate.den != enc->time_base.num)
            avframe->pts /= (rational<int64_t>(enc->framerate) * rational<int64_t>(enc->time_base))
                    .real<int64_t>();

        if (is_keyframe) {
            avframe->pict_type = AV_PICTURE_TYPE_I;
            avframe->flags |= AV_FRAME_FLAG_KEY;
        } else {
            avframe->pict_type = AV_PICTURE_TYPE_NONE;
            avframe->flags &= ~AV_FRAME_FLAG_KEY;
        }

        return encode(avframe, currentStreamIdx_);
    }
#endif // ENABLE_VIDEO

    int
    MediaEncoder::encodeAudio(AudioFrame& frame)
    {
        if (!initialized_) {
            // Initialize on first video frame, or first audio frame if no video stream
            if (not videoOpts_.isValid())
                startIO();
            else
                return 0;
        }
        frame.pointer()->pts = sent_samples;
        sent_samples += frame.pointer()->nb_samples;
        encode(frame.pointer(), currentStreamIdx_);
        return 0;
    }

    int
    MediaEncoder::encode(AVFrame* frame, int streamIdx)
    {
        if (!initialized_ && frame) {
            // Initialize on first video frame, or first audio frame if no video stream
            bool isVideo = (frame->width > 0 && frame->height > 0);
            if (isVideo and videoOpts_.isValid()) {
                // Has video stream, so init with video frame
                streamIdx = initStream(videoCodec_, frame->hw_frames_ctx);
                startIO();
            } else if (!isVideo and !videoOpts_.isValid()) {
                // Only audio, for MediaRecorder, which doesn't use encodeAudio
                startIO();
            } else {
                return 0;
            }
        }
        int ret = 0;
        if (streamIdx >= encoders_.size())
            return -1;
        AVCodecContext* encoderCtx = encoders_[streamIdx];
        AVPacket pkt;
        av_init_packet(&pkt);
        pkt.data = nullptr; // packet data will be allocated by the encoder
        pkt.size = 0;

        if (!encoderCtx)
            return -1;

        ret = avcodec_send_frame(encoderCtx, frame);
        if (ret < 0)
            return -1;

        while (ret >= 0) {
            ret = avcodec_receive_packet(encoderCtx, &pkt);
            if (ret == AVERROR(EAGAIN))
                break;
            if (ret < 0 && ret != AVERROR_EOF) { // we still want to write our frame on EOF
                SIP_CORE_ERR() << "Failed to encode frame: " << libav_utils::getError(ret);
                return ret;
            }

            if (pkt.size) {
#ifdef RQM
                // Rescale the packet's timestamps from encoder time base to output stream's time base
            pkt.pts = av_rescale_q(pkt.pts, encoderCtx->time_base, mp4Stream_->time_base);
            pkt.dts = av_rescale_q(pkt.dts, encoderCtx->time_base, mp4Stream_->time_base);
            pkt.duration = av_rescale_q(pkt.duration, encoderCtx->time_base, mp4Stream_->time_base);

            // write packet to format, it will be written to mp4 buffer, and ONLY THEN sent to rtp
            if (av_write_frame(mp4Ctx_, &pkt) == 0) {
                break;
            } else {
                SIP_CORE_ERR() << "mp4_error:Failed to write frame: " << libav_utils::getError(ret);
            }
#else
                if (send(pkt, streamIdx)) {
                    break;
                }
#endif
            }
        }

        av_packet_unref(&pkt);
        return 0;
    }

    bool
    MediaEncoder::send(AVPacket& pkt, int streamIdx, bool dummy)
    {
        if (!initialized_) {
            // do not init stream because 
            if (!dummy) {
                streamIdx = initStream(videoCodec_);
            }
            startIO();
        }
        if (streamIdx < 0)
            streamIdx = currentStreamIdx_;
        if (!dummy && streamIdx >= 0 and static_cast<size_t>(streamIdx) < encoders_.size()
            and static_cast<unsigned int>(streamIdx) < outputCtx_->nb_streams) {
            auto encoderCtx = encoders_[streamIdx];
            pkt.stream_index = streamIdx;
            if (pkt.pts != AV_NOPTS_VALUE)
                pkt.pts = av_rescale_q(pkt.pts,
                                       encoderCtx->time_base,
                                       outputCtx_->streams[streamIdx]->time_base);
            if (pkt.dts != AV_NOPTS_VALUE)
                pkt.dts = av_rescale_q(pkt.dts,
                                       encoderCtx->time_base,
                                       outputCtx_->streams[streamIdx]->time_base);
        }
        // write the compressed frame
        auto ret = av_write_frame(outputCtx_, &pkt);
        if (ret < 0) {
            SIP_CORE_ERR() << "av_write_frame failed: " << libav_utils::getError(ret);
        }
        return ret >= 0;
    }

    int
    MediaEncoder::flush()
    {
        int ret = 0;
        for (size_t i = 0; i < outputCtx_->nb_streams; ++i) {
            if (encode(nullptr, i) < 0) {
                SIP_CORE_ERR() << "Could not flush stream #" << i;
                ret |= 1u << i; // provide a way for caller to know which streams failed
            }
        }
        return -ret;
    }

    std::string
    MediaEncoder::print_sdp()
    {
        /* theora sdp can be huge */
        const auto sdp_size = outputCtx_->streams[currentStreamIdx_]->codecpar->extradata_size + 2048;
        std::string sdp(sdp_size, '\0');
        av_sdp_create(&outputCtx_, 1, &(*sdp.begin()), sdp_size);

        std::string result;
        result.reserve(sdp_size);

        std::string_view steam(sdp), line;
        while (sip_core::getline(steam, line)) {
            /* strip windows line ending */
            result += line.substr(0, line.length() - 1);
            result += "\n"sv;
        }
#ifdef DEBUG_SDP
        SIP_CORE_DBG("Sending SDP:\n%s", result.c_str());
#endif
        return result;
    }

    AVCodecContext*
    MediaEncoder::prepareEncoderContext(const AVCodec* outputCodec, bool is_video)
    {
        AVCodecContext* encoderCtx = avcodec_alloc_context3(outputCodec);

        auto encoderName = outputCodec->name; // guaranteed to be non null if AVCodec is not null

    encoderCtx->thread_count = std::min(std::thread::hardware_concurrency() / 2, is_video ? 4u : 16u);
    SIP_CORE_DBG("[%s] Using %d threads", encoderName, encoderCtx->thread_count);

        if (is_video) {
            // resolution must be a multiple of two
            encoderCtx->width = videoOpts_.width;
            encoderCtx->height = videoOpts_.height;

            // satisfy ffmpeg: denominator must be 16bit or less value
            // time base = 1/FPS
            av_reduce(&encoderCtx->framerate.num,
                      &encoderCtx->framerate.den,
                      videoOpts_.frameRate.numerator(),
                      videoOpts_.frameRate.denominator(),
                      (1U << 16) - 1);
            encoderCtx->time_base = av_inv_q(encoderCtx->framerate);

#ifdef RQM
            // this will create new stream
        mp4Stream_ = avformat_new_stream(mp4Ctx_, NULL);

        if (!mp4Stream_) {
            SIP_CORE_ERR() << "mp4_error: cannot create mp4Stream_";
        }
#else
            // Fri Jul 22 11:37:59 EDT 2011:tmatth:XXX: DON'T set this, we want our
            // pps and sps to be sent in-band for RTP
            // This is to place global headers in extradata instead of every
            // keyframe.
            // encoderCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
#endif

            // emit one intra frame every gop_size frames
            encoderCtx->max_b_frames = 0;

            // pixel format of our used video formats is always yuv420p
            encoderCtx->pix_fmt = AV_PIX_FMT_YUV420P;
            // Keep YUV format for macOS
#ifdef RING_ACCEL
            #if defined(TARGET_OS_IOS) && TARGET_OS_IOS
        if (accel_)
            encoderCtx->pix_fmt = accel_->getSoftwareFormat();
#elif !defined(__APPLE__)
        if (accel_)
            encoderCtx->pix_fmt = accel_->getFormat();
#endif
#endif

#ifdef RQM
            // mp4 REQUIRES global header in codec to work
        encoderCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
#endif
        } else {
            encoderCtx->sample_fmt = AV_SAMPLE_FMT_S16;
            encoderCtx->sample_rate = std::max(8000, audioOpts_.sampleRate);
            encoderCtx->time_base = AVRational {1, encoderCtx->sample_rate};
            if (audioOpts_.nbChannels > 2 || audioOpts_.nbChannels < 1) {
                encoderCtx->ch_layout.nb_channels = std::clamp(audioOpts_.nbChannels, 1, 2);
                SIP_CORE_ERR() << "[" << encoderName
                               << "] Clamping invalid channel count: " << audioOpts_.nbChannels
                               << " -> " << encoderCtx->ch_layout.nb_channels;
            } else {
                encoderCtx->ch_layout.nb_channels = audioOpts_.nbChannels;
            }
            av_channel_layout_default(&encoderCtx->ch_layout, encoderCtx->ch_layout.nb_channels);
            if (audioOpts_.frameSize) {
                encoderCtx->frame_size = audioOpts_.frameSize;
                SIP_CORE_DBG() << "[" << encoderName << "] Frame size " << encoderCtx->frame_size;
            } else {
                SIP_CORE_WARN() << "[" << encoderName << "] Frame size not set";
            }
        }

        return encoderCtx;
    }

    void
    MediaEncoder::forcePresetX2645(AVCodecContext* encoderCtx)
    {
#ifdef RING_ACCEL
        if (accel_ && accel_->getName() == "nvenc") {
        if (av_opt_set(encoderCtx, "preset", "fast", AV_OPT_SEARCH_CHILDREN))
            SIP_CORE_WARN("Failed to set preset to 'fast'");
        if (av_opt_set(encoderCtx, "level", "auto", AV_OPT_SEARCH_CHILDREN))
            SIP_CORE_WARN("Failed to set level to 'auto'");
        if (av_opt_set_int(encoderCtx, "zerolatency", 1, AV_OPT_SEARCH_CHILDREN))
            SIP_CORE_WARN("Failed to set zerolatency to '1'");
    } else
#endif
        {
            if (source_.find("display") != std::string::npos) {
                // Desktop sharing: optimize for screen content
                av_opt_set(encoderCtx, "preset", "veryslow", AV_OPT_SEARCH_CHILDREN);    // Balance speed/quality
                av_opt_set(encoderCtx, "tune", "stillimage", AV_OPT_SEARCH_CHILDREN); // Low latency

                auto quality = h264CrfFromQuality();

                av_opt_set_double(encoderCtx, "crf", quality, AV_OPT_SEARCH_CHILDREN);

                av_opt_set_int(encoderCtx, "refs", 1, AV_OPT_SEARCH_CHILDREN);          // Low latency

                // Screen content optimizations
                av_opt_set_int(encoderCtx, "rc-lookahead", 5, AV_OPT_SEARCH_CHILDREN);  // Reduced for lower latency
                av_opt_set_int(encoderCtx, "min-keyint", 1, AV_OPT_SEARCH_CHILDREN);    // Allow immediate keyframes on scene change

                // Additional screen optimizations
                av_opt_set_int(encoderCtx, "bframes", 0, AV_OPT_SEARCH_CHILDREN);       // No B-frames for lower latency
                av_opt_set_int(encoderCtx, "me", 1, AV_OPT_SEARCH_CHILDREN);            // Diamond motion estimation (faster)
                av_opt_set_int(encoderCtx, "subme", 6, AV_OPT_SEARCH_CHILDREN);         // Good subpixel refinement
                av_opt_set_int(encoderCtx, "trellis", 1, AV_OPT_SEARCH_CHILDREN);       // Optimize for sharp edges
            }
            else {
                // Camera sharing: optimize for motion content
                av_opt_set(encoderCtx, "preset", "ultrafast", AV_OPT_SEARCH_CHILDREN);    // Balance speed/quality
                av_opt_set(encoderCtx, "tune", "zerolatency", AV_OPT_SEARCH_CHILDREN); // Low latency

                av_opt_set_double(encoderCtx, "crf", 26, AV_OPT_SEARCH_CHILDREN);
            }
        }
    }

    int
    MediaEncoder::h264CrfFromQuality() const
    {
        switch (videoOpts_.quality) {
            case 1:
                return 35;
            case 2:
                return 30;
            case 3:
                return 25;
            case 4:
                return 20;
            default:
                return 32;
        }
    }

    void
    MediaEncoder::extractProfileLevelID(const std::string& parameters, AVCodecContext* ctx)
    {
        // From RFC3984:
        // If no profile-level-id is present, the Baseline Profile without
        // additional constraints at Level 1 MUST be implied.
        ctx->profile = AV_PROFILE_H264_CONSTRAINED_BASELINE;
        ctx->level = 0x0d;
        // ctx->level = 0x0d; // => 13 aka 1.3
        if (parameters.empty())
            return;

        const std::string target("profile-level-id=");
        size_t needle = parameters.find(target);
        if (needle == std::string::npos)
            return;

        needle += target.length();
        const size_t id_length = 6; /* digits */
        const std::string profileLevelID(parameters.substr(needle, id_length));
        if (profileLevelID.length() != id_length)
            return;

        int result;
        std::stringstream ss;
        ss << profileLevelID;
        ss >> std::hex >> result;
        // profile-level id consists of three bytes
        const unsigned char profile_idc = result >> 16;           // 42xxxx -> 42
        const unsigned char profile_iop = ((result >> 8) & 0xff); // xx80xx -> 80
        ctx->level = result & 0xff;                               // xxxx0d -> 0d
        switch (profile_idc) {
            case AV_PROFILE_H264_BASELINE:
                // check constraint_set_1_flag
                if ((profile_iop & 0x40) >> 6)
                    ctx->profile |= AV_PROFILE_H264_CONSTRAINED;
                break;
            case AV_PROFILE_H264_HIGH_10:
            case AV_PROFILE_H264_HIGH_422:
            case AV_PROFILE_H264_HIGH_444_PREDICTIVE:
                // check constraint_set_3_flag
                if ((profile_iop & 0x10) >> 4)
                    ctx->profile |= AV_PROFILE_H264_INTRA;
                break;
        }
        SIP_CORE_DBG("Using profile %s (%x) and level %d",
                     avcodec_profile_name(AV_CODEC_ID_H264, ctx->profile),
                     ctx->profile,
                     ctx->level);
    }

#ifdef RING_ACCEL
    void
MediaEncoder::enableAccel(bool enableAccel)
{
    enableAccel_ = enableAccel;
    emitSignal<libsip_core::ConfigurationSignal::HardwareEncodingChanged>(enableAccel_);
    if (!enableAccel_) {
        accel_.reset();
        for (auto enc : encoders_)
            enc->opaque = nullptr;
    }
}
#endif

    unsigned
    MediaEncoder::getStreamCount() const
    {
        return (audioOpts_.isValid() + videoOpts_.isValid());
    }

    MediaStream
    MediaEncoder::getStream(const std::string& name, int streamIdx) const
    {
        // if streamIdx is negative, use currentStreamIdx_
        if (streamIdx < 0)
            streamIdx = currentStreamIdx_;
        // make sure streamIdx is valid
        if (getStreamCount() <= 0 || streamIdx < 0 || encoders_.size() < (unsigned) (streamIdx + 1))
            return {};
        auto enc = encoders_[streamIdx];
        // TODO set firstTimestamp
        auto ms = MediaStream(name, enc);
#ifdef RING_ACCEL
        if (accel_)
        ms.format = accel_->getSoftwareFormat();
#endif
        return ms;
    }

    AVCodecContext*
    MediaEncoder::initCodec(AVMediaType mediaType, AVCodecID avcodecId, uint64_t br)
    {
        outputCodec_ = nullptr;
#ifdef RING_ACCEL
        if (mediaType == AVMEDIA_TYPE_VIDEO) {
            if (enableAccel_) {
                if (accel_) {
                    outputCodec_ = avcodec_find_encoder_by_name(accel_->getCodecName().c_str());
                }
            } else {
                SIP_CORE_WARN() << "Hardware encoding disabled";
            }
        }
#endif

        if (!outputCodec_) {
            /* find the video encoder */
            if (avcodecId == AV_CODEC_ID_H263)
                // For H263 encoding, we force the use of AV_CODEC_ID_H263P (H263-1998)
                // H263-1998 can manage all frame sizes while H263 don't
                // AV_CODEC_ID_H263 decoder will be used for decoding
                outputCodec_ = avcodec_find_encoder(AV_CODEC_ID_H263P);
            else
                outputCodec_ = avcodec_find_encoder(static_cast<AVCodecID>(avcodecId));
            if (!outputCodec_) {
                throw MediaEncoderException("No output encoder");
            }
        }

        AVCodecContext* encoderCtx = prepareEncoderContext(outputCodec_,
                                                           mediaType == AVMEDIA_TYPE_VIDEO);

        // Only clamp video bitrate
        if (mediaType == AVMEDIA_TYPE_VIDEO && br > 0) {
            if (br < SystemCodecInfo::DEFAULT_MIN_BITRATE) {
                SIP_CORE_WARNING("Requested bitrate {:d} too low, setting to {:d}",
                                 br,
                                 SystemCodecInfo::DEFAULT_MIN_BITRATE);
                br = SystemCodecInfo::DEFAULT_MIN_BITRATE;
            } else if (br > SystemCodecInfo::DEFAULT_MAX_BITRATE) {
                SIP_CORE_WARNING("Requested bitrate {:d} too high, setting to {:d}",
                                 br,
                                 SystemCodecInfo::DEFAULT_MAX_BITRATE);
                br = SystemCodecInfo::DEFAULT_MAX_BITRATE;
            }
        }

        /* Enable libopus FEC encoding support */
        if (mediaType == AVMEDIA_TYPE_AUDIO) {
            if (avcodecId == AV_CODEC_ID_OPUS) {
                initOpus(encoderCtx);
            }
        }

        /* let x264 preset override our encoder settings */
        if (avcodecId == AV_CODEC_ID_H264) {
            auto profileLevelId = libav_utils::getDictValue(options_, "parameters");
            extractProfileLevelID(profileLevelId, encoderCtx);
            forcePresetX2645(encoderCtx);
            initH264(encoderCtx, br);
        } else if (avcodecId == AV_CODEC_ID_HEVC) {
            encoderCtx->profile = AV_PROFILE_HEVC_MAIN;
            forcePresetX2645(encoderCtx);
            initH265(encoderCtx, br);
        } else if (avcodecId == AV_CODEC_ID_VP8) {
            initVP8(encoderCtx, br);
        } else if (avcodecId == AV_CODEC_ID_MPEG4) {
            initMPEG4(encoderCtx, br);
        } else if (avcodecId == AV_CODEC_ID_H263) {
            initH263(encoderCtx, br);
        }
        initAccel(encoderCtx, br);
        return encoderCtx;
    }

    int
    MediaEncoder::setBitrate(uint64_t br)
    {
        std::lock_guard<std::mutex> lk(encMutex_);
        AVCodecContext* encoderCtx = getCurrentVideoAVCtx();
        if (not encoderCtx)
            return -1; // NOK

        AVCodecID codecId = encoderCtx->codec_id;

        if (not isDynBitrateSupported(codecId))
            return 0; // Restart needed

        // No need to restart encoder for h264, h263 and MPEG4
        // Change parameters on the fly
        if (codecId == AV_CODEC_ID_H264)
            initH264(encoderCtx, br);
        if (codecId == AV_CODEC_ID_HEVC)
            initH265(encoderCtx, br);
        else if (codecId == AV_CODEC_ID_H263P)
            initH263(encoderCtx, br);
        else if (codecId == AV_CODEC_ID_MPEG4)
            initMPEG4(encoderCtx, br);
        else {
            // restart encoder on runtime doesn't work for VP8
            // stopEncoder();
            // encoderCtx = initCodec(codecType, codecId, br);
            // if (avcodec_open2(encoderCtx, outputCodec_, &options_) < 0)
            //     throw MediaEncoderException("Could not open encoder");
        }
        initAccel(encoderCtx, br);
        return 1; // OK
    }

    int
    MediaEncoder::setPacketLoss(uint64_t pl)
    {
        std::lock_guard<std::mutex> lk(encMutex_);
        AVCodecContext* encoderCtx = getCurrentAudioAVCtx();
        if (not encoderCtx)
            return -1; // NOK

        AVCodecID codecId = encoderCtx->codec_id;

        if (not isDynPacketLossSupported(codecId))
            return 0; // Restart needed

        // Cap between 0 and 100
        pl = std::clamp((int) pl, 0, 100);

        // Change parameters on the fly
        if (codecId == AV_CODEC_ID_OPUS)
            av_opt_set_int(encoderCtx, "packet_loss", (int64_t) pl, AV_OPT_SEARCH_CHILDREN);
        return 1; // OK
    }

    void
    MediaEncoder::initH264(AVCodecContext* encoderCtx, uint64_t br)
    {
        // Use capped‐CRF (quality + rate constraints) to get “normal” quality

        // Choose a CRF that gives good quality without too heavy data
        int crf = 28;  // moderate quality (lower is better quality but higher bitrate)
        // Bound quantizer swings
        int qmin = crf - 6;  // e.g. 17
        int qmax = crf + 6;  // e.g. 29
        if (qmin < 1) qmin = 1;

        // Set VBV / VBV-constrained parameters
        // maxrate = br (no exceeding the target)
        int64_t maxrate = 1500000;
        // buffsize: allow some fluctuation, but not overly large
        // e.g. buffer = br * 2/3  (or br * 3/4) — you can tune this
        int64_t bufsize = (1500000 * 2) / 3;

        // Preset: pick a trade-off between speed and compression
        const char* preset = "medium";  // you might try "medium" if CPU allows

        // Set options on encoder context
        av_opt_set_int(encoderCtx, "crf", crf, AV_OPT_SEARCH_CHILDREN);
        av_opt_set_int(encoderCtx, "maxrate", maxrate, AV_OPT_SEARCH_CHILDREN);
        av_opt_set_int(encoderCtx, "bufsize", bufsize, AV_OPT_SEARCH_CHILDREN);
        av_opt_set_int(encoderCtx, "qmin", qmin, AV_OPT_SEARCH_CHILDREN);
        av_opt_set_int(encoderCtx, "qmax", qmax, AV_OPT_SEARCH_CHILDREN);
        av_opt_set(encoderCtx, "preset", preset, AV_OPT_SEARCH_CHILDREN);

        // Optionally disable scene cut to reduce spikes
        av_opt_set_int(encoderCtx, "no-scenecut", 1, AV_OPT_SEARCH_CHILDREN);
        // Intra refresh may help error resilience / refresh gradually
        av_opt_set_int(encoderCtx, "intra-refresh", 1, AV_OPT_SEARCH_CHILDREN);

        SIP_CORE_DEBUG("H264 init for 720p: br=%" PRIu64
                    ", crf=%d, maxrate=%" PRIu64
                    ", bufsize=%" PRIu64
                    ", qmin=%d, qmax=%d, preset=%s",
                    br, crf, maxrate, bufsize, qmin, qmax, preset);
    }

    void
    MediaEncoder::initH265(AVCodecContext* encoderCtx, uint64_t br)
    {
        // If auto quality disabled use CRF mode
        if (mode_ == RateMode::CRF_CONSTRAINED) {
            uint64_t maxBitrate = 1000 * br;
            // H265 use 50% less bitrate compared to H264 (half bitrate is equivalent to a change 6 for
            // CRF) https://slhck.info/video/2017/02/24/crf-guide.html
            // 200 Kbit/s    -> CRF35
            // 6 Mbit/s      -> CRF18
            uint8_t crf = (uint8_t) std::round(LOGREG_PARAM_A_HEVC
                                               + LOGREG_PARAM_B_HEVC * std::log(maxBitrate));
            uint64_t bufSize = maxBitrate / 2;
            av_opt_set_int(encoderCtx, "crf", crf, AV_OPT_SEARCH_CHILDREN);
            av_opt_set_int(encoderCtx, "maxrate", maxBitrate, AV_OPT_SEARCH_CHILDREN);
            av_opt_set_int(encoderCtx, "bufsize", bufSize, AV_OPT_SEARCH_CHILDREN);
            SIP_CORE_DEBUG("H265 encoder setup: crf={:d}, maxrate={:d} kbit/s, bufsize={:d} kbit",
                           crf,
                           maxBitrate / 1000,
                           bufSize / 1000);
        } else if (mode_ == RateMode::CBR) {
            av_opt_set_int(encoderCtx, "b", br * 1000, AV_OPT_SEARCH_CHILDREN);
            av_opt_set_int(encoderCtx, "maxrate", br * 1000, AV_OPT_SEARCH_CHILDREN);
            av_opt_set_int(encoderCtx, "minrate", br * 1000, AV_OPT_SEARCH_CHILDREN);
            av_opt_set_int(encoderCtx, "bufsize", br * 500, AV_OPT_SEARCH_CHILDREN);
            av_opt_set_int(encoderCtx, "crf", -1, AV_OPT_SEARCH_CHILDREN);
            SIP_CORE_DEBUG("H265 encoder setup cbr: bitrate={:d} kbit/s", br);
        }
    }

    void
    MediaEncoder::initVP8(AVCodecContext* encoderCtx, uint64_t br)
    {
        if (mode_ == RateMode::CQ) {
            av_opt_set_int(encoderCtx, "g", 120, AV_OPT_SEARCH_CHILDREN);
            av_opt_set_int(encoderCtx, "lag-in-frames", 0, AV_OPT_SEARCH_CHILDREN);
            av_opt_set(encoderCtx, "deadline", "good", AV_OPT_SEARCH_CHILDREN);
            av_opt_set_int(encoderCtx, "cpu-used", 0, AV_OPT_SEARCH_CHILDREN);
            av_opt_set_int(encoderCtx, "vprofile", 0, AV_OPT_SEARCH_CHILDREN);
            av_opt_set_int(encoderCtx, "qmax", 23, AV_OPT_SEARCH_CHILDREN);
            av_opt_set_int(encoderCtx, "qmin", 0, AV_OPT_SEARCH_CHILDREN);
            av_opt_set_int(encoderCtx, "slices", 4, AV_OPT_SEARCH_CHILDREN);
            av_opt_set_int(encoderCtx, "crf", 18, AV_OPT_SEARCH_CHILDREN);
            SIP_CORE_DEBUG("VP8 encoder setup: crf=18");
        } else {
            // 1- if quality is set use it
            // bitrate need to be set. The target bitrate becomes the maximum allowed bitrate
            // 2- otherwise set rc_max_rate and rc_buffer_size
            // Using information given on this page:
            // http://www.webmproject.org/docs/encoder-parameters/
            uint64_t maxBitrate = 1000 * br;
            // 200 Kbit/s    -> CRF40
            // 6 Mbit/s      -> CRF23
            uint8_t crf = (uint8_t) std::round(LOGREG_PARAM_A + LOGREG_PARAM_B * std::log(maxBitrate));
            uint64_t bufSize = maxBitrate / 2;

            av_opt_set(encoderCtx, "quality", "realtime", AV_OPT_SEARCH_CHILDREN);
            av_opt_set_int(encoderCtx, "error-resilient", 1, AV_OPT_SEARCH_CHILDREN);
            av_opt_set_int(encoderCtx,
                           "cpu-used",
                           7,
                           AV_OPT_SEARCH_CHILDREN); // value obtained from testing
            av_opt_set_int(encoderCtx, "lag-in-frames", 0, AV_OPT_SEARCH_CHILDREN);
            // allow encoder to drop frames if buffers are full and
            // to undershoot target bitrate to lessen strain on resources
            av_opt_set_int(encoderCtx, "drop-frame", 25, AV_OPT_SEARCH_CHILDREN);
            av_opt_set_int(encoderCtx, "undershoot-pct", 95, AV_OPT_SEARCH_CHILDREN);
            // don't set encoderCtx->gop_size: let libvpx decide when to insert a keyframe
            av_opt_set_int(encoderCtx, "slices", 2, AV_OPT_SEARCH_CHILDREN); // VP8E_SET_TOKEN_PARTITIONS
            av_opt_set_int(encoderCtx, "qmax", 56, AV_OPT_SEARCH_CHILDREN);
            av_opt_set_int(encoderCtx, "qmin", 4, AV_OPT_SEARCH_CHILDREN);
            crf = std::clamp((int) crf, 4, 56);
            av_opt_set_int(encoderCtx, "crf", crf, AV_OPT_SEARCH_CHILDREN);
            av_opt_set_int(encoderCtx, "b", maxBitrate, AV_OPT_SEARCH_CHILDREN);
            av_opt_set_int(encoderCtx, "maxrate", maxBitrate, AV_OPT_SEARCH_CHILDREN);
            av_opt_set_int(encoderCtx, "bufsize", bufSize, AV_OPT_SEARCH_CHILDREN);
            SIP_CORE_DEBUG("VP8 encoder setup: crf={:d}, maxrate={:d}, bufsize={:d}",
                           crf,
                           maxBitrate / 1000,
                           bufSize / 1000);
        }
    }

    void
    MediaEncoder::initMPEG4(AVCodecContext* encoderCtx, uint64_t br)
    {
        uint64_t maxBitrate = 1000 * br;
        uint64_t bufSize = maxBitrate / 2;

        // Use CBR (set bitrate)
        encoderCtx->rc_buffer_size = bufSize;
        encoderCtx->bit_rate = encoderCtx->rc_min_rate = encoderCtx->rc_max_rate = maxBitrate;
        SIP_CORE_DEBUG("MPEG4 encoder setup: maxrate={:d}, bufsize={:d}", maxBitrate, bufSize);
    }

    void
    MediaEncoder::initH263(AVCodecContext* encoderCtx, uint64_t br)
    {
        uint64_t maxBitrate = 1000 * br;
        uint64_t bufSize = maxBitrate / 2;

        // Use CBR (set bitrate)
        encoderCtx->rc_buffer_size = bufSize;
        encoderCtx->bit_rate = encoderCtx->rc_min_rate = encoderCtx->rc_max_rate = maxBitrate;
        SIP_CORE_DEBUG("H263 encoder setup: maxrate={:d}, bufsize={:d}", maxBitrate, bufSize);
    }

    void
    MediaEncoder::initOpus(AVCodecContext* encoderCtx)
    {
        // Enable FEC support by default with 10% packet loss
        av_opt_set_int(encoderCtx, "fec", fecEnabled_ ? 1 : 0, AV_OPT_SEARCH_CHILDREN);
        av_opt_set_int(encoderCtx, "packet_loss", 10, AV_OPT_SEARCH_CHILDREN);
        av_opt_set(encoderCtx, "application", "voip", AV_OPT_SEARCH_CHILDREN);
        encoderCtx->compression_level = 10;
    }

    void
    MediaEncoder::initAccel(AVCodecContext* encoderCtx, uint64_t br)
    {
#ifdef RING_ACCEL
        if (not accel_)
        return;
    if (accel_->getName() == "nvenc"sv) {
        // Use same parameters as software
    } else if (accel_->getName() == "vaapi"sv) {
        // Use VBR encoding with bitrate target set to 80% of the maxrate
        av_opt_set_int(encoderCtx, "crf", -1, AV_OPT_SEARCH_CHILDREN);
        av_opt_set_int(encoderCtx, "b", br * 1000 * 0.8f, AV_OPT_SEARCH_CHILDREN);
    } else if (accel_->getName() == "videotoolbox"sv) {
        av_opt_set_int(encoderCtx, "b", br * 1000 * 0.8f, AV_OPT_SEARCH_CHILDREN);
    } else if (accel_->getName() == "qsv"sv) {
        // Use Video Conferencing Mode
        av_opt_set_int(encoderCtx, "vcm", 1, AV_OPT_SEARCH_CHILDREN);
        av_opt_set_int(encoderCtx, "b", br * 1000 * 0.8f, AV_OPT_SEARCH_CHILDREN);
    }
#endif
    }

    AVCodecContext*
    MediaEncoder::getCurrentVideoAVCtx()
    {
        for (auto it : encoders_) {
            if (it->codec_type == AVMEDIA_TYPE_VIDEO)
                return it;
        }
        return nullptr;
    }

    AVCodecContext*
    MediaEncoder::getCurrentAudioAVCtx()
    {
        for (auto it : encoders_) {
            if (it->codec_type == AVMEDIA_TYPE_AUDIO)
                return it;
        }
        return nullptr;
    }

    void
    MediaEncoder::stopEncoder()
    {
        flush();
        for (auto it = encoders_.begin(); it != encoders_.end(); it++) {
            if ((*it)->codec_type == AVMEDIA_TYPE_VIDEO) {
                encoders_.erase(it);
                break;
            }
        }
        AVCodecContext* encoderCtx = getCurrentVideoAVCtx();
        avcodec_free_context(&encoderCtx);
        av_free(encoderCtx);
    }

    bool
    MediaEncoder::isDynBitrateSupported(AVCodecID codecid)
    {
#ifdef RING_ACCEL
        if (accel_) {
        return accel_->dynBitrate();
    }
#endif
        if (codecid != AV_CODEC_ID_VP8)
            return true;

        return false;
    }

    bool
    MediaEncoder::isDynPacketLossSupported(AVCodecID codecid)
    {
        if (codecid == AV_CODEC_ID_OPUS)
            return true;

        return false;
    }

    void
    MediaEncoder::readConfig(AVCodecContext* encoderCtx)
    {
        std::string path = Manager::instance().getConfigPath();
        path.replace(path.find("sip"), 3, "encoder");
        std::string name = encoderCtx->codec->name;
        if (fileutils::isFile(path)) {
            SIP_CORE_WARN("encoder.json file found, default settings will be erased");
            try {
                Json::Value root;
                std::ifstream file = fileutils::ifstream(path);
                file >> root;
                if (!root.isObject()) {
                    SIP_CORE_ERR() << "Invalid encoder configuration: root is not an object";
                    return;
                }
                const auto& config = root[name];
                if (config.isNull()) {
                    SIP_CORE_WARN() << "Encoder '" << name << "' not found in configuration file";
                    return;
                }
                if (!config.isObject()) {
                    SIP_CORE_ERR() << "Invalid encoder configuration: '" << name
                                   << "' is not an object";
                    return;
                }
                for (Json::Value::const_iterator it = config.begin(); it != config.end(); ++it) {
                    Json::Value v = *it;
                    if (!it.key().isConvertibleTo(Json::ValueType::stringValue)
                        || !v.isConvertibleTo(Json::ValueType::stringValue)) {
                        SIP_CORE_ERR() << "Invalid configuration for '" << name << "'";
                        return;
                    }
                    const auto& key = it.key().asString();
                    const auto& value = v.asString();
                    int ret = av_opt_set(reinterpret_cast<void*>(encoderCtx),
                                         key.c_str(),
                                         value.c_str(),
                                         AV_OPT_SEARCH_CHILDREN);
                    if (ret < 0) {
                        SIP_CORE_ERR() << "Failed to set option " << key << " in " << name
                                       << " context: " << libav_utils::getError(ret) << "\n";
                    }
                }
            } catch (const Json::Exception& e) {
                SIP_CORE_ERR() << "Failed to load encoder configuration file: " << e.what();
            }
        }
    }

    std::string
    MediaEncoder::testH265Accel()
    {
#ifdef RING_ACCEL
        if (sip_core::Manager::instance().videoPreferences.getEncodingAccelerated()) {
        // Get compatible list of Hardware API
        auto APIs = video::HardwareAccel::getCompatibleAccel(AV_CODEC_ID_H265,
                                                             1280,
                                                             720,
                                                             CODEC_ENCODER);

        std::unique_ptr<video::HardwareAccel> accel;

        for (const auto& it : APIs) {
            accel = std::make_unique<video::HardwareAccel>(it); // save accel
            // Init codec need accel to init encoderCtx accelerated
            auto outputCodec = avcodec_find_encoder_by_name(accel->getCodecName().c_str());

            AVCodecContext* encoderCtx = avcodec_alloc_context3(outputCodec);
            encoderCtx->thread_count = std::min(std::thread::hardware_concurrency(), 16u);
            encoderCtx->thread_type = FF_THREAD_SLICE;
            encoderCtx->width = 1280;
            encoderCtx->height = 720;
            AVRational framerate;
            framerate.num = 30;
            framerate.den = 1;
            encoderCtx->time_base = av_inv_q(framerate);
            encoderCtx->pix_fmt = accel->getFormat();
            encoderCtx->profile = AV_PROFILE_HEVC_MAIN;
            encoderCtx->opaque = accel.get();

            auto br = SystemCodecInfo::DEFAULT_VIDEO_BITRATE;
            av_opt_set_int(encoderCtx, "b", br * 1000, AV_OPT_SEARCH_CHILDREN);
            av_opt_set_int(encoderCtx, "maxrate", br * 1000, AV_OPT_SEARCH_CHILDREN);
            av_opt_set_int(encoderCtx, "minrate", br * 1000, AV_OPT_SEARCH_CHILDREN);
            av_opt_set_int(encoderCtx, "bufsize", br * 500, AV_OPT_SEARCH_CHILDREN);
            av_opt_set_int(encoderCtx, "crf", -1, AV_OPT_SEARCH_CHILDREN);

            auto ret = accel->initAPI(false, nullptr);
            if (ret < 0) {
                accel.reset();
                ;
                encoderCtx = nullptr;
                continue;
            }
            accel->setDetails(encoderCtx);
            if (avcodec_open2(encoderCtx, outputCodec, nullptr) < 0) {
                // Failed to open codec
                SIP_CORE_WARN("Fail to open hardware encoder H265 with %s ", it.getName().c_str());
                avcodec_free_context(&encoderCtx);
                encoderCtx = nullptr;
                accel = nullptr;
                continue;
            } else {
                // Succeed to open codec
                avcodec_free_context(&encoderCtx);
                encoderCtx = nullptr;
                accel = nullptr;
                return it.getName();
            }
        }
    }
#endif
        return "";
    }

#ifdef ENABLE_VIDEO

// make everything null and send it as dummy packet
    void
    MediaEncoder::sendDummyPacket()
    {
        AVPacket pkt;
        av_init_packet(&pkt);
        pkt.data = nullptr;
        pkt.size = 0;
        send(pkt, -1, true);
    }

    int
    MediaEncoder::getHWFrame(const std::shared_ptr<VideoFrame>& input,
                             std::shared_ptr<VideoFrame>& output)
    {
        try {
#if defined(TARGET_OS_IOS) && TARGET_OS_IOS
            // iOS
        // if (accel_) {
        //     auto pix = accel_->getSoftwareFormat();
        //     if (input->format() != pix) {
        //         output = scaler_.convertFormat(*input.get(), pix);
        //     } else {
        //         // Fully accelerated pipeline, skip main memory
        //         output = input;
        //     }
        // } else {
        output = getScaledSWFrame(*input.get());
        // }
#elif !defined(__APPLE__) && defined(RING_ACCEL)
            // Other Platforms
        auto desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(input->format()));
        bool isHardware = desc && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL);
        if (accel_ && accel_->isLinked() && isHardware) {
            // Fully accelerated pipeline, skip main memory
            output = input;
        } else if (isHardware) {
            // Hardware decoded frame, transfer back to main memory
            // Transfer to GPU if we have a hardware encoder
            // Hardware decoders decode to NV12, but sip_core's supported software encoders want YUV420P
            output = getUnlinkedHWFrame(*input.get());
        } else if (accel_) {
            // Software decoded frame with a hardware encoder, convert to accepted format first
            output = getHWFrameFromSWFrame(*input.get());
        } else {
            output = getScaledSWFrame(*input.get());
        }
#else
            // macOS
            output = getScaledSWFrame(*input.get());
#endif
        } catch (const std::runtime_error& e) {
            SIP_CORE_ERR("Accel failure: %s", e.what());
            return -1;
        }

        return 0;
    }

#ifdef RING_ACCEL
std::shared_ptr<VideoFrame>
MediaEncoder::getUnlinkedHWFrame(const VideoFrame& input)
{
    AVPixelFormat pix = (accel_ ? accel_->getSoftwareFormat() : AV_PIX_FMT_NV12);
    std::shared_ptr<VideoFrame> framePtr = video::HardwareAccel::transferToMainMemory(input, pix);
    if (!accel_) {
        framePtr = scaler_.convertFormat(*framePtr, AV_PIX_FMT_YUV420P);
    } else {
        framePtr = accel_->transfer(*framePtr);
    }
    return framePtr;
}

std::shared_ptr<VideoFrame>
MediaEncoder::getHWFrameFromSWFrame(const VideoFrame& input)
{
    std::shared_ptr<VideoFrame> framePtr;
    auto pix = accel_->getSoftwareFormat();
    if (input.format() != pix) {
        framePtr = scaler_.convertFormat(input, pix);
        framePtr = accel_->transfer(*framePtr);
    } else {
        framePtr = accel_->transfer(input);
    }
    return framePtr;
}
#endif

    std::shared_ptr<VideoFrame>
    MediaEncoder::getScaledSWFrame(const VideoFrame& input)
    {
        libav_utils::fillWithBlack(scaledFrame_->pointer());

        if (videoOpts_.noColor) {
            libav_utils::fillWithBlack(grayScaledFrame_->pointer());
            grayScaler_.scale_with_aspect(input, *grayScaledFrame_);
            scaler_.scale_with_aspect(*grayScaledFrame_, *scaledFrame_);
        } else {
            scaler_.scale_with_aspect(input, *scaledFrame_);
        }
        return scaledFrame_;
    }
#endif

    void
    MediaEncoder::resetStreams(int width, int height)
    {
        videoOpts_.width = width;
        videoOpts_.height = height;

        try {
            flush();
            initialized_ = false;
            if (outputCtx_) {
                for (auto encoderCtx : encoders_) {
                    if (encoderCtx) {
                        avcodec_free_context(&encoderCtx);
                    }
                }
                encoders_.clear();
            }
        } catch (...) {
        }
    }

    bool
    MediaEncoder::sendBuffer(uint8_t* buf1, unsigned int len, unsigned int samples, int flags)
    {
        AVPacket pkt;
        av_init_packet(&pkt);
        pkt.data = buf1;
        pkt.size = len;
        pkt.flags = flags;

        auto currentEncoder = encoders_[currentStreamIdx_];

        pkt.pts = sent_samples - currentEncoder->initial_padding;
        pkt.dts = sent_samples - currentEncoder->initial_padding;

        sent_samples += samples;

        return send(pkt, -1);
    }

} // namespace sip_core