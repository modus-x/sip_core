#include "g729_decoder.h"

#include <thread>

namespace sip_core {

#define S16_PACKET_SIZE 80
#define G729_PACKET_SIZE 10
#define G729_SID_SIZE 2

g729MediaDecoder::
g729MediaDecoder()
{
    SIP_CORE_DBG("[%p] New instance created", this);
}

g729MediaDecoder::
g729MediaDecoder(MediaObserver o) :
    callback_(std::move(o))
{
    SIP_CORE_DBG("[%p] New instance created", this);
}

g729MediaDecoder::~
g729MediaDecoder()
{
    if (context_)
        closeBcg729DecoderChannel(context_);

    SIP_CORE_DBG("[%p] Instance destroyed", this);
}

void g729MediaDecoder::emulateRate()
{
    return;
}

int
g729MediaDecoder::openInput(const DeviceParams& p)
{
    if (p.format != "sdp") {
        SIP_CORE_ERR() << "G729 decoder supports only sdp format";
        return -1;
    }

    std::lock_guard<std::mutex> lock(mut_);
    context_ = initBcg729DecoderChannel();

    if (!context_) {
        SIP_CORE_ERR() << "Can not create G.729 decoder context";
        return -1;
    }

    return 0;
}

void
g729MediaDecoder::setInterruptCallback(int (*cb)(void*), void* opaque)
{

}

void
g729MediaDecoder::setIOContext(MediaIOHandle* ioctx)
{
    std::lock_guard<std::mutex> lock(mut_);
    if(ioctx && ioctx->getContext() && ioctx->getContext()->opaque)
        sp_ = static_cast<SocketPair*>(ioctx->getContext()->opaque);
}

void
g729MediaDecoder::enableLateFrameDrop(std::chrono::microseconds threshold)
{
    return;
}

int
g729MediaDecoder::setup(AVMediaType type)
{
    std::lock_guard<std::mutex> lock(mut_);
    startTime_ = av_gettime();
    if(!sp_ || !context_)
        return -1;

    uint8_t buffer[256];
    int len;
    do {
        len = sp_->readData(buffer, sizeof(buffer));
        if(len < 0) return -1;
        
        std::vector<G729RtpPayload> packets;
        auto res = readRtp(buffer, len, packets);
        if(res < 0)
            return -1;

    } while(len == 0);

    return 0;
}

int g729MediaDecoder::setupAudio()
{
    return setup(AVMediaType::AVMEDIA_TYPE_AUDIO);
}

int g729MediaDecoder::setupVideo()
{
    SIP_CORE_ERR("G.729 is not a video codec");
    return -1;
}

DecodeStatus
g729MediaDecoder::decode()
{
    std::lock_guard<std::mutex> lock(mut_);
    if(!sp_ || !context_)
        return DecodeStatus::RestartRequired;

    // avg g.729 packet size is 160 bites
    // so 256 bites buffer sould be fine
    uint8_t buffer[256];
    auto len = sp_->readDataNoBlock(buffer, sizeof(buffer));
    if(len <= 0 && ((av_gettime() - lastPacketTime_) < 10000)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return DecodeStatus::Success;
    }

    std::vector<G729RtpPayload> packets;
    auto res = readRtp(buffer, len, packets);
    if(res < 0)
        return DecodeStatus::ReadError;

    for(auto p : packets) {
        if(decodePacket(&p))
            return DecodeStatus::DecodeError;
    }

    return DecodeStatus::FrameFinished;
}

DecodeStatus
g729MediaDecoder::flush()
{
    return DecodeStatus::Success;
}

int
g729MediaDecoder::getWidth() const
{
    return 0;
}

int
g729MediaDecoder::getHeight() const
{
    return  0;
}

std::string
g729MediaDecoder::getDecoderName() const
{
    return "G729";
}

rational<double>
g729MediaDecoder::getFps() const
{
    return 0;
}

AVPixelFormat
g729MediaDecoder::getPixelFormat() const
{
    return AV_PIX_FMT_NONE;
}

void
g729MediaDecoder::updateStartTime(int64_t startTime)
{
    std::lock_guard<std::mutex> lock(mut_);
    startTime_ = startTime;
}

void
g729MediaDecoder::emitFrame(bool isAudio)
{
    return;
}

void
g729MediaDecoder::flushBuffers()
{
    return;
}

void
g729MediaDecoder::setSeekTime(int64_t time)
{
    std::lock_guard<std::mutex> lock(mut_);
    seekTime_ = time;
}

#ifdef RING_ACCEL
void
g729MediaDecoder::enableAccel(bool enableAccel)
{
    return;
}
#endif

MediaStream
g729MediaDecoder::getStream(std::string name) const
{
    return MediaStream(name, AV_SAMPLE_FMT_S16, rational<int>(1, 100), 8000, 1, 80);
}

void 
g729MediaDecoder::setResolutionChangedCallback(std::function<void(int, int)> cb)
{
    return;
}

void
g729MediaDecoder::setFEC(bool enable)
{
    return;
}

void
g729MediaDecoder::setContextCallback(const std::function<void()>& cb)
{
    firstDecode_.exchange(true);
    contextCallback_ = cb;
}

int
g729MediaDecoder::readRtp(uint8_t* buf, int buf_size, std::vector<G729RtpPayload>& packets)
{
    if(buf_size < 14)
        return -1;

    if((buf[0] >> 6) != 2 ||
       (buf[1] & 0x7f) != 18)
        return -1;

    bool has_padding = (buf[0] & 0x20) == 0x20;
    if(has_padding) 
        buf_size -= buf[buf_size - 1];
    
    // we dont need csrc but we need it to calculate header size
    const int csrc_count = buf[0] & 0x0f;
    const int header_size = 12 + csrc_count * 4;
    int payload_size = buf_size - header_size;
    if(payload_size <= 0)
        return -1;

    // skip extention data as per 
    // RFC 3550 Section 5.3.1 RTP Header Extension handling
    uint16_t ext_size = 0;
    if(buf[0] & 0x10) {
        if (payload_size < 4)
            return -1;
        
        ext_size = ((buf[header_size + 2] << 8) | buf[header_size + 3]) + 1;
        ext_size = ext_size << 2;
        payload_size -= ext_size;
    }

    if(payload_size <= 0)
        return -1;

    bool has_sid = (payload_size - G729_SID_SIZE) % 10 == 0;
    if( payload_size % 10 != 0 && !has_sid)
        return -1;

    uint16_t seq_val = (buf[2] << 8) | buf[3];

    uint32_t timestamp = ((uint32_t)buf[4] << 24) |
                         ((uint32_t)buf[5] << 16) |
                         ((uint32_t)buf[6] << 8)  |
                                    buf[7];

    uint32_t ssrc = ((uint32_t)buf[8]  << 24) |
                    ((uint32_t)buf[9]  << 16) |
                    ((uint32_t)buf[10] << 8)  |
                               buf[11];

    int mismatch = static_cast<int>(seq_val) - static_cast<int>(lastFrameSeq_) - 1;
    if(mismatch < 0)
        return 0;

    packets.reserve(packets.size() + payload_size / 10);

    int tail;
    for (tail = header_size + ext_size; tail < buf_size - 2; tail += 10) {
        packets.push_back( {false, seq_val, timestamp, &buf[tail]} );
        timestamp += 80;
    }

    // VAD
    if((buf_size - tail) == 2) {
        packets.push_back( {true, seq_val, timestamp, &buf[tail]} );
        timestamp += 80;
    }

    lastFrameSeq_ = seq_val;
    
    return 0;
}

int
g729MediaDecoder::createAVFrame(AVFrame* frame, int16_t* buf)
{
    if(!frame)
        return -1;
    
    auto now = av_gettime();
    av_channel_layout_default(&frame->ch_layout, 1);
    frame->time_base = { 1, 8000 }; // 8 kHz audio by the standart
    frame->format = AV_SAMPLE_FMT_S16;
    frame->sample_rate = 8000;
    frame->nb_samples = 80;
    frame->pts = av_rescale_q_rnd(now - startTime_,
                                  {1, AV_TIME_BASE},
                                  frame->time_base,
                                  static_cast<AVRounding>(AV_ROUND_NEAR_INF
                                                          | AV_ROUND_PASS_MINMAX));


    if (av_frame_get_buffer(frame, 0) < 0) { 
        return -1;
    }

    memcpy((void*)frame->data[0], buf, S16_PACKET_SIZE * 2);
    lastPacketTime_ = now;

    return 0;
}

int 
g729MediaDecoder::decodePacket(G729RtpPayload* packet)
{
    int16_t output[S16_PACKET_SIZE];
    if(packet)
        bcg729Decoder(context_,
                      packet->payloadBuff,
                      packet->isSID ? G729_SID_SIZE : G729_PACKET_SIZE,
                      0, (uint8_t)packet->isSID, 0, output);
    else
        bcg729Decoder(context_, NULL, 0, 1, 0, 0, output);
    
    auto frame = std::static_pointer_cast<MediaFrame>(std::make_shared<AudioFrame>());
    if(createAVFrame(frame->pointer(), output))
        return -1;

    if (callback_)
        callback_(std::move(frame));

    if (contextCallback_ && firstDecode_.load()) {
        firstDecode_.exchange(false);
        contextCallback_();
    }
    return 0;
}


} // namespace sip_core