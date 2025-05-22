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

#include "audio_sender.h"
#include "client/videomanager.h"
#include "libav_deps.h"
#include "logger.h"
#include "media_encoder.h"
#include "media_io_handle.h"
#include "media_stream.h"

#include <memory>

namespace sip_core {

AudioSender::AudioSender(const std::string& dest,
                         const MediaDescription& args,
                         SocketPair& socketPair,
                         const uint16_t seqVal,
                         const uint16_t mtu)
    : dest_(dest)
    , args_(args)
    , seqVal_(seqVal)
    , mtu_(mtu)
{
    setup(socketPair);
}

AudioSender::~AudioSender()
{
    audioEncoder_.reset();
    muxContext_.reset();
    micData_.clear();
    resampledData_.clear();
}

bool
AudioSender::setup(SocketPair& socketPair)
{
    audioEncoder_.reset(new MediaEncoder);
    muxContext_.reset(socketPair.createIOContext(mtu_));

    try {
        /* Encoder setup */
        SIP_CORE_DBG("audioEncoder_->openOutput %s", dest_.c_str());
        audioEncoder_->openOutput(dest_, "rtp");
        audioEncoder_->setOptions(args_);
        auto codec = std::static_pointer_cast<AccountAudioCodecInfo>(args_.codec);
        auto ms = MediaStream("audio sender", codec->audioformat);
        audioEncoder_->setOptions(ms);
        audioEncoder_->addStream(args_.codec->systemCodecInfo);
        audioEncoder_->setInitSeqVal(seqVal_);
        audioEncoder_->setIOContext(muxContext_->getContext());
    } catch (const MediaEncoderException& e) {
        SIP_CORE_ERR("%s", e.what());
        return false;
    }
#ifdef DEBUG_SDP
    audioEncoder_->print_sdp();
#endif

    return true;
}

// currently, we support only simple dtmf events
bool
AudioSender::sendRtpEvents(const std::string& events)
{
    std::lock_guard<std::mutex> lock(dtmfQueueMutex_);

    bool found = false;

    // no more than 32 symbols at once
    if (txDtmfQueue_.size() + events.size() >= 32) {
        return false;
    }

    /* convert ASCII digits from events into payload type first, to make sure
     * that all digits are valid.
     */
    for (auto c : events) {
        unsigned int dig = std::tolower(static_cast<unsigned char>(c));
        unsigned pt;

        if (dig >= '0' && dig <= '9') {
            pt = dig - '0';
        } else if (dig >= 'a' && dig <= 'd') {
            pt = dig - 'a' + 12;
        } else if (dig == '*') {
            pt = 10;
        } else if (dig == '#') {
            pt = 11;
        } else if (dig == 'r') {
            pt = 16;
        } else {
            continue;
        }

        found = true;

        txDtmfQueue_.push({pt, 0, 0});
    }

    return found;
}

void
AudioSender::update(Observable<std::shared_ptr<sip_core::MediaFrame>>* /*obs*/,
                    const std::shared_ptr<sip_core::MediaFrame>& framePtr)
{
    auto frame = framePtr->pointer();

    // check for change in voice activity, if so, call callback
    // downcast MediaFrame to AudioFrame
    bool hasVoice = std::dynamic_pointer_cast<AudioFrame>(framePtr)->has_voice;
    if (hasVoice != voice_) {
        voice_ = hasVoice;
        if (voiceCallback_) {
            voiceCallback_(voice_);
        } else {
            SIP_CORE_ERR("AudioSender no voice callback!");
        }
    }

    if (txDtmfQueue_.size() != 0) {
        RtpDtmfPayload dtmfPayload {};

        bool first, last = false;

        createDtmfPayload(&dtmfPayload, &first, &last);

        // packet with 32 flag == dtmf
        int flags = 32;

        if (first) {
            // set marker bit for first packet
            flags |= 128;

            // set new timestamp for first packet
            flags |= 64;
        }
        
        audioEncoder_->sendBuffer(reinterpret_cast<uint8_t*>(&dtmfPayload), 4, sent_samples, flags);
        sent_samples += 160;

    } else {
        frame->pts = sent_samples;
        sent_samples += frame->nb_samples;
        if (audioEncoder_->encodeAudio(*std::static_pointer_cast<AudioFrame>(framePtr)) < 0)
            SIP_CORE_ERR("encoding failed");
    }
}

void
AudioSender::setVoiceCallback(std::function<void(bool)> cb)
{
    if (cb) {
        voiceCallback_ = std::move(cb);
    } else {
        SIP_CORE_ERR("AudioSender trying to set invalid voice callback");
    }
}

uint16_t
AudioSender::getLastSeqValue()
{
    return audioEncoder_->getLastSeqValue();
}

int
AudioSender::setPacketLoss(uint64_t pl)
{
    // The encoder may be destroy during a bitrate change
    // when a codec parameter like auto quality change
    if (!audioEncoder_)
        return -1; // NOK

    return audioEncoder_->setPacketLoss(pl);
}

/* RFC 2833 digit */
static const char digitmap[17]
    = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '*', '#', 'A', 'B', 'C', 'D', 'R'};

void
AudioSender::createDtmfPayload(RtpDtmfPayload* payload, bool* first, bool* last)
{
    dtmf& data = txDtmfQueue_.front();

    constexpr uint16_t SAMPLES_PER_PACKET = 160; // 20 ms @ 8 kHz
    constexpr uint16_t MIN_END_DURATION = 800;   // ≥100 ms before setting E-bit

    *first = *last = false;

    /* First packet for this digit ----------------------------------------- */
    if (data.duration == 0) {
        SIP_CORE_DBG() << "Sending DTMF digit id " << digitmap[data.event];
        *first = true;
    }

    /* --------------------------------------------------------------------- */
    /* Build the RTP-DTMF payload                                            */
    /* --------------------------------------------------------------------- */
    payload->event = static_cast<uint8_t>(data.event);
    payload->volume = 10; // 0-63 (arbitrary example)
    payload->duration = static_cast<uint16_t>(data.duration);

    /* --------------------------------------------------------------------- */
    /* End-of-event handling                                                 */
    /* --------------------------------------------------------------------- */
    if (data.duration >= MIN_END_DURATION) {
        payload->volume |= 0x80; // set E-bit

        /* RFC 2833: transmit the ending packet a few times (here: 3) */
        if (++data.eBitRetransmissions >= 3) {
            *last = true;
            /* Move on to the next queued digit                           */
            std::lock_guard<std::mutex> lock(dtmfQueueMutex_);
            txDtmfQueue_.pop();
        }
    }

    /* --------------------------------------------------------------------- */
    /* Prepare for the next invocation                                       */
    /* --------------------------------------------------------------------- */
    data.duration += SAMPLES_PER_PACKET; // cumulative
}
} // namespace sip_core