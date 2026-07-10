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
 
 #ifdef RQM
 #include <fstream>
 #endif
 
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
 
 class MediaEncoderBase
 {
 public:
     virtual ~MediaEncoderBase() = default;
     virtual void openOutput(const std::string& filename, const std::string& format = "") = 0;
     virtual void setMetadata(const std::string& title, const std::string& description) = 0;
 
     // set media stream parameters (video width, height, audio sample rate, etc)
     virtual void setOptions(const MediaStream& opts) = 0;
 
     // set media description parameters (payload type, video mode)
     virtual void setOptions(const MediaDescription& args) = 0;
 
     // add steam to context with some predefined codec info
 
     virtual int addStream(const SystemCodecInfo& codec) = 0;
     virtual void setIOContext(AVIOContext* ioctx) = 0;
     virtual void resetStreams(int width, int height) = 0;
 
     virtual bool send(AVPacket& packet, int streamIdx = -1, bool dummy = false) = 0;
 
     // send raw data
     virtual bool sendBuffer(uint8_t* buf1, unsigned int len, unsigned int samples, int flags) = 0;
 
 #ifdef ENABLE_VIDEO
     virtual int encode(const std::shared_ptr<VideoFrame>& input, bool is_keyframe, int64_t frame_number) = 0;
 #endif // ENABLE_VIDEO
 
     virtual int encodeAudio(AudioFrame& frame) = 0;
 
     // frame should be ready to be sent to the encoder at this point
     virtual int encode(AVFrame* frame, int streamIdx) = 0;
 
     virtual int flush() = 0;
     virtual std::string print_sdp() = 0;
 
     /* getWidth and getHeight return size of the encoded frame.
      * Values have meaning only after openLiveOutput call.
      */
     virtual int getWidth() const = 0;
     virtual int getHeight() const = 0;
 
     virtual void setInitSeqVal(uint16_t seqVal) = 0;
     virtual uint16_t getLastSeqValue() = 0;
 
     virtual const std::string& getAudioCodec() const = 0;
     virtual const std::string& getVideoCodec() const = 0;
 
     virtual int setBitrate(uint64_t br) = 0;
     virtual int setPacketLoss(uint64_t pl) = 0;
 
 #ifdef RING_ACCEL
     virtual void enableAccel(bool enableAccel) = 0;
 #endif
 
     virtual unsigned getStreamCount() const = 0;
     virtual MediaStream getStream(const std::string& name, int streamIdx = -1) const = 0;
     virtual void sendDummyPacket() = 0;
 
     virtual void setSource(const std::string& source) = 0;
 };
 
 } // namespace sip_core
 