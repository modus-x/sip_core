/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
 *  Author: Tristan Matthews <tristan.matthews@savoirfairelinux.com>
 *  Author: Adrien Béraud <adrien.beraud@savoirfairelinux.com>
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
#include "media_device.h"
#include "rtp_session.h"
#include "media_stream.h"

#include "threadloop.h"

#include <string>
#include <memory>

namespace sip_core {

class AudioInput;
class AudioReceiveThread;
class AudioSender;
class MediaRecorder;
class RingBuffer;

struct RTCPInfo
{
    float packetLoss;
    unsigned int jitter;
    unsigned int nb_sample;
    float latency;
};

class AudioRtpSession : public RtpSession
{
public:
    AudioRtpSession(const std::string& callId,
                    const std::string& streamId,
                    const std::shared_ptr<MediaRecorder>& rec);
    virtual ~AudioRtpSession();

    void start() override;
    void restartSender() override;
    void stop() override;
    void setMuted(bool muted, Direction dir = Direction::SEND) override;

    void initRecorder() override;
    void deinitRecorder() override;

    void sendRtpEvents(const std::string& events, double duration, unsigned int volume);
    void startEarlyMedia();
    void stopEarlyMedia();
    void promoteEarlyMediaToActive();
    void startHoldKeepalive();
    void stopHoldKeepalive(bool restartSender = false);

    std::shared_ptr<AudioInput>& getAudioLocal() { return audioInput_; }
    std::unique_ptr<AudioReceiveThread>& getAudioReceive() { return receiveThread_; }

    void setVoiceCallback(std::function<void(const std::string&, bool)> cb);

    virtual rtcpRRHeader getRtcpRR() override;
    virtual rtcpREMBHeader getRtcpREMB() override;
    virtual rtcpSRHeader getRtcpSR() override;

private:
    void ensureSocketPairLocked();
    void ensureEarlySenderLocked();
    void startNatPunchingLocked();
    void stopNatPunchingLocked();
    void processNatPunch();
    void startSender();
    void startReceiver();
    bool check_RCTP_Info_RR(RTCPInfo& rtcpi);
    void adaptQualityAndBitrate();
    void dropProcessing(RTCPInfo* rtcpi);
    void setNewPacketLoss(unsigned int newPL);
    float getPonderateLoss(float lastLoss);

    std::unique_ptr<AudioSender> sender_;
    std::unique_ptr<AudioReceiveThread> receiveThread_;
    std::shared_ptr<AudioInput> audioInput_;
    std::shared_ptr<RingBuffer> ringbuffer_;
    uint16_t initSeqVal_ {0};
    bool muteState_ {false};
    bool receiverActive_ {true};
    bool earlyMediaMode_ {false};
    bool holdKeepaliveMode_ {false};
    unsigned packetLoss_ {10};
    DeviceParams localAudioParams_;

    InterruptedThreadLoop rtcpCheckerThread_;
    InterruptedThreadLoop natPunchThread_;
    void processRtcpChecker();

    // Interval in seconds between RTCP checking
    std::chrono::seconds rtcp_checking_interval {4};
    std::chrono::milliseconds natPunchInterval_ {1000};

    std::function<void(const std::string& id, bool)> voiceCallback_;

    void attachRemoteRecorder(const MediaStream& ms);
    void attachLocalRecorder(const MediaStream& ms);
};

} // namespace sip_core
