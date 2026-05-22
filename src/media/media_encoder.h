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

 #include "media_encoder_base.h"

 #include <mutex>
 
 namespace sip_core {
 
 #ifdef RING_ACCEL
 namespace video {
 class HardwareAccel;
 }
 #endif
 
 class MediaEncoder final : public MediaEncoderBase
 {
 public:
     MediaEncoder();
     ~MediaEncoder();
 
     void openOutput(const std::string& filename, const std::string& format = "") override;
     void setMetadata(const std::string& title, const std::string& description) override;
 
     // set media stream parameters (video width, height, audio sample rate, etc)
     void setOptions(const MediaStream& opts) override;
 
     // set media description parameters (payload type, video mode)
     void setOptions(const MediaDescription& args) override;
 
     // add steam to context with some predefined codec info
 
     int addStream(const SystemCodecInfo& codec) override;
     void setIOContext(AVIOContext* ioctx) override { ioCtx_ = ioctx; }
     void resetStreams(int width, int height) override;
 
     bool send(AVPacket& packet, int streamIdx = -1, bool dummy = false) override;
 
     // send raw data
     bool sendBuffer(uint8_t* buf1, unsigned int len, unsigned int samples, int flags) override;
 
 #ifdef ENABLE_VIDEO
     int encode(const std::shared_ptr<VideoFrame>& input, bool is_keyframe, int64_t frame_number) override;
 #endif // ENABLE_VIDEO
 
     int encodeAudio(AudioFrame& frame) override;
 
     // frame should be ready to be sent to the encoder at this point
     int encode(AVFrame* frame, int streamIdx) override;
 
     int flush() override;
     std::string print_sdp() override;
 
     /* getWidth and getHeight return size of the encoded frame.
      * Values have meaning only after openLiveOutput call.
      */
     int getWidth() const override { return videoOpts_.width; };
     int getHeight() const override { return videoOpts_.height; };
 
     void setInitSeqVal(uint16_t seqVal) override;
     uint16_t getLastSeqValue() override;
 
     const std::string& getAudioCodec() const override { return audioCodec_; }
     const std::string& getVideoCodec() const override { return videoCodec_; }
 
     int setBitrate(uint64_t br) override;
     int setPacketLoss(uint64_t pl) override;
 
 #ifdef RING_ACCEL
     void enableAccel(bool enableAccel) override;
 #endif
 
     static std::string testH265Accel();
 
     unsigned getStreamCount() const override;
     MediaStream getStream(const std::string& name, int streamIdx = -1) const override;
     void sendDummyPacket() override;
 
     void setSource(const std::string& source) override { source_ = source; }
 
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
     int h264CrfFromQuality() const;
     void initH265(AVCodecContext* encoderCtx, uint64_t br);
     void initVP8(AVCodecContext* encoderCtx, uint64_t br);
     void initMPEG4(AVCodecContext* encoderCtx, uint64_t br);
     void initH263(AVCodecContext* encoderCtx, uint64_t br);
     void initOpus(AVCodecContext* encoderCtx);
     bool isDynBitrateSupported(AVCodecID codecid);
     bool isDynPacketLossSupported(AVCodecID codecid);
     void initAccel(AVCodecContext* encoderCtx, uint64_t br);
 #ifdef RQM
     int writeContainerToRtp(const uint8_t* buf, int buf_size);
 #endif
 
 #ifdef ENABLE_VIDEO
     int getHWFrame(const std::shared_ptr<VideoFrame>& input, std::shared_ptr<VideoFrame>& output);
     std::shared_ptr<VideoFrame> getUnlinkedHWFrame(const VideoFrame& input);
     std::shared_ptr<VideoFrame> getHWFrameFromSWFrame(const VideoFrame& input);
     std::shared_ptr<VideoFrame> getScaledSWFrame(const VideoFrame& input);
 #endif
 
     // encode data into h264 / something another
     std::vector<AVCodecContext*> encoders_;
 
     // output from encoder. it may be rtp or file
     AVFormatContext* outputCtx_ = NULL;
 
 #ifdef RQM
     // output to mp4. only local file url
     AVFormatContext* mp4Ctx_ = NULL;

     // bytes with mp4 will be written here
     AVIOContext *mp4IOCtx_ = NULL;

     unsigned int mp4SentPackets_ {1};

     // Monotonic per-frame counter used to compute correct pkt.pts/dts/duration
     // in stream time-base before feeding fragments to the mp4 muxer.
     // See the override block in encode() for why this is necessary.
     int64_t mp4FramesEncoded_ {0};

     // Wall-clock anchor for the first encoded frame. Each subsequent frame's
     // pts is derived from (steady_clock::now() - mp4RecordingStart_), so
     // the file's timeline matches the real recording duration even when
     // x11grab can't keep up with the configured framerate.
     std::chrono::steady_clock::time_point mp4RecordingStart_ {};
     int64_t mp4LastFramePts_ {0};

     // codec for mp4
     AVCodecContext* mp4CodecContext_ = NULL;

     // Captured fmp4 init segment (ftyp + moov). Filled while
     // capturingInitSegment_ is true and avio_flush() drains the mp4 muxer's
     // initial write into our writeContainerToRtp callback. Kept around so
     // the local-file mirror (mp4LocalFile_) can include the same init bytes
     // as the RTP stream, even though they're emitted only once at startIO.
     std::vector<uint8_t> initSegment_;
     bool                 capturingInitSegment_ {false};

     // Optional local-file mirror of the fmp4 stream. When the env var
     // RQM_LOCAL_RECORDING_DIR is set, the same bytes that go onto the RTP
     // wire (ftyp+moov init segment plus every moof+mdat fragment) also get
     // appended to a local .mp4 file in that directory. Useful for:
     //   * diagnosing RTP-server drops (compare local vs received .rsf),
     //   * always having a playable recording on the daemon's host machine.
     // Set to NULL when the env var isn't configured.
     FILE*                mp4LocalFile_ {nullptr};
     // Path of the file pointed to by mp4LocalFile_. Kept so startIO() can
     // unlink an unplayable artifact if the muxer rejected our movflags
     // (e.g. an older libavformat that doesn't know +frag_every_frame
     // would also reject +empty_moov as collateral, leaving us with a
     // ftyp-only file that no player will accept).
     std::string          mp4LocalFilePath_;

     // Tracks whether avformat_write_header(mp4Ctx_) succeeded. Unlike
     // outputCtx_'s write_header (which throws on failure), the mp4
     // muxer's write_header is treated as soft-fail and only logged.
     // The destructor gates av_write_trailer(mp4Ctx_) on this so we
     // never invoke trailer on a half-initialised muxer.
     bool                 mp4HeaderWritten_ {false};

     // stream for mp4
     AVStream *mp4Stream_ = NULL;
 
     std::ofstream mp4FileStream_;
 
     std::string mp4File_;
 
     AVDictionary *mp4Opts_ = NULL;
 #endif
 
     AVIOContext* ioCtx_ = nullptr;
     int currentStreamIdx_ = -1;
     unsigned sent_samples = 0;
     bool initialized_ {false};
     bool fileIO_ {false};
     unsigned int currentVideoCodecID_ {0};
     const AVCodec* outputCodec_ = nullptr;
     std::recursive_mutex encMutex_;
     bool linkableHW_ {false};
     RateMode mode_ {RateMode::CBR};
     bool fecEnabled_ {true};
 
 #ifdef ENABLE_VIDEO
     video::VideoScaler scaler_;
     video::VideoScaler grayScaler_;
 
     std::shared_ptr<VideoFrame> scaledFrame_;
     std::shared_ptr<VideoFrame> grayScaledFrame_;
 #endif // ENABLE_VIDEO
 
     std::vector<uint8_t> scaledFrameBuffer_;
     int scaledFrameBufferSize_ = 0;
 
     std::vector<uint8_t> grayScaledFrameBuffer_;
     int grayScaledFrameBufferSize_ = 0;
 
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
 
     std::string source_;
 };
 
 } // namespace sip_core
 