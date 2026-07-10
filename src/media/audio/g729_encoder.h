 #pragma once
 
 #include "media_encoder_base.h"
 #include "socket_pair.h"
 
 extern "C" {
 #include "bcg729/encoder.h"
 }

 namespace sip_core {
 
 #ifdef RING_ACCEL
 namespace video {
 class HardwareAccel;
 }
 #endif
    
 class g729MediaEncoder : public MediaEncoderBase
 {
 public:
     g729MediaEncoder(int max_buffer_size, bool annexB = false);
     virtual ~g729MediaEncoder();
 
     void openOutput(const std::string& filename, const std::string& format = "") override;
     void setMetadata(const std::string& title, const std::string& description) override;
 
     // set media stream parameters (video width, height, audio sample rate, etc)
     void setOptions(const MediaStream& opts) override;
 
     // set media description parameters (payload type, video mode)
     void setOptions(const MediaDescription& args) override;
 
     // add steam to context with some predefined codec info
 
     int addStream(const SystemCodecInfo& codec) override;
     void setIOContext(AVIOContext* ioctx) override;
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
     int getWidth() const override { return 0; };
     int getHeight() const override { return 0; };
 
     void setInitSeqVal(uint16_t seqVal) override;
     uint16_t getLastSeqValue() override;

     #ifdef RING_ACCEL
     void enableAccel(bool enableAccel) override;
     #endif
 
     const std::string& getAudioCodec() const override { static const std::string codec{"G.729"}; return codec; }
     const std::string& getVideoCodec() const override { return empty_string; }
 
     int setBitrate(uint64_t br) override;
     int setPacketLoss(uint64_t pl) override;
 
     unsigned getStreamCount() const override;
     MediaStream getStream(const std::string& name, int streamIdx = -1) const override;
     void sendDummyPacket() override;
 
     void setSource(const std::string& source) override { source_ = source; }

 private:
     int initHeader(int8_t* buffer);
     int encode_packet(const int16_t* packet);
     int send(uint8_t* buffer, int size);
    
     // evil hack for string ref function return
     std::mutex mut_;
     const std::string empty_string;
     std::string source_;
     MediaStream opts_;
     bool annexB_;

    uint32_t timestamp_ = 0;
    uint32_t dtmfTimestamp_ = 0;
    uint32_t ssrc_ = 0;
     uint16_t seq_val_ = 0;
     uint64_t packet_loss_ = 0;
     
     uint8_t* buffer_ = nullptr;
     uint16_t buffer_size_;
     uint16_t packets_in_rtp;
     uint16_t packet_in_rtp_idx = 0;

     uint16_t packetQueue_[80];
     uint16_t packetQueueIdx = 0;

     SocketPair* sp_ = nullptr;

     bcg729EncoderChannelContextStruct* context_ = nullptr;
 };
 
 } // namespace sip_core
 