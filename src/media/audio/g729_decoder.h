 #pragma once
 
 #include "media_decoder_base.h"
 #include "socket_pair.h"

 extern "C" {
 #include "bcg729/decoder.h"
 }

 namespace sip_core {
 
 #ifdef RING_ACCEL
 namespace video {
 class HardwareAccel;
 }
 #endif
    
 class g729MediaDecoder final : public MediaDecoderBase
 {
 public:
     g729MediaDecoder();
     g729MediaDecoder(MediaObserver observer);
     ~g729MediaDecoder();
 
     void emulateRate() override;
 
     /// just forward to demuxer
     int openInput(const DeviceParams&) override;
     /// just forward to demuxer
     void setInterruptCallback(int (*cb)(void*), void* opaque) override;
     /// just forward to demuxer
     void setIOContext(MediaIOHandle* ioctx) override;
     void enableLateFrameDrop(std::chrono::microseconds threshold) override;
 
     int setup(AVMediaType type) override;
     int setupAudio() override;
     int setupVideo() override;
 
     // forward to demuxer. at the end, if stream was setup correctly, MediaDecoder's decode ,ethod will be called
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
 
     void setResolutionChangedCallback(std::function<void(int, int)> cb) override;
 
     void setFEC(bool enable) override;
 
     void setContextCallback(const std::function<void()>& cb) override;
 
 private:
     struct G729RtpPayload
     {
         bool isSID;
         uint16_t seq;
         uint32_t timestamp;

         uint8_t* payloadBuff;
     };

     int readRtp(uint8_t* buf, int buf_size, std::vector<G729RtpPayload>& packets);
     int createAVFrame(AVFrame* frame, int16_t* buf);
     int decodePacket(G729RtpPayload* packet);

     std::mutex mut_;
     MediaObserver callback_;
     int64_t startTime_ = AV_NOPTS_VALUE;
     int64_t lastPacketTime_ = AV_NOPTS_VALUE;
     uint16_t lastFrameSeq_ = 0;
     int64_t seekTime_ = 0;
     SocketPair* sp_ = nullptr;
     
     std::function<void()> contextCallback_;
     std::atomic_bool firstDecode_ {true};

     bcg729DecoderChannelContextStruct* context_ = nullptr;
 };
 
 } // namespace sip_core
 