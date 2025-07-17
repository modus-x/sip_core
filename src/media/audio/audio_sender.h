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
#pragma once

#include "audiobuffer.h"
#include "media_buffer.h"
#include "media_codec.h"
#include "noncopyable.h"
#include "observer.h"
#include "socket_pair.h"
#include <queue>

namespace sip_core {

// converted from simple char, unsigned int for fastest work
    struct dtmf
    {
        unsigned int event;
        unsigned int duration;
        unsigned int requestedDuration;
        unsigned int volume;
        unsigned int samplesPerPacket;
        unsigned int eBitRetransmissions; /**< # of E bit transmissions   */
        bool firstSent;
    };

    class AudioInput;
    class MediaEncoder;
    class MediaIOHandle;
    class Resampler;

    class AudioSender : public Observer<std::shared_ptr<MediaFrame>>
    {
    public:
        AudioSender(const std::string& dest,
                    const MediaDescription& args,
                    SocketPair& socketPair,
                    const uint16_t seqVal,
                    const uint16_t mtu);
        ~AudioSender();

        uint16_t getLastSeqValue();
        int setPacketLoss(uint64_t pl);

        void setVoiceCallback(std::function<void(bool)> cb);

        void update(Observable<std::shared_ptr<sip_core::MediaFrame>>*,
                    const std::shared_ptr<sip_core::MediaFrame>&) override;

        bool sendRtpEvents(const std::string& events, double duration, unsigned int volume);

    private:
        NON_COPYABLE(AudioSender);

        /**
         * Declaration for DTMF telephony-events (RFC2833, 32 bytes)
         */
        struct RtpDtmfPayload
        {
            uint8_t event;     /**< Event type ID.	    */
            uint8_t volume;    /**< Event volume.	    */
            uint16_t duration; /**< Event duration.    */
        };

        unsigned int createDtmfPayload(RtpDtmfPayload* payload, bool* first, bool* last);

        /* RFC 2833 DTMF transmission FIFO queue */
        std::mutex dtmfQueueMutex_;
        std::queue<dtmf> txDtmfQueue_;

        bool setup(SocketPair& socketPair);

        std::string dest_;
        MediaDescription args_;
        std::unique_ptr<MediaEncoder> audioEncoder_;
        std::unique_ptr<MediaIOHandle> muxContext_;

        uint64_t sent_samples = 0;

        AudioBuffer micData_;
        AudioBuffer resampledData_;
        const uint16_t seqVal_;
        uint16_t mtu_;

        // last voice activity state
        bool voice_ {false};
        std::function<void(bool)> voiceCallback_;

        std::chrono::steady_clock::time_point nextDigitEarliest_{
                std::chrono::steady_clock::now()
        };
        unsigned int interDigitGapMs_ = 300;
    };

} // namespace sip_core
