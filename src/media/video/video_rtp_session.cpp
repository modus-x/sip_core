/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
 *  Author: Tristan Matthews <tristan.matthews@savoirfairelinux.com>
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

#include "client/videomanager.h"
#include "video_rtp_session.h"
#include "video_sender.h"
#include "video_receive_thread.h"
#include "video_mixer.h"
#include "socket_pair.h"
#include "sip/sipvoiplink.h" // for enqueueKeyframeRequest
#include "manager.h"
#include "logger.h"
#include "string_utils.h"
#include "call.h"
#include "conference.h"
#include "congestion_control.h"

#include "account_const.h"

#include <sstream>
#include <map>
#include <string>
#include <thread>
#include <chrono>

namespace sip_core {
namespace video {

static void
keep_alive_timer_cb(pj_timer_heap_t* th, pj_timer_entry* te)
{
    VideoRtpSession* rtp_session;
    pj_time_val delay;
    pj_status_t status;
    unsigned lower_bound;

    PJ_UNUSED_ARG(th);

    te->id = PJ_FALSE;

    rtp_session = (VideoRtpSession*) te->user_data;


    /* Send some empty rtp packet with correct params */
    rtp_session->natPing();

    /* Check just in case keep-alive has been disabled. This shouldn't happen
     * though as when ka_interval is changed this timer should have been
     * cancelled.
     */
    int interval = rtp_session->getKaInterval();
    if (interval == 0)
        return;

    lower_bound = (unsigned) ((float) interval * 0.8f);
    delay.sec = pj_rand() % (interval - lower_bound) + lower_bound;
    delay.msec = 0;

    /* Reschedule next timer */
    status = pjsip_endpt_schedule_timer(rtp_session->getAccount()->getVoipLink().getEndpoint(),
                                        te,
                                        &delay);
    if (status == PJ_SUCCESS) {
        te->id = PJ_TRUE;
    } else {
        SIP_CORE_ERROR("VideoRtpSession Error starting keep-alive rtp timer from callback: {:d}", status);
    }
}

using std::string;

static constexpr unsigned MAX_REMB_DEC {1};

constexpr auto DELAY_AFTER_RESTART = std::chrono::milliseconds(1000);
constexpr auto EXPIRY_TIME_RTCP = std::chrono::seconds(2);
constexpr auto DELAY_AFTER_REMB_INC = std::chrono::seconds(1);
constexpr auto DELAY_AFTER_REMB_DEC = std::chrono::milliseconds(500);

VideoRtpSession::VideoRtpSession(const string& callId,
                                 const string& streamId,
                                 const DeviceParams& localVideoParams,
                                 std::shared_ptr<SIPAccountBase> account)
    : RtpSession(callId, streamId, MediaType::MEDIA_VIDEO, account)
    , localVideoParams_(localVideoParams)
    , videoBitrateInfo_ {}
    , rtcpCheckerThread_([] { return true; }, [this] { processRtcpChecker(); }, [] {})
{
    setupVideoBitrateInfo(); // reset bitrate
    cc = std::make_unique<CongestionControl>();
    SIP_CORE_DBG("VideoRtpSession [%p] Video RTP session created for call %s", this, callId_.c_str());
}

VideoRtpSession::~VideoRtpSession()
{
    stop();
    SIP_CORE_DBG("VideoRtpSession [%p] Video RTP session destroyed", this);
}

const VideoBitrateInfo&
VideoRtpSession::getVideoBitrateInfo()
{
    return videoBitrateInfo_;
}

/// Setup internal VideoBitrateInfo structure from media descriptors.
///
void
VideoRtpSession::updateMedia(const MediaDescription& send, const MediaDescription& receive)
{
    BaseType::updateMedia(send, receive);
    setupVideoBitrateInfo();
}

void
VideoRtpSession::natPing()
{
    SIP_CORE_DEBUG("VideoRtpSession Sending keep-alive rtp packet to session {:s}", getRemoteRtpUri());
    sender_->natPing();
}

void
VideoRtpSession::setRequestKeyFrameCallback(std::function<void(void)> cb)
{
    cbKeyFrameRequest_ = std::move(cb);
}

void
VideoRtpSession::startSender()
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    SIP_CORE_DBG("VideoRtpSession [%p] Start video RTP sender: input [%s] - muted [%s]",
                 this,
                 conference_ ? "Video Mixer" : input_.c_str(),
                 send_.onHold ? "YES" : "NO");

    if (not socketPair_) {
        // Ignore if the transport is not set yet
        SIP_CORE_WARN("[%p] Transport not set yet", this);
        return;
    }

    if (send_.enabled and not send_.onHold) {
        if (sender_) {
            if (videoLocal_)
                videoLocal_->detach(sender_.get());
            if (videoMixer_)
                videoMixer_->detach(sender_.get());
            SIP_CORE_WARN("VideoRtpSession [%p] Restarting video sender", this);
        }

        if (not conference_) {
            auto input = getVideoInput(input_);
            videoLocal_ = input;
            if (input) {
                auto newParams = input->getParams();
                try {
                    if (newParams.valid()
                        && newParams.wait_for(NEWPARAMS_TIMEOUT) == std::future_status::ready) {
                        localVideoParams_ = newParams.get();
                    } else {
                        SIP_CORE_ERR("VideoRtpSession [%p] No valid new video parameters", this);
                        return;
                    }
                } catch (const std::exception& e) {
                    SIP_CORE_ERR("VideoRtpSession Exception during retrieving video parameters: %s", e.what());
                    return;
                }
            } else {
                SIP_CORE_WARN("VideoRtpSession Can't lock video input");
                return;
            }

#ifdef __ANDROID__
            if (auto input1 = std::static_pointer_cast<VideoInput>(videoLocal_)) {
                input1->setupSink();
                input1->setFrameSize(localVideoParams_.width, localVideoParams_.height);
            }
#endif
        }

        // be sure to not send any packets before saving last RTP seq value
        socketPair_->stopSendOp();

        auto codecVideo = std::static_pointer_cast<sip_core::AccountVideoCodecInfo>(send_.codec);
        auto autoQuality = codecVideo->isAutoQualityEnabled;

        send_.linkableHW = conference_ == nullptr;
        send_.bitrate = videoBitrateInfo_.videoBitrateCurrent;
        // NOTE:
        // Current implementation does not handle resolution change
        // (needed by window sharing feature) with HW codecs, so HW
        // codecs will be disabled for now.
        bool allowHwAccel = (localVideoParams_.format != "x11grab"
                             && localVideoParams_.format != "dxgigrab");

        if (socketPair_)
            initSeqVal_ = socketPair_->lastSeqValOut();

        try {
            sender_.reset();
            socketPair_->stopSendOp(false);
            MediaStream ms
                = !videoMixer_
                      ? MediaStream("video sender",
                                    AV_PIX_FMT_YUV420P,
                                    1 / static_cast<rational<int>>(localVideoParams_.framerate),
                                    localVideoParams_.width,
                                    localVideoParams_.height,
                                    send_.bitrate,
                                    static_cast<rational<int>>(localVideoParams_.framerate))
                      : videoMixer_->getStream("Video Sender");
            sender_.reset(new VideoSender(getRemoteRtpUri(),
                                          ms,
                                          send_,
                                          *socketPair_,
                                          initSeqVal_ + 1,
                                          mtu_,
                                          callId_,
                                          allowHwAccel));

            if (changeOrientationCallback_)
                sender_->setChangeOrientationCallback(changeOrientationCallback_);
            if (socketPair_)
                socketPair_->setPacketLossCallback([this]() { cbKeyFrameRequest_(); });

            // make hole and wait for turning video on / immediate attaching to video conference
            if (!videoMixer_) {
                natPing();
                emitSignal<libsip_core::CallSignal::VideoSenderNatResolved>(callId_);

                setupKaTimer();
            }

        } catch (const MediaEncoderException& e) {
            SIP_CORE_ERR("%s", e.what());
            send_.enabled = false;
        }
        lastMediaRestart_ = clock::now();
        last_REMB_inc_ = clock::now();
        last_REMB_dec_ = clock::now();
        if (autoQuality and not rtcpCheckerThread_.isRunning())
            rtcpCheckerThread_.start();
        else if (not autoQuality and rtcpCheckerThread_.isRunning())
            rtcpCheckerThread_.join();
    }
}

void
VideoRtpSession::setupKaTimer()
{
    /* Setup and start the timer */
    pj_time_val delay;
    pj_status_t status;
    unsigned delay_initial;
    unsigned lower_bound;
    ka_timer_.cb = &keep_alive_timer_cb;
    ka_timer_.user_data = (void*) this;

    delay_initial = ka_inverval_;

    lower_bound = (unsigned) ((float) delay_initial * 0.8f);
    delay.sec = pj_rand() % (delay_initial - lower_bound) + lower_bound;
    delay.msec = 0;
    status = pjsip_endpt_schedule_timer(account_->getVoipLink().getEndpoint(), &ka_timer_, &delay);
    SIP_CORE_DEBUG("Video rtp keep-alive delay_initial is {:d}, lower_bound is {:d}",
                   delay_initial,
                   lower_bound);
    if (status == PJ_SUCCESS) {
        ka_timer_.id = PJ_TRUE;
        SIP_CORE_DEBUG("Keep-alive timer started for video rtp session {:s}, interval: {:d}s",
                       getRemoteRtpUri(),
                       delay.sec);
    } else {
        ka_timer_.id = PJ_FALSE;
    }
}

void
VideoRtpSession::restartSender()
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // ensure that start has been called before restart
    if (not socketPair_)
        return;

    startSender();

    if (conference_)
        setupConferenceVideoPipeline(*conference_, Direction::SEND);
}

void
VideoRtpSession::stopSender()
{
    // Concurrency protection must be done by caller.
    SIP_CORE_DBG("VideoRtpSession [%p] Stop video RTP sender: input [%s] - muted [%s]",
                 this,
                 conference_ ? "Video Mixer" : input_.c_str(),

                 send_.onHold ? "YES" : "NO");

    cancelKeepAliveTimer();

    if (sender_) {
        if (videoLocal_)
            videoLocal_->detach(sender_.get());
        if (videoMixer_)
            videoMixer_->detach(sender_.get());
        sender_.reset();
    }

    if (socketPair_)
        socketPair_->stopSendOp();
}

void
VideoRtpSession::startReceiver()
{
    // Concurrency protection must be done by caller.

    SIP_CORE_DBG("VideoRtpSession [%p] Starting receiver", this);

    if (receive_.enabled and not receive_.onHold) {
        if (receiveThread_)
            SIP_CORE_WARN("[%p] Already has a receiver, restarting", this);
        receiveThread_.reset(
            new VideoReceiveThread(callId_, !conference_, receive_.receiving_sdp, mtu_));

        // XXX keyframe requests can timeout if unanswered
        receiveThread_->addIOContext(*socketPair_);
        receiveThread_->setSuccessfulSetupCb(onSuccessfulSetup_);
        receiveThread_->startLoop();
        receiveThread_->setRequestKeyFrameCallback([this]() { cbKeyFrameRequest_(); });
        receiveThread_->setRotation(rotation_.load());
        if (videoMixer_ and conference_) {
            // Note, this should be managed differently, this is a bit hacky
            auto audioId = streamId_;
            string_replace(audioId, "video", "audio");
            auto activeStream = videoMixer_->verifyActive(audioId);
            videoMixer_->removeAudioOnlySource(callId_, audioId);
            if (activeStream)
                videoMixer_->setActiveStream(streamId_);
        }

    } else {
        SIP_CORE_DBG("VideoRtpSession [%p] Video receiver disabled", this);
        if (receiveThread_ and videoMixer_ and conference_) {
            // Note, this should be managed differently, this is a bit hacky
            auto audioId_ = streamId_;
            string_replace(audioId_, "video", "audio");
            auto activeStream = videoMixer_->verifyActive(streamId_);
            videoMixer_->addAudioOnlySource(callId_, audioId_);
            receiveThread_->detach(videoMixer_.get());
            if (activeStream)
                videoMixer_->setActiveStream(audioId_);
        }
    }
    if (socketPair_)
        socketPair_->setReadBlockingMode(true);
}

void
VideoRtpSession::stopReceiver()
{
    // Concurrency protection must be done by caller.

    SIP_CORE_DBG("VideoRtpSession [%p] Stopping receiver", this);

    if (not receiveThread_)
        return;

    if (videoMixer_) {
        auto activeStream = videoMixer_->verifyActive(streamId_);
        auto audioId = streamId_;
        string_replace(audioId, "video", "audio");
        videoMixer_->addAudioOnlySource(callId_, audioId);
        receiveThread_->detach(videoMixer_.get());
        if (activeStream)
            videoMixer_->setActiveStream(audioId);
    }

    // We need to disable the read operation, otherwise the
    // receiver thread will block since the peer stopped sending
    // RTP packets.
    if (socketPair_)
        socketPair_->setReadBlockingMode(false);

    receiveThread_->stopLoop();
    receiveThread_->stopSink();
}

void
VideoRtpSession::start()
{
    SIP_CORE_WARN("VideoRtpSession [%p] Starting video rtp session", this);
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    try {
        socketPair_.reset(new SocketPair(getRemoteRtpUri().c_str(), receive_.addr.getPort()));

        last_REMB_inc_ = clock::now();
        last_REMB_dec_ = clock::now();

        socketPair_->setRtpDelayCallback(
            [&](int gradient, int deltaT) { delayMonitor(gradient, deltaT); });

        if (send_.crypto and receive_.crypto) {
            socketPair_->createSRTP(receive_.crypto.getCryptoSuite().c_str(),
                                    receive_.crypto.getSrtpKeyInfo().c_str(),
                                    send_.crypto.getCryptoSuite().c_str(),
                                    send_.crypto.getSrtpKeyInfo().c_str());
        }
    } catch (const std::runtime_error& e) {
        SIP_CORE_ERR("VideoRtpSession [%p] Socket creation failed: %s", this, e.what());
        return;
    }

    startSender();

    if (not send_.enabled and not receive_.enabled) {
        SIP_CORE_WARN("VideoRtpSession [%p] Video rtp session stopped, because send is not enabled", this);
        stop();
        return;
    }

    startReceiver();

    if (conference_) {
        if (send_.enabled and not send_.onHold) {
            setupConferenceVideoPipeline(*conference_, Direction::SEND);
        }
        if (receive_.enabled and not receive_.onHold) {
            setupConferenceVideoPipeline(*conference_, Direction::RECV);
        }
    }
}

void
VideoRtpSession::stop()
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    stopSender();
    stopReceiver();

    rtcpCheckerThread_.join();

    // reset default video quality if exist
    if (videoBitrateInfo_.videoQualityCurrent != SystemCodecInfo::DEFAULT_NO_QUALITY)
        videoBitrateInfo_.videoQualityCurrent = SystemCodecInfo::DEFAULT_CODEC_QUALITY;

    videoBitrateInfo_.videoBitrateCurrent = SystemCodecInfo::DEFAULT_VIDEO_BITRATE;
    storeVideoBitrateInfo();

    if (socketPair_)
        socketPair_->interrupt();
    socketPair_.reset();
    videoLocal_.reset();
}

void
VideoRtpSession::setMuted(bool mute, Direction dir)
{
    SIP_CORE_DBG("[%p] VideoRtpSession change to %s", this, mute ? "muted" : "un-muted");
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // ensure that start has been called before setmuted
    if (not socketPair_)
        return;

    // Sender
    if (dir == Direction::SEND) {
        if (send_.onHold == mute) {
            SIP_CORE_DBG("[%p] Local already %s", this, mute ? "muted" : "un-muted");
            return;
        }

        if ((send_.onHold = mute)) {
            stopSender();
        } else {
            restartSender();
        }
        return;
    }

    // Receiver
    if (receive_.onHold == mute) {
        SIP_CORE_DBG("[%p] Remote already %s", this, mute ? "muted" : "un-muted");
        return;
    }

    if ((receive_.onHold = mute)) {
        stopReceiver();
    } else {
        startReceiver();
        if (conference_ and not receive_.onHold) {
            setupConferenceVideoPipeline(*conference_, Direction::RECV);
        }
    }
}

void
VideoRtpSession::forceKeyFrame()
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
#if __ANDROID__
    if (videoLocal_)
        emitSignal<libsip_core::VideoSignal::RequestKeyFrame>(videoLocal_->getName());
#else
    if (sender_)
        sender_->forceKeyFrame();
#endif
}

void
VideoRtpSession::setRotation(int rotation)
{
    rotation_.store(rotation);
    if (receiveThread_)
        receiveThread_->setRotation(rotation);
}

void
VideoRtpSession::attachLocalVideo(bool attach)
{
    if (sender_) {
        if (videoLocal_) {
            SIP_CORE_DBG("VideoRtpSession [%p] Attach local video - %d", this, attach);
            if (attach) {
                cancelKeepAliveTimer();
                videoLocal_->attach(sender_.get());
            } else {
                auto sender = sender_.get();
                videoLocal_->detach(sender);
                // send some frames immediately
                for (size_t i = 0; i < 5; i++) {
                    sender->blackFrame();
                }
                setupKaTimer();
            }
        }
    } else {
        videoLocal_.reset();
    }
}

void
VideoRtpSession::setupConferenceVideoPipeline(Conference& conference, Direction dir)
{
    if (dir == Direction::SEND) {
        SIP_CORE_DBG("VideoRtpSession [%p] Setup video sender pipeline on conference %s for call %s",
                     this,
                     conference.getConfId().c_str(),
                     callId_.c_str());
        videoMixer_ = conference.getVideoMixer();
        if (sender_) {
            // Swap sender from local video to conference video mixer
            if (videoLocal_)
                videoLocal_->detach(sender_.get());
            if (videoMixer_)
                videoMixer_->attach(sender_.get());
        } else {
            SIP_CORE_WARN("[%p] no sender", this);
        }
    } else {
        SIP_CORE_DBG("VideoRtpSession [%p] Setup video receiver pipeline on conference %s for call %s",
                     this,
                     conference.getConfId().c_str(),
                     callId_.c_str());
        if (receiveThread_) {
            receiveThread_->stopSink();
            if (videoMixer_)
                videoMixer_->attachVideo(receiveThread_.get(), callId_, streamId_);
        } else {
            SIP_CORE_WARN("[%p] no receiver", this);
        }
    }
}

void
VideoRtpSession::enterConference(Conference& conference)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    exitConference();

    conference_ = &conference;
    videoMixer_ = conference.getVideoMixer();
    SIP_CORE_DBG("VideoRtpSession [%p] enterConference (conf: %s)", this, conference.getConfId().c_str());

    if (send_.enabled or receiveThread_) {
        // Restart encoder with conference parameter ON in order to unlink HW encoder
        // from HW decoder.
        restartSender();
        if (conference_) {
            setupConferenceVideoPipeline(conference, Direction::RECV);
        }
    }
}

void
VideoRtpSession::exitConference()
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!conference_)
        return;

    SIP_CORE_DBG("VideoRtpSession [%p] exitConference (conf: %s)", this, conference_->getConfId().c_str());

    if (videoMixer_) {
        if (sender_)
            videoMixer_->detach(sender_.get());

        if (receiveThread_) {
            auto activeStream = videoMixer_->verifyActive(streamId_);
            videoMixer_->detachVideo(receiveThread_.get());
            receiveThread_->startSink();
            if (activeStream)
                videoMixer_->setActiveStream(streamId_);
        }

        videoMixer_.reset();
    }

    conference_ = nullptr;
}

bool
VideoRtpSession::check_RCTP_Info_RR(RTCPInfo& rtcpi)
{
    auto rtcpInfoVect = socketPair_->getRtcpRR();
    unsigned totalLost = 0;
    unsigned totalJitter = 0;
    unsigned nbDropNotNull = 0;
    auto vectSize = rtcpInfoVect.size();

    if (vectSize != 0) {
        for (const auto& it : rtcpInfoVect) {
            if (it.fraction_lost != 0) // Exclude null drop
                nbDropNotNull++;
            totalLost += it.fraction_lost;
            totalJitter += ntohl(it.jitter);
        }
        rtcpi.packetLoss = nbDropNotNull ? (float) (100 * totalLost) / (256.0 * nbDropNotNull) : 0;
        // Jitter is expressed in timestamp unit -> convert to milliseconds
        // https://stackoverflow.com/questions/51956520/convert-jitter-from-rtp-timestamp-unit-to-millisseconds
        rtcpi.jitter = (totalJitter / vectSize / 90000.0f) * 1000;
        rtcpi.nb_sample = vectSize;
        rtcpi.latency = socketPair_->getLastLatency();
        return true;
    }
    return false;
}

bool
VideoRtpSession::check_RCTP_Info_REMB(uint64_t* br)
{
    auto rtcpInfoVect = socketPair_->getRtcpREMB();

    if (!rtcpInfoVect.empty()) {
        auto pkt = rtcpInfoVect.back();
        auto temp = cc->parseREMB(pkt);
        *br = (temp >> 10) | ((temp << 6) & 0xff00) | ((temp << 16) & 0x30000);
        return true;
    }
    return false;
}

void
VideoRtpSession::adaptQualityAndBitrate()
{
    setupVideoBitrateInfo();

    uint64_t br;
    if (check_RCTP_Info_REMB(&br)) {
        delayProcessing(br);
    }

    RTCPInfo rtcpi {};
    if (check_RCTP_Info_RR(rtcpi)) {
        dropProcessing(&rtcpi);
    }
}

void
VideoRtpSession::dropProcessing(RTCPInfo* rtcpi)
{
    // If bitrate has changed, let time to receive fresh RTCP packets
    auto now = clock::now();
    auto restartTimer = now - lastMediaRestart_;
    if (restartTimer < DELAY_AFTER_RESTART) {
        return;
    }

    // Do nothing if jitter is more than 1 second
    if (rtcpi->jitter > 1000) {
        return;
    }

    auto pondLoss = getPonderateLoss(rtcpi->packetLoss);
    auto oldBitrate = videoBitrateInfo_.videoBitrateCurrent;
    int newBitrate = oldBitrate;

    // Fill histoLoss and histoJitter_ with samples
    if (restartTimer < DELAY_AFTER_RESTART + std::chrono::seconds(1)) {
        return;
    } else {
        // If ponderate drops are inferior to 10% that mean drop are not from congestion but from
        // network...
        // ... we can increase
        if (pondLoss >= 5.0f && rtcpi->packetLoss > 0.0f) {
            newBitrate *= 1.0f - rtcpi->packetLoss / 150.0f;
            histoLoss_.clear();
            lastMediaRestart_ = now;
            SIP_CORE_DBG(
                "[BandwidthAdapt] Detected transmission bandwidth overuse, decrease bitrate from "
                "%u Kbps to %d Kbps, ratio %f (ponderate loss: %f%%, packet loss rate: %f%%)",
                oldBitrate,
                newBitrate,
                (float) newBitrate / oldBitrate,
                pondLoss,
                rtcpi->packetLoss);
        }
    }

    setNewBitrate(newBitrate);
}

void
VideoRtpSession::delayProcessing(int br)
{
    int newBitrate = videoBitrateInfo_.videoBitrateCurrent;
    if (br == 0x6803)
        newBitrate *= 0.85f;
    else if (br == 0x7378)
        newBitrate *= 1.05f;
    else
        return;

    setNewBitrate(newBitrate);
}

void
VideoRtpSession::setNewBitrate(unsigned int newBR)
{
    newBR = std::max(newBR, videoBitrateInfo_.videoBitrateMin);
    newBR = std::min(newBR, videoBitrateInfo_.videoBitrateMax);

    if (videoBitrateInfo_.videoBitrateCurrent != newBR) {
        videoBitrateInfo_.videoBitrateCurrent = newBR;
        storeVideoBitrateInfo();

#if __ANDROID__
        if (auto input_device = std::dynamic_pointer_cast<VideoInput>(videoLocal_))
            emitSignal<libsip_core::VideoSignal::SetBitrate>(input_device->getConfig().name,
                                                             (int) newBR);
#endif

        if (sender_) {
            auto ret = sender_->setBitrate(newBR);
            if (ret == -1)
                SIP_CORE_ERR("Fail to access the encoder");
            else if (ret == 0)
                restartSender();
        } else {
            SIP_CORE_ERR("Fail to access the sender");
        }
    }
}

void
VideoRtpSession::setupVideoBitrateInfo()
{
    auto codecVideo = std::static_pointer_cast<sip_core::AccountVideoCodecInfo>(send_.codec);
    if (codecVideo) {
        auto& info = codecVideo->systemCodecInfo;
        videoBitrateInfo_ = {
            codecVideo->bitrate,
            info.minBitrate,
            info.maxBitrate,
            codecVideo->quality,
            info.minQuality,
            info.maxQuality,
            videoBitrateInfo_.cptBitrateChecking,
            videoBitrateInfo_.maxBitrateChecking,
            videoBitrateInfo_.packetLostThreshold,
        };
    } else {
        videoBitrateInfo_
            = {0, 0, 0, 0, 0, 0, 0, MAX_ADAPTATIVE_BITRATE_ITERATION, PACKET_LOSS_THRESHOLD};
    }
}

void
VideoRtpSession::storeVideoBitrateInfo()
{
    if (auto codecVideo = std::static_pointer_cast<sip_core::AccountVideoCodecInfo>(send_.codec)) {
        codecVideo->bitrate = videoBitrateInfo_.videoBitrateCurrent;
        codecVideo->quality = videoBitrateInfo_.videoQualityCurrent;
    }
}

void
VideoRtpSession::processRtcpChecker()
{
    adaptQualityAndBitrate();
    socketPair_->waitForRTCP(std::chrono::seconds(rtcp_checking_interval));
}

void
VideoRtpSession::initRecorder(std::shared_ptr<MediaRecorder>& rec)
{
    if (receiveThread_) {
        if (auto ob = rec->addStream(receiveThread_->getInfo())) {
            receiveThread_->attach(ob);
        }
    }
    if (Manager::instance().videoPreferences.getRecordPreview()) {
        if (auto input = std::static_pointer_cast<VideoInput>(videoLocal_)) {
            if (auto ob = rec->addStream(input->getInfo())) {
                input->attach(ob);
            }
        }
    }
}

void
VideoRtpSession::deinitRecorder(std::shared_ptr<MediaRecorder>& rec)
{
    if (!rec)
        return;
    if (receiveThread_) {
        if (auto ob = rec->getStream(receiveThread_->getInfo().name)) {
            receiveThread_->detach(ob);
        }
    }
    if (auto input = std::static_pointer_cast<VideoInput>(videoLocal_)) {
        if (auto ob = rec->getStream(input->getInfo().name)) {
            input->detach(ob);
        }
    }
}

void
VideoRtpSession::setChangeOrientationCallback(std::function<void(int)> cb)
{
    changeOrientationCallback_ = std::move(cb);
    if (sender_)
        sender_->setChangeOrientationCallback(changeOrientationCallback_);
}

float
VideoRtpSession::getPonderateLoss(float lastLoss)
{
    float pond = 0.0f, pondLoss = 0.0f, totalPond = 0.0f;
    constexpr float coefficient_a = -1 / 100.0f;
    constexpr float coefficient_b = 100.0f;

    auto now = clock::now();

    histoLoss_.emplace_back(now, lastLoss);

    for (auto it = histoLoss_.begin(); it != histoLoss_.end();) {
        auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->first);

        // 1ms      -> 100%
        // 2000ms   -> 80%
        if (delay <= EXPIRY_TIME_RTCP) {
            if (it->second == 0.0f)
                pond = 20.0f; // Reduce weight of null drop
            else
                pond = std::min(delay.count() * coefficient_a + coefficient_b, 100.0f);
            totalPond += pond;
            pondLoss += it->second * pond;
            ++it;
        } else
            it = histoLoss_.erase(it);
    }
    if (totalPond == 0)
        return 0.0f;

    return pondLoss / totalPond;
}

void
VideoRtpSession::delayMonitor(int gradient, int deltaT)
{
    float estimation = cc->kalmanFilter(gradient);
    float thresh = cc->get_thresh();

    cc->update_thresh(estimation, deltaT);

    BandwidthUsage bwState = cc->get_bw_state(estimation, thresh);
    auto now = clock::now();

    if (bwState == BandwidthUsage::bwOverusing) {
        auto remb_timer_dec = now - last_REMB_dec_;
        if ((not remb_dec_cnt_) or (remb_timer_dec > DELAY_AFTER_REMB_DEC)) {
            last_REMB_dec_ = now;
            remb_dec_cnt_ = 0;
        }

        // Limit REMB decrease to MAX_REMB_DEC every DELAY_AFTER_REMB_DEC ms
        if (remb_dec_cnt_ < MAX_REMB_DEC && remb_timer_dec < DELAY_AFTER_REMB_DEC) {
            remb_dec_cnt_++;
            SIP_CORE_WARN("VideoRtpSession [BandwidthAdapt] Detected reception bandwidth overuse");
            uint8_t* buf = nullptr;
            uint64_t br = 0x6803; // Decrease 3
            auto v = cc->createREMB(br);
            buf = &v[0];
            socketPair_->writeData(buf, v.size());
            last_REMB_inc_ = clock::now();
        }
    } else if (bwState == BandwidthUsage::bwNormal) {
        auto remb_timer_inc = now - last_REMB_inc_;
        if (remb_timer_inc > DELAY_AFTER_REMB_INC) {
            uint8_t* buf = nullptr;
            uint64_t br = 0x7378; // INcrease
            auto v = cc->createREMB(br);
            buf = &v[0];
            socketPair_->writeData(buf, v.size());
            last_REMB_inc_ = clock::now();
        }
    }
}
} // namespace video
} // namespace sip_core
