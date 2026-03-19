#include "g729_encoder.h"

#define RTP_HEADER_SIZE 12
#define G729_PACKET_SIZE 10
#define G729_VAD_SIZE 2

namespace sip_core {

    g729MediaEncoder::g729MediaEncoder(int max_buffer_size, bool annexB) :
        annexB_(annexB),
        buffer_size_(max_buffer_size)
    {
        if (max_buffer_size < (RTP_HEADER_SIZE + G729_PACKET_SIZE))
            SIP_CORE_ERR() << "Buffer is too small";
        
        // we prefer
        if(max_buffer_size >= (RTP_HEADER_SIZE + 2 * G729_PACKET_SIZE)) {
            packets_in_rtp = 2;
            buffer_ = (uint8_t*)malloc(RTP_HEADER_SIZE + 2 * G729_PACKET_SIZE);
        }
        else {
            packets_in_rtp = 1;
            buffer_ = (uint8_t*)malloc(RTP_HEADER_SIZE + G729_PACKET_SIZE);
        }
        
        if (!buffer_)
            SIP_CORE_ERR() << "Failed to create G.729 encoder. Out of memory!";

        std::srand(static_cast<unsigned>(std::time(nullptr)));
        ssrc_ = std::rand();
        timestamp_ = std::rand();

        if(initHeader((int8_t*)buffer_))
            SIP_CORE_ERR() << "Failed to init G.729 rtp header";

        SIP_CORE_DBG("[%p] New instance created", this);
    }

    g729MediaEncoder::~g729MediaEncoder()
    {
        free(buffer_);

        if (context_)
            closeBcg729EncoderChannel(context_);

        SIP_CORE_DBG("[%p] Instance destroyed", this);
    }

    void
    g729MediaEncoder::openOutput(const std::string& filename, const std::string& format)
    {
        if(format != "rtp") {
            SIP_CORE_ERR() << "G729 encoder supports only RTP format";
            return;
        }

        std::lock_guard<std::mutex> lock(mut_);
        context_ = initBcg729EncoderChannel(annexB_);

        if (!context_)
            SIP_CORE_ERR() << "Can not create G.729 encoder context";
    }

    void
    g729MediaEncoder::setMetadata(const std::string& title, const std::string& description)
    {
        return;
    }

    void
    g729MediaEncoder::setOptions(const MediaStream& opts)
    {
        std::lock_guard<std::mutex> lock(mut_);
        if (!opts.isValid()) {
            SIP_CORE_ERR() << "Invalid options";
            return;
        }

        opts_ = opts;
    }

    void
    g729MediaEncoder::setOptions(const MediaDescription& args)
    {
        return;
    }

    int
    g729MediaEncoder::addStream(const SystemCodecInfo& systemCodecInfo)
    {
        SIP_CORE_ERR() << "G.729 encoder supports only one audio stream";
        return -1;
    }

    void 
    g729MediaEncoder::setIOContext(AVIOContext* ioctx)
    { 
        if(ioctx && ioctx->opaque)
            sp_ = static_cast<SocketPair*>(ioctx->opaque);
    }

    void
    g729MediaEncoder::resetStreams(int width, int height)
    {
        opts_ = MediaStream();
        return;
    }

    bool
    g729MediaEncoder::send(AVPacket& pkt, int streamIdx, bool dummy)
    {

        return false;
    }

    bool
    g729MediaEncoder::sendBuffer(uint8_t* buf1, unsigned int len, unsigned int samples, int flags)
    {
        if (!sp_ || len != 4)
            return false;

        constexpr uint8_t TELEPHONE_EVENT_PT = 101;
        constexpr int DTMF_PKT_SIZE = RTP_HEADER_SIZE + 4;

        uint8_t pkt[DTMF_PKT_SIZE];

        // If "new timestamp" flag is set, capture event start timestamp
        if (flags & 64)
            dtmfTimestamp_ = timestamp_;

        // RTP header
        pkt[0]  = 2 << 6;  // V=2, P=0, X=0, CC=0
        pkt[1]  = TELEPHONE_EVENT_PT;
        if (flags & 128)    // marker bit
            pkt[1] |= 0x80;
        pkt[2]  = (uint8_t)(seq_val_ >> 8);
        pkt[3]  = (uint8_t)(seq_val_);
        pkt[4]  = (uint8_t)(dtmfTimestamp_ >> 24);
        pkt[5]  = (uint8_t)(dtmfTimestamp_ >> 16);
        pkt[6]  = (uint8_t)(dtmfTimestamp_ >> 8);
        pkt[7]  = (uint8_t)(dtmfTimestamp_);
        pkt[8]  = (uint8_t)(ssrc_ >> 24);
        pkt[9]  = (uint8_t)(ssrc_ >> 16);
        pkt[10] = (uint8_t)(ssrc_ >> 8);
        pkt[11] = (uint8_t)(ssrc_);

        // DTMF payload (4 bytes: event, volume, duration_hi, duration_lo)
        memcpy(&pkt[RTP_HEADER_SIZE], buf1, 4);

        seq_val_++;
        timestamp_ += samples;

        int ret;
        do {
            ret = sp_->writeData(pkt, DTMF_PKT_SIZE);
        } while (ret < 0 && errno == EAGAIN);

        return ret >= 0;
    }

    #ifdef ENABLE_VIDEO
    int 
    g729MediaEncoder::encode(const std::shared_ptr<VideoFrame>& input, bool is_keyframe, int64_t frame_number) 
    {
        return 0;
    }
    #endif // ENABLE_VIDEO

    int
    g729MediaEncoder::encodeAudio(AudioFrame& frame)
    {
        if(frame.getFormat().sampleFormat != AV_SAMPLE_FMT_S16 ||
           frame.getFormat().sample_rate != 8000 ||
           frame.getFormat().nb_channels != 1) { // todo
            SIP_CORE_ERR() << "G.729 encoder received unsupported format";
            return -1;
        }

        std::lock_guard<std::mutex> lock(mut_);
        encode(frame.pointer(), 0);
        return 0;
    }

    int
    g729MediaEncoder::encode(AVFrame* frame, int streamIdx)
    {
        (void)streamIdx;

        int samples_pressesed = 0;
        while(samples_pressesed < frame->nb_samples)
        {
            auto nb_samples_left = frame->nb_samples - samples_pressesed;
            if(packetQueueIdx + nb_samples_left >= 80) {
                auto samples = 80 - packetQueueIdx;
                memcpy(&packetQueue_[packetQueueIdx], frame->data[0] + (samples_pressesed * 2), samples * 2);
                samples_pressesed += samples;
                packetQueueIdx = 0;

                encode_packet((int16_t*)packetQueue_);
            }
            else {
                memcpy(&packetQueue_[packetQueueIdx], frame->data[0] + (samples_pressesed * 2), nb_samples_left * 2);
                samples_pressesed += nb_samples_left;
                packetQueueIdx += nb_samples_left;
                break;
            }
        }

        return 0;
    }

    int
    g729MediaEncoder::flush()
    {
        // reset queues
        packet_in_rtp_idx = 0;
        packetQueueIdx = 0;

        return 0;
    }

    std::string
    g729MediaEncoder::print_sdp()
    {
        return "";
    }

    void
    g729MediaEncoder::setInitSeqVal(uint16_t seqVal)
    {
        // only set not default value (!=0)
        seq_val_ = seqVal;
    }

    uint16_t
    g729MediaEncoder::getLastSeqValue()
    {
        return seq_val_;
    }

    #ifdef RING_ACCEL
    void
    g729MediaEncoder::enableAccel(bool enableAccel)
    {
        return;
    }
    #endif

    int
    g729MediaEncoder::setBitrate(uint64_t br)
    {
        SIP_CORE_ERR() << "G.729 encoder supports only fixed bitrate";
        return 0;
    }

    int
    g729MediaEncoder::setPacketLoss(uint64_t pl)
    {
        packet_loss_ = pl;
        return 1;
    }

    unsigned
    g729MediaEncoder::getStreamCount() const
    {
        return 1;
    }

    MediaStream
    g729MediaEncoder::getStream(const std::string& name, int streamIdx) const
    {
        return opts_;
    }
    
    void
    g729MediaEncoder::sendDummyPacket()
    {
        AVPacket* pkt = av_packet_alloc();
        if (!pkt)
            return;
        send(*pkt, -1, true);
        av_packet_free(&pkt);
    }

    int
    g729MediaEncoder::encode_packet(const int16_t* packet)
    {
        uint8_t output[10];
        uint8_t length = 0;
        bcg729Encoder(context_, packet, output, &length);

        if(length == 0 || (length != G729_PACKET_SIZE && length != G729_VAD_SIZE))
            return -1;

        memcpy(&buffer_[RTP_HEADER_SIZE + (packet_in_rtp_idx * G729_PACKET_SIZE)] , output, length);
        if (packet_in_rtp_idx == packets_in_rtp - 1 || length == G729_VAD_SIZE) {
            int last_frame_size = length == G729_VAD_SIZE ? G729_VAD_SIZE : G729_PACKET_SIZE;
            send(buffer_, RTP_HEADER_SIZE + ((packet_in_rtp_idx) * G729_PACKET_SIZE) + last_frame_size);
            seq_val_++;
            packet_in_rtp_idx = 0;
        }
        else packet_in_rtp_idx++;

        // update info for new packet
        timestamp_ += 80;

        return 0;
    }

    int
    g729MediaEncoder::initHeader(int8_t* buffer)
    {
        buffer[0]  = 2 << 6;    // ver=2;P=0;X=0;CC=0;
        buffer[1]  = 18 & 0x7f; // M=0;PT=18;
        buffer[2]  =          seq_val_ >> 8;
        buffer[3]  = (int8_t) seq_val_;
        buffer[4]  =          timestamp_ >> 24;
        buffer[5]  = (int8_t)(timestamp_ >> 16);
        buffer[6]  = (int8_t)(timestamp_ >> 8);
        buffer[7]  = (int8_t) timestamp_;
        buffer[8]  =          ssrc_ >> 24;
        buffer[9]  = (int8_t)(ssrc_ >> 16);
        buffer[10] = (int8_t)(ssrc_ >> 8);
        buffer[11] = (int8_t) ssrc_;

        return 0;
    }

    int
    g729MediaEncoder::send(uint8_t* buffer, int size)
    {
        // update rtp header
        buffer[2]  =          seq_val_ >> 8;
        buffer[3]  = (int8_t) seq_val_;
        buffer[4]  =          timestamp_ >> 24;
        buffer[5]  = (int8_t)(timestamp_ >> 16);
        buffer[6]  = (int8_t)(timestamp_ >> 8);
        buffer[7]  = (int8_t) timestamp_;

        if(sp_) {
            int ret;
            do {
                ret = sp_->writeData(buffer, size);
            } while (ret < 0 and errno == EAGAIN);
        }

        return 0;
    }

}