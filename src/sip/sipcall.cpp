/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
 *  Author: Emmanuel Milou <emmanuel.milou@savoirfairelinux.com>
 *  Author: Alexandre Bourget <alexandre.bourget@savoirfairelinux.com>
 *  Author: Yan Morin <yan.morin@savoirfairelinux.com>
 *  Author: Laurielle Lea <laurielle.lea@savoirfairelinux.com>
 *  Author: Guillaume Roguez <guillaume.roguez@savoirfairelinux.com>
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

#include "call_factory.h"
#include "sip/sipcall.h"
#include "sip/sipaccount.h"
#include "sip/sipaccountbase.h"
#include "sip/sipvoiplink.h"
#include "logger.h"
#include "sdp.h"
#include "manager.h"
#include "string_utils.h"
#include "connectivity/sip_utils.h"
#include "audio/audio_rtp_session.h"
#include "system_codec_container.h"
#include "im/instant_messaging.h"
#include "sip_core/account_const.h"
#include "sip_core/call_const.h"
#include "sip_core/media_const.h"
#include "client/ring_signal.h"
#include "pjsip-ua/sip_inv.h"

#ifdef ENABLE_VIDEO

#include "client/videomanager.h"
#include "media/video/video_input.h"
#include "media/video/video_source_utils.h"
#include "video/video_rtp_session.h"
#include "sip_core/videomanager_interface.h"
#include <chrono>
#include <libavutil/display.h>
#include <video/sinkclient.h>
#include "media/video/video_mixer.h"

#endif

#include "audio/ringbufferpool.h"

#include "errno.h"

#include <atomic>
#include <fmt/ranges.h>

#include "tracepoint.h"

namespace sip_core {

using sip_utils::CONST_PJ_STR;
using namespace libsip_core::Call;

#ifdef ENABLE_VIDEO

static DeviceParams
getVideoSettings()
{
    const auto& videomon = sip_core::getVideoDeviceMonitor();
    return videomon.getDeviceParams(videomon.getDefaultDevice());
}

static bool
isValidVideoSwitchSource(const std::string& source)
{
    return video::isValidVideoSwitchSource(source, sip_core::getVideoDeviceMonitor().getDeviceList());
}

#endif

static const char*
mediaDirectionToString(MediaDirection direction)
{
    switch (direction) {
    case MediaDirection::SENDRECV:
        return "sendrecv";
    case MediaDirection::SENDONLY:
        return "sendonly";
    case MediaDirection::RECVONLY:
        return "recvonly";
    case MediaDirection::INACTIVE:
        return "inactive";
    case MediaDirection::UNKNOWN:
        break;
    }

    return "unknown";
}

static constexpr std::chrono::milliseconds MS_BETWEEN_2_KEYFRAME_REQUEST {1000};
static constexpr auto MULTISTREAM_REQUIRED_VERSION_STR = "10.0.2"sv;
static const std::vector<unsigned> MULTISTREAM_REQUIRED_VERSION
    = split_string_to_unsigned(MULTISTREAM_REQUIRED_VERSION_STR, '.');
static constexpr auto NEW_CONFPROTOCOL_VERSION_STR = "13.1.0"sv;
static const std::vector<unsigned> NEW_CONFPROTOCOL_VERSION
    = split_string_to_unsigned(NEW_CONFPROTOCOL_VERSION_STR, '.');

SIPCall::SIPCall(const std::shared_ptr<SIPAccountBase>& account,
                 const std::string& callId,
                 Call::CallType type,
                 const std::vector<libsip_core::MediaMap>& mediaList)
    : Call(account, callId, type)
    , sdp_(new Sdp(callId))
    , srtpEnabled_(account->isSrtpEnabled())
    , canRetryWithBackupRoute_(type == Call::CallType::OUTGOING)
{
    sip_core_tracepoint(call_start, callId.c_str());

    // Set the media caps.
    sdp_->setLocalMediaCapabilities(MediaType::MEDIA_AUDIO,
                                    account->getActiveAccountCodecInfoList(MEDIA_AUDIO));
#ifdef ENABLE_VIDEO
    sdp_->setLocalMediaCapabilities(MediaType::MEDIA_VIDEO,
                                    account->getActiveAccountCodecInfoList(MEDIA_VIDEO));
#endif

    auto mediaAttrList = MediaAttribute::buildMediaAttributesList(mediaList, isSrtpEnabled());

    if (mediaAttrList.size() == 0) {
        if (type_ == Call::CallType::INCOMING) {
            // Handle incoming call without media offer.
            SIP_CORE_WARN(
                "[call:%s] No media offered in the incoming invite. An offer will be provided in "
                "the answer",
                getCallId().c_str());
            mediaAttrList = getSIPAccount()->createDefaultMediaList(false,
                                                                    getState() == CallState::HOLD);
        } else {
            SIP_CORE_WARN("[call:%s] Creating an outgoing call with empty offer",
                          getCallId().c_str());
        }
    }

    SIP_CORE_DEBUG("[call:{:s}] Create a new [{:s}] SIP call with {:d} media",
                   getCallId(),
                   type == Call::CallType::INCOMING
                       ? "INCOMING"
                       : (type == Call::CallType::OUTGOING ? "OUTGOING" : "MISSED"),
                   mediaList.size());

    initMediaStreams(mediaAttrList);
}

SIPCall::~SIPCall()
{
    std::lock_guard<std::recursive_mutex> lk {callMutex_};

    setSipTransport({});
    setInviteSession(); // prevents callback usage
}

int
SIPCall::findRtpStreamIndex(const std::string& label) const
{
    const auto iter = std::find_if(rtpStreams_.begin(),
                                   rtpStreams_.end(),
                                   [&label](const RtpStream& rtp) {
                                       return label == rtp.mediaAttribute_->label_;
                                   });

    // Return the index if there is a match.
    if (iter != rtpStreams_.end())
        return std::distance(rtpStreams_.begin(), iter);

    // No match found.
    return -1;
}

void
SIPCall::createRtpSession(RtpStream& stream)
{
    if (not stream.mediaAttribute_)
        throw std::runtime_error("Missing media attribute");

    // To get audio_0 ; video_0
    auto streamId = sip_utils::streamId(id_, stream.mediaAttribute_->label_);
    if (stream.mediaAttribute_->type_ == MediaType::MEDIA_AUDIO) {
        stream.rtpSession_ = std::make_shared<AudioRtpSession>(id_, streamId, recorder_);
    }
#ifdef ENABLE_VIDEO
    else if (stream.mediaAttribute_->type_ == MediaType::MEDIA_VIDEO) {
        stream.rtpSession_ = std::make_shared<video::VideoRtpSession>(id_,
                                                                      streamId,
                                                                      getVideoSettings(),
                                                                      getSIPAccount(),
                                                                      recorder_);
        std::static_pointer_cast<video::VideoRtpSession>(stream.rtpSession_)->setRotation(rotation_);
    }
#endif
    else {
        throw std::runtime_error("Unsupported media type");
    }

    // Must be valid at this point.
    if (not stream.rtpSession_)
        throw std::runtime_error("Failed to create RTP Session");
    ;
}

void
SIPCall::configureRtpSession(const std::shared_ptr<RtpSession>& rtpSession,
                             const std::shared_ptr<MediaAttribute>& mediaAttr,
                             const MediaDescription& localMedia,
                             const MediaDescription& remoteMedia)
{
    SIP_CORE_DBG("[call:%s] Configuring [%s] rtp session",
                 getCallId().c_str(),
                 MediaAttribute::mediaTypeToString(mediaAttr->type_));

    if (not rtpSession)
        throw std::runtime_error("Must have a valid RTP Session");

    // Configure the media stream

    auto new_mtu = 1200;
    // TODO: get MTU from somewhere else
    rtpSession->setMtu(new_mtu);
    rtpSession->updateMedia(remoteMedia, localMedia);

    if (localMedia.type == MediaType::MEDIA_AUDIO) {
        if (pendingAudioSocketPair_) {
            localAudioPort_ = pendingAudioSocketPair_->rtpPort();
            rtpSession->setReservedSocketPair(std::move(*pendingAudioSocketPair_));
            pendingAudioSocketPair_.reset();
        } else if (!localMedia.enabled) {
            localAudioPort_ = 0;
        }
    }
#ifdef ENABLE_VIDEO
    else if (localMedia.type == MediaType::MEDIA_VIDEO) {
        if (pendingVideoSocketPair_) {
            localVideoPort_ = pendingVideoSocketPair_->rtpPort();
            rtpSession->setReservedSocketPair(std::move(*pendingVideoSocketPair_));
            pendingVideoSocketPair_.reset();
        } else if (!localMedia.enabled) {
            localVideoPort_ = 0;
        }
    }
#endif

    // Mute/un-mute media
    if (mediaAttr->muted_) {
        rtpSession->setMuted(true);
    } else {
        rtpSession->setMuted(false);
    }

    // always set media source event for muted - we need to start NAT ping streams too.
    rtpSession->setMediaSource(mediaAttr->sourceUri_);

    rtpSession->setSuccessfulSetupCb([w = weak()](MediaType type, bool isRemote) {
        if (auto thisPtr = w.lock())
            thisPtr->rtpSetupSuccess();
    });

    if (localMedia.type == MediaType::MEDIA_AUDIO) {
        setupVoiceCallback(rtpSession);
    }

#ifdef ENABLE_VIDEO
    if (localMedia.type == MediaType::MEDIA_VIDEO) {
        auto videoRtp = std::dynamic_pointer_cast<video::VideoRtpSession>(rtpSession);
        assert(videoRtp && mediaAttr);
        auto streamIdx = findRtpStreamIndex(mediaAttr->label_);
        videoRtp->setRequestKeyFrameCallback([w = weak(), streamIdx] {
            runOnMainThread([w = std::move(w), streamIdx] {
                if (auto thisPtr = w.lock())
                    thisPtr->requestKeyframe(streamIdx);
            });
        });
        videoRtp->setChangeOrientationCallback([w = weak(), streamIdx](int angle) {
            runOnMainThread([w, angle, streamIdx] {
                if (auto thisPtr = w.lock())
                    thisPtr->setVideoOrientation(streamIdx, angle);
            });
        });
        videoRtp->setLocalDeviceParamsChangedCallback([w = weak()](DeviceParams& newParams) {
            if (auto thisPtr = w.lock())
                thisPtr->sendObjectJson("videoDeviceParams", newParams.toJson());
        });
    }
#endif
}

void
SIPCall::setupVoiceCallback(const std::shared_ptr<RtpSession>& rtpSession)
{
    // need to downcast to access setVoiceCallback
    auto audioRtp = std::dynamic_pointer_cast<AudioRtpSession>(rtpSession);

    audioRtp->setVoiceCallback([w = weak()](const std::string& streamId, bool voice) {
        // this is called whenever voice is detected on the local audio

        runOnMainThread([w, streamId, voice] {
            if (auto thisPtr = w.lock()) {
                std::string defaultId = "";
#ifdef ENABLE_VIDEO
                if (not sip_core::getVideoDeviceMonitor().getDeviceList().empty()) {
                    // if we have a video device
                    defaultId = sip_utils::streamId("", sip_utils::DEFAULT_VIDEO_STREAMID);
                }
#endif
                if (defaultId != streamId) {
                    // remote participant audio
                    thisPtr->peerVoice(voice);
                } else {
                    // local mic voice activity
                    thisPtr->localVoice(voice);
                }
            } else {
                SIP_CORE_ERR("voice activity callback unable to lock weak ptr to SIPCall");
            }
        });
    });
}

std::shared_ptr<SIPAccountBase>
SIPCall::getSIPAccount() const
{
    return std::static_pointer_cast<SIPAccountBase>(getAccount().lock());
}

bool
SIPCall::hasEnabledMedia(const std::vector<MediaAttribute>& mediaAttrList, MediaType type)
{
    return std::any_of(mediaAttrList.begin(), mediaAttrList.end(), [type](const auto& mediaAttr) {
        return mediaAttr.type_ == type && mediaAttr.enabled_;
    });
}

uint16_t
SIPCall::getPublishedMediaFamily() const
{
    if (!sdp_) {
        return AF_UNSPEC;
    }

    auto publishedAddr = sdp_->getPublishedIPAddr();
    if (!publishedAddr) {
        return AF_UNSPEC;
    }

    return publishedAddr.getFamily();
}

void
SIPCall::applyPendingLocalPortsToSdp()
{
    if (!sdp_) {
        return;
    }

    const auto audioRtpPort = pendingAudioSocketPair_ ? pendingAudioSocketPair_->rtpPort()
                                                      : static_cast<uint16_t>(localAudioPort_);
    const auto audioRtcpPort = pendingAudioSocketPair_
                                   ? pendingAudioSocketPair_->rtcpPort()
                                   : static_cast<uint16_t>(localAudioPort_ ? localAudioPort_ + 1
                                                                           : 0);
    sdp_->setLocalPublishedAudioPorts(audioRtpPort,
                                      rtcpMuxEnabled_ || audioRtpPort == 0 ? 0 : audioRtcpPort);

#ifdef ENABLE_VIDEO
    const auto videoRtpPort = pendingVideoSocketPair_ ? pendingVideoSocketPair_->rtpPort()
                                                      : static_cast<uint16_t>(localVideoPort_);
    const auto videoRtcpPort = pendingVideoSocketPair_
                                   ? pendingVideoSocketPair_->rtcpPort()
                                   : static_cast<uint16_t>(localVideoPort_ ? localVideoPort_ + 1
                                                                           : 0);
    sdp_->setLocalPublishedVideoPorts(videoRtpPort,
                                      rtcpMuxEnabled_ || videoRtpPort == 0 ? 0 : videoRtcpPort);
#endif
}

bool
SIPCall::prepareLocalMediaReservations(const std::vector<MediaAttribute>& mediaAttrList)
{
    std::lock_guard<std::recursive_mutex> lk {callMutex_};

    auto account = getSIPAccount();
    if (!account) {
        SIP_CORE_ERR("[call:%s] No account detected", getCallId().c_str());
        return false;
    }

    const auto family = getPublishedMediaFamily();
    if (family != AF_INET && family != AF_INET6) {
        SIP_CORE_ERR("[call:%s] Local published address is unavailable, cannot reserve RTP ports",
                     getCallId().c_str());
        return false;
    }

    std::optional<ReservedSocketPair> pendingAudio;
#ifdef ENABLE_VIDEO
    std::optional<ReservedSocketPair> pendingVideo;
#endif

    try {
        if (hasEnabledMedia(mediaAttrList, MediaType::MEDIA_AUDIO)) {
            pendingAudio.emplace(account->reserveAudioSocketPair(family));
        }
#ifdef ENABLE_VIDEO
        if (hasEnabledMedia(mediaAttrList, MediaType::MEDIA_VIDEO)) {
            pendingVideo.emplace(account->reserveVideoSocketPair(family));
        }
#endif
    } catch (const std::exception& e) {
        SIP_CORE_ERR("[call:%s] Failed to reserve local media ports: %s",
                     getCallId().c_str(),
                     e.what());
        return false;
    }

    pendingAudioSocketPair_ = std::move(pendingAudio);
#ifdef ENABLE_VIDEO
    pendingVideoSocketPair_ = std::move(pendingVideo);
#endif
    applyPendingLocalPortsToSdp();
    return true;
}

void
SIPCall::clearPendingLocalReservations()
{
    std::lock_guard<std::recursive_mutex> lk {callMutex_};
    pendingAudioSocketPair_.reset();
#ifdef ENABLE_VIDEO
    pendingVideoSocketPair_.reset();
#endif
    applyPendingLocalPortsToSdp();
}

const std::string&
SIPCall::getContactHeader() const
{
    return contactHeader_;
}

void
SIPCall::setSipTransport(const std::shared_ptr<SipTransport>& transport,
                         const std::string& contactHdr)
{
    const auto list_id = reinterpret_cast<uintptr_t>(this);
    if (sipTransport_)
        sipTransport_->removeStateListener(list_id);

    if (transport != sipTransport_) {
        SIP_CORE_DBG("[call:%s] Setting transport to [%p]", getCallId().c_str(), transport.get());
    }

    sipTransport_ = transport;
    contactHeader_ = contactHdr;

    if (not transport) {
        // Done.
        return;
    }

    if (contactHeader_.empty()) {
        SIP_CORE_WARN("[call:%s] Contact header is empty", getCallId().c_str());
    }

    if (isSrtpEnabled() and not sipTransport_->isSecure()) {
        SIP_CORE_WARN(
            "[call:%s] Crypto (SRTP) is negotiated over an un-encrypted signaling channel",
            getCallId().c_str());
    }

    if (not isSrtpEnabled() and sipTransport_->isSecure()) {
        SIP_CORE_WARN("[call:%s] The signaling channel is encrypted but the media is not encrypted",
                      getCallId().c_str());
    }

    // listen for transport destruction
    sipTransport_->addStateListener(
        list_id, [wthis_ = weak()](pjsip_transport_state state, const pjsip_transport_state_info*) {
            if (auto this_ = wthis_.lock()) {
                SIP_CORE_DBG("[call:%s] SIP transport state [%i] - connection state [%u]",
                             this_->getCallId().c_str(),
                             state,
                             static_cast<unsigned>(this_->getConnectionState()));

                // End the call if the SIP transport was shut down
                auto isAlive = SipTransport::isAlive(state);
                if (not isAlive and this_->getConnectionState() != ConnectionState::DISCONNECTED) {
                    SIP_CORE_WARN(
                        "[call:%s] Ending call because underlying SIP transport was closed",
                        this_->getCallId().c_str());
                    this_->stopAllMedia();
                    this_->detachAudioFromConference();
                    this_->onFailure(ECONNRESET);
                }
            }
        });
}

void
SIPCall::requestReinvite(const std::vector<MediaAttribute>& mediaAttrList)
{
    SIP_CORE_DBG("[call:%s] Sending a SIP re-invite to request media change", getCallId().c_str());

    SIPSessionReinvite(mediaAttrList);
}

/**
 * Send a reINVITE inside an active dialog to modify its state
 * Local SDP session should be modified before calling this method
 */
int
SIPCall::SIPSessionReinvite(const std::vector<MediaAttribute>& mediaAttrList)
{
    assert(not mediaAttrList.empty());

    std::lock_guard<std::recursive_mutex> lk {callMutex_};

    // Do nothing if no invitation processed yet
    if (not inviteSession_ or inviteSession_->invite_tsx)
        return PJ_SUCCESS;

    SIP_CORE_DBG("[call:%s] Preparing and sending a re-invite (state=%s)",
                 getCallId().c_str(),
                 pjsip_inv_state_name(inviteSession_->state));

    // Reserve the next local transport before advertising it in SDP.
    if (!prepareLocalMediaReservations(mediaAttrList)) {
        return !PJ_SUCCESS;
    }

    sdp_->setActiveRemoteSdpSession(nullptr);
    sdp_->setActiveLocalSdpSession(nullptr);

    auto acc = getSIPAccount();
    if (not acc) {
        SIP_CORE_ERR("No account detected");
        return !PJ_SUCCESS;
    }

    if (not sdp_->createOffer(mediaAttrList)) {
        clearPendingLocalReservations();
        return !PJ_SUCCESS;
    }

    pjsip_tx_data* tdata;
    auto local_sdp = sdp_->getLocalSdpSession();
    auto result = pjsip_inv_reinvite(inviteSession_.get(), nullptr, local_sdp, &tdata);
    if (result == PJ_SUCCESS) {
        if (!tdata)
            return PJ_SUCCESS;

        // Add user-agent header
        sip_utils::addUserAgentHeader(acc->getUserAgentName(), tdata);

        // Add out contact header
        sip_utils::addContactHeader(contactHeader_, tdata);

        result = pjsip_inv_send_msg(inviteSession_.get(), tdata);
        if (result == PJ_SUCCESS)
            return PJ_SUCCESS;
        SIP_CORE_ERR("[call:%s] Failed to send REINVITE msg (pjsip: %s)",
                     getCallId().c_str(),
                     sip_utils::sip_strerror(result).c_str());
        // Canceling internals without sending (anyways the send has just failed!)
        pjsip_inv_cancel_reinvite(inviteSession_.get(), &tdata);
        clearPendingLocalReservations();
    } else {
        SIP_CORE_ERR("[call:%s] Failed to create REINVITE msg (pjsip: %s)",
                     getCallId().c_str(),
                     sip_utils::sip_strerror(result).c_str());
        clearPendingLocalReservations();
    }

    return !PJ_SUCCESS;
}

int
SIPCall::SIPSessionReinvite()
{
    auto mediaList = getMediaAttributeList();
    return SIPSessionReinvite(mediaList);
}

void
SIPCall::sendSIPInfo(std::string_view body, std::string_view subtype)
{
    if (subtype != "media_control+xml" && subtype != "dtmf-relay") {
        return;
    }
    std::lock_guard<std::recursive_mutex> lk {callMutex_};
    if (not inviteSession_ or not inviteSession_->dlg)
        throw VoipLinkException("Couldn't get invite dialog");

    constexpr pj_str_t methodName = CONST_PJ_STR("INFO");
    constexpr pj_str_t type = CONST_PJ_STR("application");

    pjsip_method method;
    pjsip_method_init_np(&method, (pj_str_t*) &methodName);

    /* Create request message. */
    pjsip_tx_data* tdata;
    if (pjsip_dlg_create_request(inviteSession_->dlg, &method, -1, &tdata) != PJ_SUCCESS) {
        SIP_CORE_ERR("[call:%s] Could not create dialog", getCallId().c_str());
        return;
    }

    /* Create "application/<subtype>" message body. */
    pj_str_t content = CONST_PJ_STR(body);
    pj_str_t pj_subtype = CONST_PJ_STR(subtype);
    tdata->msg->body = pjsip_msg_body_create(tdata->pool, &type, &pj_subtype, &content);
    if (tdata->msg->body == NULL)
        pjsip_tx_data_dec_ref(tdata);
    else
        pjsip_dlg_send_request(inviteSession_->dlg,
                               tdata,
                               Manager::instance().sipVoIPLink().getModId(),
                               NULL);
}

void
SIPCall::updateRecState(bool state)
{
    std::string BODY = "<?xml version=\"1.0\" encoding=\"utf-8\" ?>"
                       "<media_control><vc_primitive><to_encoder>"
                       "<recording_state="
                       + std::to_string(state)
                       + "/>"
                         "</to_encoder></vc_primitive></media_control>";
    // see https://tools.ietf.org/html/rfc5168 for XML Schema for Media Control details

    SIP_CORE_DBG("Sending recording state via SIP INFO");

    try {
        // sendSIPInfo(BODY, "media_control+xml");
    } catch (const std::exception& e) {
        SIP_CORE_ERR("Error sending recording state: %s", e.what());
    }
}

void
SIPCall::requestKeyframe(int streamIdx)
{
    auto now = clock::now();
    if ((now - lastKeyFrameReq_) < MS_BETWEEN_2_KEYFRAME_REQUEST
        and lastKeyFrameReq_ != time_point::min())
        return;

    std::string streamIdPart;
    if (streamIdx != -1)
        streamIdPart = fmt::format("<stream_id>{}</stream_id>", streamIdx);
    std::string BODY = "<?xml version=\"1.0\" encoding=\"utf-8\" ?>"
                       "<media_control><vc_primitive> "
                       + streamIdPart + "<to_encoder>"
                       + "<picture_fast_update/>"
                         "</to_encoder></vc_primitive></media_control>";
    SIP_CORE_DBG("Sending video keyframe request via SIP INFO");
    try {
        // sendSIPInfo(BODY, "media_control+xml");
    } catch (const std::exception& e) {
        SIP_CORE_ERR("Error sending video keyframe request: %s", e.what());
    }
    lastKeyFrameReq_ = now;
}

void
SIPCall::sendMuteState(bool state)
{
    std::string BODY = "<?xml version=\"1.0\" encoding=\"utf-8\" ?>"
                       "<media_control><vc_primitive><to_encoder>"
                       "<mute_state="
                       + std::to_string(state)
                       + "/>"
                         "</to_encoder></vc_primitive></media_control>";
    // see https://tools.ietf.org/html/rfc5168 for XML Schema for Media Control details

    SIP_CORE_DBG("Sending mute state via SIP INFO");

    try {
        // sendSIPInfo(BODY, "media_control+xml");
    } catch (const std::exception& e) {
        SIP_CORE_ERR("Error sending mute state: %s", e.what());
    }
}

// void
// SIPCall::sendVoiceActivity(std::string_view streamId, bool state)
// {
//     // dont send streamId if it's -1
//     std::string streamIdPart = "";
//     if (streamId != "-1" && !streamId.empty()) {
//         streamIdPart = fmt::format("<stream_id>{}</stream_id>", streamId);
//     }

//     std::string BODY = "<?xml version=\"1.0\" encoding=\"utf-8\" ?>"
//                        "<media_control><vc_primitive>"
//                        + streamIdPart
//                        + "<to_encoder>"
//                          "<voice_activity="
//                        + std::to_string(state)
//                        + "/>"
//                          "</to_encoder></vc_primitive></media_control>";

//     try {
//         sendSIPInfo(BODY, "media_control+xml");
//     } catch (const std::exception& e) {
//         SIP_CORE_ERR("Error sending voice activity state: %s", e.what());
//     }
// }

void
SIPCall::setInviteSession(pjsip_inv_session* inviteSession)
{
    std::lock_guard<std::recursive_mutex> lk {callMutex_};

    if (inviteSession == nullptr and inviteSession_) {
        SIP_CORE_DBG("[call:%s] Delete current invite session", getCallId().c_str());
    } else if (inviteSession != nullptr) {
        // NOTE: The first reference of the invite session is owned by pjsip. If
        // that counter goes down to zero the invite will be destroyed, and the
        // unique_ptr will point freed datas.  To avoid this, we increment the
        // ref counter and let our unique_ptr share the ownership of the session
        // with pjsip.
        if (PJ_SUCCESS != pjsip_inv_add_ref(inviteSession)) {
            SIP_CORE_WARN("[call:%s] trying to set invalid invite session [%p]",
                          getCallId().c_str(),
                          inviteSession);
            inviteSession_.reset(nullptr);
            return;
        }
        SIP_CORE_DBG("[call:%s] Set new invite session [%p]", getCallId().c_str(), inviteSession);
    } else {
        // Nothing to do.
        return;
    }

    inviteSession_.reset(inviteSession);
}

void
SIPCall::terminateSipSession(int status)
{
    SIP_CORE_DBG("[call:%s] Terminate SIP session", getCallId().c_str());
    std::lock_guard<std::recursive_mutex> lk {callMutex_};
    if (inviteSession_ and inviteSession_->state != PJSIP_INV_STATE_DISCONNECTED) {
        pjsip_tx_data* tdata = nullptr;
        auto ret = pjsip_inv_end_session(inviteSession_.get(), status, nullptr, &tdata);
        if (ret == PJ_SUCCESS) {
            if (tdata) {
                auto account = getSIPAccount();
                if (account) {
                    sip_utils::addContactHeader(contactHeader_, tdata);
                    // Add user-agent header
                    sip_utils::addUserAgentHeader(account->getUserAgentName(), tdata);
                } else {
                    SIP_CORE_ERR("No account detected");
                    std::ostringstream msg;
                    msg << "[call:" << getCallId().c_str() << "] "
                        << "The account owning this call is invalid";
                    throw std::runtime_error(msg.str());
                }

                ret = pjsip_inv_send_msg(inviteSession_.get(), tdata);
                if (ret != PJ_SUCCESS)
                    SIP_CORE_ERR("[call:%s] failed to send terminate msg, SIP error (%s)",
                                 getCallId().c_str(),
                                 sip_utils::sip_strerror(ret).c_str());
            }
        } else
            SIP_CORE_ERR("[call:%s] failed to terminate INVITE@%p, SIP error (%s)",
                         getCallId().c_str(),
                         inviteSession_.get(),
                         sip_utils::sip_strerror(ret).c_str());
    }
    setInviteSession();
}

void
SIPCall::setExtraSipHeaders(std::map<std::string, std::string> extraHeaders)
{
    extraHeaders_.merge(extraHeaders);
}

void
SIPCall::answer()
{
    std::lock_guard<std::recursive_mutex> lk {callMutex_};
    auto account = getSIPAccount();
    if (!account) {
        SIP_CORE_ERR("No account detected");
        return;
    }

    if (not inviteSession_)
        throw VoipLinkException("[call:" + getCallId()
                                + "] answer: no invite session for this call");

    if (!inviteSession_->neg) {
        SIP_CORE_WARN("[call:%s] Negotiator is NULL, we've received an INVITE without an SDP",
                      getCallId().c_str());

        Manager::instance().sipVoIPLink().createSDPOffer(inviteSession_.get());
    }

    pjsip_tx_data* tdata;
    if (!inviteSession_->last_answer)
        throw std::runtime_error("Should only be called for initial answer");

    // answer with SDP if no SDP was given in initial invite (i.e. inv->neg is NULL)
    if (pjsip_inv_answer(inviteSession_.get(),
                         PJSIP_SC_OK,
                         NULL,
                         !inviteSession_->neg ? sdp_->getLocalSdpSession() : NULL,
                         &tdata)
        != PJ_SUCCESS)
        throw std::runtime_error("Could not init invite request answer (200 OK)");

    if (contactHeader_.empty()) {
        throw std::runtime_error("Cant answer with an invalid contact header");
    }

    SIP_CORE_DBG("[call:%s] Answering with contact header: %s",
                 getCallId().c_str(),
                 contactHeader_.c_str());

    sip_utils::addContactHeader(contactHeader_, tdata);

    // Add user-agent header
    sip_utils::addUserAgentHeader(account->getUserAgentName(), tdata);

    if (pjsip_inv_send_msg(inviteSession_.get(), tdata) != PJ_SUCCESS) {
        setInviteSession();
        throw std::runtime_error("Could not send invite request answer (200 OK)");
    }

    setState(CallState::ACTIVE, ConnectionState::CONNECTED);
}

void
SIPCall::answer(const std::vector<libsip_core::MediaMap>& mediaList)
{
    std::lock_guard<std::recursive_mutex> lk {callMutex_};
    auto account = getSIPAccount();
    if (not account) {
        SIP_CORE_ERR("No account detected");
        return;
    }

    if (not inviteSession_) {
        SIP_CORE_ERR("[call:%s] No invite session for this call", getCallId().c_str());
        return;
    }

    if (not sdp_) {
        SIP_CORE_ERR("[call:%s] No SDP session for this call", getCallId().c_str());
        return;
    }

    auto newMediaAttrList = MediaAttribute::buildMediaAttributesList(mediaList, isSrtpEnabled());

    if (newMediaAttrList.empty() and rtpStreams_.empty()) {
        SIP_CORE_ERR("[call:%s] Media list must not be empty!", getCallId().c_str());
        return;
    }

    // If the media list is empty, use the current media (this could happen
    // with auto-answer for instance), otherwise update the current media.
    if (newMediaAttrList.empty()) {
        SIP_CORE_DBG("[call:%s] Media list is empty, using current media", getCallId().c_str());
    } else if (newMediaAttrList.size() != rtpStreams_.size()) {
        // Media count is not expected to change
        SIP_CORE_ERROR("[call:{:s}] Media list size {:d} in answer does not match. Expected {:d}",
                       getCallId(),
                       newMediaAttrList.size(),
                       rtpStreams_.size());
        return;
    }

    auto const& mediaAttrList = newMediaAttrList.empty() ? getMediaAttributeList()
                                                         : newMediaAttrList;

    SIP_CORE_DBG("[call:%s] Answering incoming call with following media:", getCallId().c_str());
    for (size_t idx = 0; idx < mediaAttrList.size(); idx++) {
        auto const& mediaAttr = mediaAttrList.at(idx);
        SIP_CORE_DEBUG("[call:{:s}] Media @{:d} - {:s}", getCallId(), idx, mediaAttr.toString(true));
    }

    // Apply the media attributes.
    for (size_t idx = 0; idx < mediaAttrList.size(); idx++) {
        updateMediaStream(mediaAttrList[idx], idx);
    }

    if (inviteSession_->neg) {
        if (!prepareLocalMediaReservations(mediaAttrList)) {
            return;
        }

        if (!sdp_->processIncomingOffer(mediaAttrList)) {
            clearPendingLocalReservations();
            SIP_CORE_ERR("[call:%s] Could not prepare the SDP answer", getCallId().c_str());
            return;
        }
    }

    if (not inviteSession_->neg) {
        // We are answering to an INVITE that did not include a media offer (SDP).
        // The SIP specification (RFCs 3261/6337) requires that if a UA wishes to
        // proceed with the call, it must provide a media offer (SDP) if the initial
        // INVITE did not offer one. In this case, the SDP offer will be included in
        // the SIP OK (200) answer. The peer UA will then include its SDP answer in
        // the SIP ACK message.

        // TODO. This code should be unified with the code used by accounts to create
        // SDP offers.

        SIP_CORE_WARN("[call:%s] No negotiator session, peer sent an empty INVITE (without SDP)",
                      getCallId().c_str());

        Manager::instance().sipVoIPLink().createSDPOffer(inviteSession_.get());
    }

    if (!sdp_->getLocalSdpSession()) {
        clearPendingLocalReservations();
        SIP_CORE_ERR("[call:%s] No valid local SDP session available for answer",
                     getCallId().c_str());
        return;
    }

    if (!inviteSession_->last_answer)
        throw std::runtime_error("Should only be called for initial answer");

    // Set the SIP final answer (200 OK).
    pjsip_tx_data* tdata;
    if (pjsip_inv_answer(inviteSession_.get(), PJSIP_SC_OK, NULL, sdp_->getLocalSdpSession(), &tdata)
        != PJ_SUCCESS) {
        clearPendingLocalReservations();
        throw std::runtime_error("Could not init invite request answer (200 OK)");
    }

    if (contactHeader_.empty()) {
        clearPendingLocalReservations();
        throw std::runtime_error("Cant answer with an invalid contact header");
    }

    SIP_CORE_DBG("[call:%s] Answering with contact header: %s",
                 getCallId().c_str(),
                 contactHeader_.c_str());

    sip_utils::addContactHeader(contactHeader_, tdata);

    // Add user-agent header
    sip_utils::addUserAgentHeader(account->getUserAgentName(), tdata);

    if (pjsip_inv_send_msg(inviteSession_.get(), tdata) != PJ_SUCCESS) {
        clearPendingLocalReservations();
        setInviteSession();
        throw std::runtime_error("Could not send invite request answer (200 OK)");
    }

    setState(CallState::ACTIVE, ConnectionState::CONNECTED);
}

void
SIPCall::answerMediaChangeRequest(const std::vector<libsip_core::MediaMap>& mediaList, bool isRemote)
{
    std::lock_guard<std::recursive_mutex> lk {callMutex_};

    auto account = getSIPAccount();
    if (not account) {
        SIP_CORE_ERR("[call:%s] No account detected", getCallId().c_str());
        return;
    }

    auto mediaAttrList = MediaAttribute::buildMediaAttributesList(mediaList, isSrtpEnabled());

    // TODO. is the right place?
    // Disable video if disabled in the account.
    if (not account->isVideoEnabled()) {
        for (auto& mediaAttr : mediaAttrList) {
            if (mediaAttr.type_ == MediaType::MEDIA_VIDEO) {
                mediaAttr.enabled_ = false;
            }
        }
    }

    if (mediaAttrList.empty()) {
        SIP_CORE_WARN("[call:%s] Media list is empty. Ignoring the media change request",
                      getCallId().c_str());
        return;
    }

    if (not sdp_) {
        SIP_CORE_ERR("[call:%s] No valid SDP session", getCallId().c_str());
        return;
    }

    SIP_CORE_DBG("[call:%s] Current media", getCallId().c_str());
    unsigned idx = 0;
    for (auto const& rtp : rtpStreams_) {
        SIP_CORE_DBG("[call:%s] Media @%u: %s",
                     getCallId().c_str(),
                     idx++,
                     rtp.mediaAttribute_->toString(true).c_str());
    }

    SIP_CORE_DBG("[call:%s] Answering to media change request with new media", getCallId().c_str());
    idx = 0;
    for (auto const& newMediaAttr : mediaAttrList) {
        SIP_CORE_DBG("[call:%s] Media @%u: %s",
                     getCallId().c_str(),
                     idx++,
                     newMediaAttr.toString(true).c_str());
    }

    if (!updateAllMediaStreams(mediaAttrList, isRemote))
        return;

    if (!prepareLocalMediaReservations(mediaAttrList)) {
        return;
    }

    if (not sdp_->processIncomingOffer(mediaAttrList)) {
        clearPendingLocalReservations();
        SIP_CORE_WARN("[call:%s] Could not process the new offer, ignoring", getCallId().c_str());
        return;
    }

    if (not sdp_->getRemoteSdpSession()) {
        clearPendingLocalReservations();
        SIP_CORE_ERR("[call:%s] No valid remote SDP session", getCallId().c_str());
        return;
    }

    if (not sdp_->startNegotiation()) {
        clearPendingLocalReservations();
        SIP_CORE_ERR("[call:%s] Could not start media negotiation for a re-invite request",
                     getCallId().c_str());
        return;
    }

    if (pjsip_inv_set_sdp_answer(inviteSession_.get(), sdp_->getLocalSdpSession()) != PJ_SUCCESS) {
        clearPendingLocalReservations();
        SIP_CORE_ERR("[call:%s] Could not start media negotiation for a re-invite request",
                     getCallId().c_str());
        return;
    }

    pjsip_tx_data* tdata;
    if (pjsip_inv_answer(inviteSession_.get(), PJSIP_SC_OK, NULL, NULL, &tdata) != PJ_SUCCESS) {
        clearPendingLocalReservations();
        SIP_CORE_ERR("[call:%s] Could not init answer to a re-invite request", getCallId().c_str());
        return;
    }

    if (not contactHeader_.empty()) {
        sip_utils::addContactHeader(contactHeader_, tdata);
    }

    // Add user-agent header
    sip_utils::addUserAgentHeader(account->getUserAgentName(), tdata);

    if (pjsip_inv_send_msg(inviteSession_.get(), tdata) != PJ_SUCCESS) {
        clearPendingLocalReservations();
        SIP_CORE_ERR("[call:%s] Could not send answer to a re-invite request", getCallId().c_str());
        setInviteSession();
        return;
    }

    SIP_CORE_DBG("[call:%s] Successfully answered the media change request", getCallId().c_str());
}

void
SIPCall::hangup(int reason)
{
    std::lock_guard<std::recursive_mutex> lk {callMutex_};
    pendingRecord_ = false;
    if (inviteSession_ and inviteSession_->dlg) {
        pjsip_route_hdr* route = inviteSession_->dlg->route_set.next;
        while (route and route != &inviteSession_->dlg->route_set) {
            char buf[1024];
            int printed = pjsip_hdr_print_on(route, buf, sizeof(buf));
            if (printed >= 0) {
                buf[printed] = '\0';
                SIP_CORE_DBG("[call:%s] Route header %s", getCallId().c_str(), buf);
            }
            route = route->next;
        }

        int status = PJSIP_SC_OK;
        if (reason)
            status = reason;
        else if (inviteSession_->state <= PJSIP_INV_STATE_EARLY
                 and inviteSession_->role != PJSIP_ROLE_UAC)
            status = PJSIP_SC_CALL_TSX_DOES_NOT_EXIST;
        else if (inviteSession_->state >= PJSIP_INV_STATE_DISCONNECTED)
            status = PJSIP_SC_DECLINE;

        // Notify the peer
        terminateSipSession(status);
    }

    // Stop all RTP streams
    stopAllMedia();
    setState(Call::ConnectionState::DISCONNECTED, reason);
    removeCall();
}

void
SIPCall::detachAudioFromConference()
{
#ifdef ENABLE_VIDEO
    if (auto conf = getConference()) {
        if (auto mixer = conf->getVideoMixer()) {
            for (auto& stream : getRtpSessionList(MediaType::MEDIA_AUDIO)) {
                mixer->removeAudioOnlySource(getCallId(), stream->streamId());
            }
        }
    }
#endif
}

void
SIPCall::refuse()
{
    if (!isIncoming() or getConnectionState() == ConnectionState::CONNECTED or !inviteSession_)
        return;

    stopAllMedia();

    // Notify the peer
    terminateSipSession(PJSIP_SC_BUSY_HERE);

    setState(Call::ConnectionState::DISCONNECTED, ECONNABORTED);
    removeCall();
}

namespace {
struct TransferClientCtx
{
    std::string accountId;
    std::string callId;
};
} // anonymous namespace

static void
transfer_client_cb(pjsip_evsub* sub, pjsip_event* event)
{
    auto mod_ua_id = Manager::instance().sipVoIPLink().getModId();

    pjsip_evsub_state state = pjsip_evsub_get_state(sub);

    pjsip_status_line status_line {};

    pjsip_rx_data* r_data = event ? event->body.rx_msg.rdata : nullptr;

    switch (state) {
    case PJSIP_EVSUB_STATE_ACTIVE:
    case PJSIP_EVSUB_STATE_TERMINATED: {
        if (r_data && r_data->msg_info.msg && r_data->msg_info.len > 0) {
            std::string request(pjsip_rx_data_get_info(r_data));
            if (r_data->msg_info.msg->line.req.method.id == PJSIP_OTHER_METHOD
                and request.find("NOTIFY") != std::string::npos) {
                pjsip_msg_body* body = r_data->msg_info.msg->body;

                if (body) {
                    // may parse, or may not
                    pjsip_parse_status_line((char*) body->data, body->len, &status_line);
                }
            }
        }
        break;
    }

    case PJSIP_EVSUB_STATE_ACCEPTED:
    case PJSIP_EVSUB_STATE_NULL:
    case PJSIP_EVSUB_STATE_SENT:
    case PJSIP_EVSUB_STATE_PENDING:
    case PJSIP_EVSUB_STATE_UNKNOWN:
        break;
    }

    auto ctx = static_cast<TransferClientCtx*>(pjsip_evsub_get_mod_data(sub, mod_ua_id));
    if (ctx) {
        emitSignal<libsip_core::CallSignal::TransferStateChange>(ctx->accountId,
                                                                 ctx->callId,
                                                                 state,
                                                                 status_line.code,
                                                                 sip_utils::as_string(
                                                                     status_line.reason));
    }

    switch (state) {
    case PJSIP_EVSUB_STATE_ACCEPTED:
        if (!event)
            return;

        pj_assert(event->type == PJSIP_EVENT_TSX_STATE
                  && event->body.tsx_state.type == PJSIP_EVENT_RX_MSG);
        break;

    case PJSIP_EVSUB_STATE_TERMINATED: {
        // clean mod data on sub termination
        if (ctx) {
            delete ctx;
            pjsip_evsub_set_mod_data(sub, mod_ua_id, NULL);
        }
        break;
    }

    case PJSIP_EVSUB_STATE_ACTIVE: {
        if (!event)
            return;

        if (!r_data || !r_data->msg_info.cid)
            return;

        if (status_line.code / 100 == 2) {
            // clean mod data on success
            if (ctx) {
                delete ctx;
                pjsip_evsub_set_mod_data(sub, mod_ua_id, NULL);
            }
        }

        break;
    }

    case PJSIP_EVSUB_STATE_NULL:
    case PJSIP_EVSUB_STATE_SENT:
    case PJSIP_EVSUB_STATE_PENDING:
    case PJSIP_EVSUB_STATE_UNKNOWN:
    default:
        break;
    }
}

bool
SIPCall::transferCommon(const pj_str_t* dst)
{
    auto acc = getSIPAccount();
    if (not acc) {
        SIP_CORE_ERR("No account detected");
        return !PJ_SUCCESS;
    }

    if (not inviteSession_ or not inviteSession_->dlg)
        return false;

    pjsip_evsub_user xfer_cb;
    pj_bzero(&xfer_cb, sizeof(xfer_cb));
    xfer_cb.on_evsub_state = &transfer_client_cb;

    constexpr pj_str_t str_ref_by = CONST_PJ_STR("Referred-by");

    pjsip_evsub* sub;

    if (pjsip_xfer_create_uac(inviteSession_->dlg, &xfer_cb, &sub) != PJ_SUCCESS)
        return false;

    /* Associate context with the client subscription */
    auto* ctx = new TransferClientCtx {acc->getAccountID(), getCallId()};
    pjsip_evsub_set_mod_data(sub, Manager::instance().sipVoIPLink().getModId(), ctx);

    /*
     * Create REFER request.
     */
    pjsip_tx_data* tdata;

    if (pjsip_xfer_initiate(sub, dst, &tdata) != PJ_SUCCESS) {
        delete ctx;
        pjsip_evsub_set_mod_data(sub, Manager::instance().sipVoIPLink().getModId(), NULL);
        return false;
    }

    // add user agent and Referred-by header
    pjsip_generic_string_hdr* gs_hdr
        = pjsip_generic_string_hdr_create(tdata->pool,
                                          &str_ref_by,
                                          &inviteSession_->dlg->local.info_str);
    pjsip_msg_add_hdr(tdata->msg, reinterpret_cast<pjsip_hdr*>(gs_hdr));

    sip_utils::addUserAgentHeader(acc->getUserAgentName(), tdata);

    /* Send. */
    if (pjsip_xfer_send_request(sub, tdata) != PJ_SUCCESS) {
        delete ctx;
        pjsip_evsub_set_mod_data(sub, Manager::instance().sipVoIPLink().getModId(), NULL);
        return false;
    }

    return true;
}

void
SIPCall::transfer(const std::string& to)
{
    auto account = getSIPAccount();
    if (!account) {
        SIP_CORE_ERR("No account detected");
        return;
    }

    deinitRecorder();
    if (Call::isRecording())
        stopRecording();

    std::string toUri = account->getToUri(to);
    const pj_str_t dst(CONST_PJ_STR(toUri));
    SIP_CORE_DBG("[call:%s] Transferring to %.*s", getCallId().c_str(), (int) dst.slen, dst.ptr);

    if (!transferCommon(&dst))
        throw VoipLinkException("Couldn't transfer");
}

bool
SIPCall::attendedTransfer(const std::string& to)
{
    auto toCall = Manager::instance().callFactory.getCall<SIPCall>(to);
    if (!toCall)
        return false;

    if (not toCall->inviteSession_ or not toCall->inviteSession_->dlg)
        return false;

    pjsip_dialog* target_dlg = toCall->inviteSession_->dlg;
    pjsip_uri* uri = (pjsip_uri*) pjsip_uri_get_uri(target_dlg->remote.info->uri);

    char str_dest_buf[PJSIP_MAX_URL_SIZE * 2] = {'<'};
    pj_str_t dst = {str_dest_buf, 1};

    dst.slen += pjsip_uri_print(PJSIP_URI_IN_REQ_URI,
                                uri,
                                str_dest_buf + 1,
                                sizeof(str_dest_buf) - 1);
    dst.slen += pj_ansi_snprintf(str_dest_buf + dst.slen,
                                 sizeof(str_dest_buf) - dst.slen,
                                 "?"
                                 "Replaces=%.*s"
                                 "%%3bto-tag%%3d%.*s"
                                 "%%3bfrom-tag%%3d%.*s>",
                                 (int) target_dlg->call_id->id.slen,
                                 target_dlg->call_id->id.ptr,
                                 (int) target_dlg->remote.info->tag.slen,
                                 target_dlg->remote.info->tag.ptr,
                                 (int) target_dlg->local.info->tag.slen,
                                 target_dlg->local.info->tag.ptr);

    return transferCommon(&dst);
}

bool
SIPCall::onhold(OnReadyCb&& cb)
{
    auto result = hold();

    if (cb)
        cb(result);

    return result;
}

bool
SIPCall::hold()
{
    if (getConnectionState() != ConnectionState::CONNECTED) {
        SIP_CORE_WARN("[call:%s] Not connected, ignoring hold request", getCallId().c_str());
        return false;
    }

    if (not setState(CallState::HOLD)) {
        SIP_CORE_WARN("[call:%s] Failed to set state to HOLD", getCallId().c_str());
        return false;
    }

    stopAllMedia();

    for (auto& stream : rtpStreams_) {
        stream.mediaAttribute_->onHold_ = true;
    }

#ifdef ENABLE_VIDEO
    // Keep outbound video decodable during local hold even if re-INVITE is not forwarded.
    applyLocalHoldVideoBlackout(true, true);
#endif

    if (SIPSessionReinvite() != PJ_SUCCESS) {
        SIP_CORE_WARN("[call:%s] Reinvite failed", getCallId().c_str());
        return false;
    }

    SIP_CORE_DBG("[call:%s] Set state to HOLD", getCallId().c_str());
    return true;
}

bool
SIPCall::offhold(OnReadyCb&& cb)
{
    SIP_CORE_DBG("[call:%s] Resuming the call", getCallId().c_str());
    auto result = unhold();

    if (cb)
        cb(result);

    return result;
}

bool
SIPCall::unhold()
{
    auto account = getSIPAccount();
    if (!account) {
        SIP_CORE_ERR("No account detected");
        return false;
    }

    bool success = false;
    try {
        success = internalOffHold([] {});
    } catch (const SdpException& e) {
        SIP_CORE_ERR("[call:%s] %s", getCallId().c_str(), e.what());
        throw VoipLinkException("SDP issue in offhold");
    }

    return success;
}

bool
SIPCall::internalOffHold(const std::function<void()>& sdp_cb)
{
    if (getConnectionState() != ConnectionState::CONNECTED) {
        SIP_CORE_WARN("[call:%s] Not connected, ignoring resume request", getCallId().c_str());
    }

    if (not setState(CallState::ACTIVE))
        return false;

    sdp_cb();

    {
        for (auto& stream : rtpStreams_) {
            stream.mediaAttribute_->onHold_ = false;
        }
#ifdef ENABLE_VIDEO
        // Restore normal local/remote mute handling before negotiating hold-off.
        applyLocalHoldVideoBlackout(false, false);
#endif
        if (SIPSessionReinvite(getMediaAttributeList()) != PJ_SUCCESS) {
            SIP_CORE_WARN("[call:%s] resuming hold", getCallId().c_str());
            hold();
            return false;
        }
    }

    return true;
}

// switch or reload media input on the fly
bool
SIPCall::switchInput(const std::string& source)
{
#ifdef ENABLE_VIDEO
    SIP_CORE_DBG("[call:%s] Set selected source to %s", getCallId().c_str(), source.c_str());

    if (!isValidVideoSwitchSource(source)) {
        SIP_CORE_WARN("[call:%s] Rejecting unavailable video source '%s'",
                      getCallId().c_str(),
                      source.c_str());
        reportMediaNegotiationStatus(libsip_core::Media::MediaNegotiationStatusEvents::NEGOTIATION_FAIL);
        return false;
    }

    if (source.empty()) {
        auto currentMediaList = getMediaAttributeList();
        for (const auto& videoRtp : getRtpSessionList(MediaType::MEDIA_VIDEO)) {
            auto input = std::static_pointer_cast<video::VideoRtpSession>(videoRtp)->getVideoLocal();
            if (input)
                input->stopInput();
        }
        for (auto& media : currentMediaList) {
            if (media.type_ == MediaType::MEDIA_VIDEO) {
                media.sourceUri_ = source;
                media.muted_ = true;
            }
        }
        if (!updateAllMediaStreams(currentMediaList, false)) {
            reportMediaNegotiationStatus(libsip_core::Media::MediaNegotiationStatusEvents::NEGOTIATION_FAIL);
            return false;
        }
        reportMediaNegotiationStatus();

    } else {
        auto currentMediaList = getMediaAttributeList();
        for (auto& media : currentMediaList) {
            if (media.type_ == MediaType::MEDIA_VIDEO) {
                media.sourceUri_ = source;
                media.muted_ = false;
            }
        }
        if (!updateAllMediaStreams(currentMediaList, false)) {
            reportMediaNegotiationStatus(libsip_core::Media::MediaNegotiationStatusEvents::NEGOTIATION_FAIL);
            return false;
        }

        std::vector<std::shared_ptr<video::VideoInput>> inputsToSwitch;
        for (const auto& videoRtp : getRtpSessionList(MediaType::MEDIA_VIDEO)) {
            auto input = std::static_pointer_cast<video::VideoRtpSession>(videoRtp)->getVideoLocal();
            if (input)
                inputsToSwitch.emplace_back(std::move(input));
        }

        if (inputsToSwitch.empty()) {
            reportMediaNegotiationStatus();
        } else {
            auto remainingInputs = std::make_shared<std::atomic_size_t>(inputsToSwitch.size());
            auto switchFinished = std::make_shared<std::atomic_bool>(false);

            for (auto& input : inputsToSwitch) {
                std::weak_ptr<video::VideoInput> inputWeak = input;
                input->setSuccessfulSetupCb([callWeak = weak(),
                                             inputWeak,
                                             remainingInputs,
                                             switchFinished](MediaType, bool) {
                    if (auto localInput = inputWeak.lock()) {
                        localInput->setSuccessfulSetupCb({});
                        localInput->setFailedSetupCb({});
                    }

                    if (switchFinished->load())
                        return;

                    if (remainingInputs->fetch_sub(1) == 1) {
                        bool expected = false;
                        if (switchFinished->compare_exchange_strong(expected, true))
                            if (auto call = callWeak.lock())
                                call->reportMediaNegotiationStatus();
                    }
                });
                input->setFailedSetupCb([callWeak = weak(), inputWeak, switchFinished](MediaType) {
                    if (auto localInput = inputWeak.lock()) {
                        localInput->setSuccessfulSetupCb({});
                        localInput->setFailedSetupCb({});
                    }

                    bool expected = false;
                    if (switchFinished->compare_exchange_strong(expected, true))
                        if (auto call = callWeak.lock())
                            call->reportMediaNegotiationStatus(
                                libsip_core::Media::MediaNegotiationStatusEvents::NEGOTIATION_FAIL);
                });
                input->switchInput(source);
            }
        }
    }
    return true;
#endif
    return false;
}

void
SIPCall::peerHungup()
{
    pendingRecord_ = false;
    // Stop all RTP streams
    stopAllMedia();

    if (inviteSession_)
        terminateSipSession(PJSIP_SC_NOT_FOUND);
    detachAudioFromConference();
    Call::peerHungup();
}

void
SIPCall::carryingDTMFdigits(const std::string& dtmfEvents, double duration, unsigned int volume)
{
    auto account = getSIPAccount();
    if (not account) {
        SIP_CORE_ERR("[call:%s] No account detected", getCallId().c_str());
        return;
    }

    const std::string& dtmfType = account->getDtmfType();

    // this is handled at SIP layer
    if (dtmfType == SIPINFO_STR) {
        int duration = Manager::instance().voipPreferences.getPulseLength();

        for (auto code : dtmfEvents) {
            char dtmf_body[1000];
            int ret;

            // handle flash code
            if (code == '!') {
                ret = snprintf(dtmf_body,
                               sizeof dtmf_body - 1,
                               "Signal=16\r\nDuration=%d\r\n",
                               duration);
            } else {
                ret = snprintf(dtmf_body,
                               sizeof dtmf_body - 1,
                               "Signal=%c\r\nDuration=%d\r\n",
                               code,
                               duration);
            }

            try {
                sendSIPInfo({dtmf_body, (size_t) ret}, "dtmf-relay");
            } catch (const std::exception& e) {
                SIP_CORE_ERR("Error sending DTMF: %s", e.what());
            }
        }
    } else if (dtmfType == OVERRTP_STR) {
        // this is handled at RTP layer
        for (const auto& rtpSession : getRtpSessionList(MediaType::MEDIA_AUDIO)) {
            auto audioRtp = std::dynamic_pointer_cast<AudioRtpSession>(rtpSession);
            audioRtp->sendRtpEvents(dtmfEvents, duration, volume);
        }
    }
}

void
SIPCall::setVideoOrientation(int streamIdx, int rotation)
{
    std::string streamIdPart;
    if (streamIdx != -1)
        streamIdPart = fmt::format("<stream_id>{}</stream_id>", streamIdx);
    std::string sip_body = "<?xml version=\"1.0\" encoding=\"utf-8\" ?>"
                           "<media_control><vc_primitive><to_encoder>"
                           "<device_orientation="
                           + std::to_string(-rotation) + "/>" + "</to_encoder>" + streamIdPart
                           + "</vc_primitive></media_control>";

    SIP_CORE_DBG("Sending device orientation via SIP INFO %d for stream %u", rotation, streamIdx);

    // sendSIPInfo(sip_body, "media_control+xml");
}

void
SIPCall::sendTextMessage(const std::map<std::string, std::string>& messages, const std::string& from)
{
    std::lock_guard<std::recursive_mutex> lk {callMutex_};
    // TODO: for now we ignore the "from" (the previous implementation for sending this info was
    //      buggy and verbose), another way to send the original message sender will be implemented
    //      in the future
    if (not subcalls_.empty()) {
        pendingOutMessages_.emplace_back(messages, from);
        for (auto& c : subcalls_)
            c->sendTextMessage(messages, from);
    } else {
        if (inviteSession_) {
            try {
                // Ignore if the peer does not allow "MESSAGE" SIP method
                // NOTE:
                // The SIP "Allow" header is not mandatory as per RFC-3261. If it's
                // not present and since "MESSAGE" method is an extention method,
                // we choose to assume that the peer does not support the "MESSAGE"
                // method to prevent unexpected behavior when interoperating with
                // some SIP implementations.

                // if (not isSipMethodAllowedByPeer(sip_utils::SIP_METHODS::MESSAGE)) {
                //     SIP_CORE_WARN() << fmt::format("[call:{}] Peer does not allow \"{}\" method",
                //                                getCallId(),
                //                                sip_utils::SIP_METHODS::MESSAGE);

                //     // Print peer's allowed methods
                //     SIP_CORE_INFO() << fmt::format("[call:{}] Peer's allowed methods: {}",
                //                                getCallId(),
                //                                peerAllowedMethods_);
                //     return;
                // }

                im::sendSipMessage(inviteSession_.get(), messages);

            } catch (...) {
                SIP_CORE_ERR("[call:%s] Failed to send SIP text message", getCallId().c_str());
            }
        } else {
            pendingOutMessages_.emplace_back(messages, from);
            SIP_CORE_ERR("[call:%s] sendTextMessage: no invite session for this call",
                         getCallId().c_str());
        }
    }
}

void
SIPCall::removeCall()
{
    std::lock_guard<std::recursive_mutex> lk {callMutex_};
    SIP_CORE_DBG("[call:%s] removeCall()", getCallId().c_str());
    pendingAudioSocketPair_.reset();
#ifdef ENABLE_VIDEO
    pendingVideoSocketPair_.reset();
#endif
    if (sdp_) {
        sdp_->setActiveLocalSdpSession(nullptr);
        sdp_->setActiveRemoteSdpSession(nullptr);
        applyPendingLocalPortsToSdp();
    }
    Call::removeCall();

    setInviteSession();
    setSipTransport({});
}

void
SIPCall::onFailure(signed cause)
{
    if (setState(CallState::MERROR, ConnectionState::DISCONNECTED, cause)) {
        runOnMainThread([w = weak()] {
            if (auto shared = w.lock()) {
                auto& call = *shared;
                Manager::instance().callFailure(call);
                call.removeCall();
            }
        });
    }
}

void
SIPCall::onBusyHere()
{
    if (getCallType() == CallType::OUTGOING)
        setState(CallState::PEER_BUSY, ConnectionState::DISCONNECTED);
    else
        setState(CallState::BUSY, ConnectionState::DISCONNECTED);

    runOnMainThread([w = weak()] {
        if (auto shared = w.lock()) {
            auto& call = *shared;
            Manager::instance().callBusy(call);
            call.removeCall();
        }
    });
}

void
SIPCall::onClosed()
{
    runOnMainThread([w = weak()] {
        if (auto shared = w.lock()) {
            auto& call = *shared;
            Manager::instance().peerHungupCall(call);
            call.removeCall();
        }
    });
}

void
SIPCall::onAnswered()
{
    SIP_CORE_WARN("[call:%s] onAnswered()", getCallId().c_str());
    {
        std::lock_guard<std::recursive_mutex> lk {callMutex_};
        promoteEarlyMediaToActiveLocked();
    }
    runOnMainThread([w = weak()] {
        if (auto shared = w.lock()) {
            if (shared->getConnectionState() != ConnectionState::CONNECTED) {
                shared->setState(CallState::ACTIVE, ConnectionState::CONNECTED);
                if (not shared->isSubcall()) {
                    Manager::instance().peerAnsweredCall(*shared);
                }
            }
        }
    });
}

void
SIPCall::onEarlyMediaProgress183()
{
    std::lock_guard<std::recursive_mutex> lk {callMutex_};

    if (getCallType() != CallType::OUTGOING or !inviteSession_ or !sdp_) {
        return;
    }

    if (inviteSession_->state != PJSIP_INV_STATE_EARLY
        || getConnectionState() == ConnectionState::CONNECTED) {
        return;
    }

    SIP_CORE_DBG("[call:%s] Received 183 Session Progress: enabling early audio media",
                 getCallId().c_str());

    earlyMediaRequested_ = true;

    // If SDP is not active yet, early media startup will happen from onMediaNegotiationComplete().
    if (!sdp_->getActiveLocalSdpSession() or !sdp_->getActiveRemoteSdpSession()) {
        SIP_CORE_DBG("[call:%s] 183 received before active SDP; waiting media negotiation callback",
                     getCallId().c_str());
        return;
    }

    setupNegotiatedMedia();
    stopAllMedia();
    updateRemoteMedia();
    startEarlyMediaLocked();
}

void
SIPCall::startEarlyMediaLocked()
{
    bool started = false;

    for (const auto& stream : rtpStreams_) {
        if (!stream.rtpSession_ || stream.rtpSession_->getMediaType() != MediaType::MEDIA_AUDIO) {
            continue;
        }

        auto audioRtp = std::dynamic_pointer_cast<AudioRtpSession>(stream.rtpSession_);
        if (!audioRtp) {
            continue;
        }

        audioRtp->startEarlyMedia();
        started = true;
    }

    earlyMediaStarted_ = started;
    if (!started) {
        SIP_CORE_WARN("[call:%s] Early media requested but no audio RTP session is available",
                      getCallId().c_str());
    }
}

void
SIPCall::promoteEarlyMediaToActiveLocked()
{
    if (!earlyMediaStarted_) {
        return;
    }

    SIP_CORE_DBG("[call:%s] Promoting early media to active media", getCallId().c_str());

    for (const auto& stream : rtpStreams_) {
        if (!stream.rtpSession_ || stream.rtpSession_->getMediaType() != MediaType::MEDIA_AUDIO) {
            continue;
        }

        auto audioRtp = std::dynamic_pointer_cast<AudioRtpSession>(stream.rtpSession_);
        if (!audioRtp) {
            continue;
        }

        audioRtp->promoteEarlyMediaToActive();
    }

    earlyMediaStarted_ = false;
    earlyMediaRequested_ = false;
}

void
SIPCall::sendKeyframe(int streamIdx)
{
#ifdef ENABLE_VIDEO
    SIP_CORE_DBG("handling picture fast update request");
    if (streamIdx == -1) {
        for (const auto& videoRtp : getRtpSessionList(MediaType::MEDIA_VIDEO))
            std::static_pointer_cast<video::VideoRtpSession>(videoRtp)->forceKeyFrame();
    } else if (streamIdx > -1 && streamIdx < static_cast<int>(rtpStreams_.size())) {
        // Apply request for wanted stream
        auto& stream = rtpStreams_[streamIdx];
        if (stream.rtpSession_ && stream.rtpSession_->getMediaType() == MediaType::MEDIA_VIDEO)
            std::static_pointer_cast<video::VideoRtpSession>(stream.rtpSession_)->forceKeyFrame();
    }

#endif
}

void
SIPCall::setInviteCallId(std::string_view inviteCallId)
{
    inviteCallId_ = inviteCallId;
}

void
SIPCall::setPeerUaVersion(std::string_view ua)
{
    if (peerUserAgent_ == ua or ua.empty()) {
        // Silently ignore if it did not change or empty.
        return;
    }

    if (peerUserAgent_.empty()) {
        SIP_CORE_DBG("[call:%s] Set peer's User-Agent to [%.*s]",
                     getCallId().c_str(),
                     (int) ua.size(),
                     ua.data());
    } else if (not peerUserAgent_.empty()) {
        // Unlikely, but should be handled since we dont have control over the peer.
        // Even if it's unexpected, we still try to parse the UA version.
        SIP_CORE_WARN("[call:%s] Peer's User-Agent unexpectedly changed from [%s] to [%.*s]",
                      getCallId().c_str(),
                      peerUserAgent_.c_str(),
                      (int) ua.size(),
                      ua.data());
    }

    peerUserAgent_ = ua;

    // User-agent parsing
    constexpr std::string_view PACK_NAME(PACKAGE_NAME " ");
    auto pos = ua.find(PACK_NAME);
    if (pos == std::string_view::npos) {
        // Must have the expected package name.
        SIP_CORE_WARN("Could not find the expected package name in peer's User-Agent");
        return;
    }

    ua = ua.substr(pos + PACK_NAME.length());

    std::string_view version;
    // Unstable (un-released) versions has a hiphen + commit Id after
    // the version number. Find the commit Id if any, and ignore it.
    pos = ua.find('-');
    if (pos != std::string_view::npos) {
        // Get the version and ignore the commit ID.
        version = ua.substr(0, pos);
    } else {
        // Extract the version number.
        pos = ua.find(' ');
        if (pos != std::string_view::npos) {
            version = ua.substr(0, pos);
        }
    }

    if (version.empty()) {
        SIP_CORE_DBG("[call:%s] Could not parse peer's version", getCallId().c_str());
        return;
    }

    auto peerVersion = split_string_to_unsigned(version, '.');
    if (peerVersion.size() > 4u) {
        SIP_CORE_WARN("[call:%s] Could not parse peer's version", getCallId().c_str());
        return;
    }
}

void
SIPCall::setPeerAllowMethods(std::vector<std::string> methods)
{
    std::lock_guard<std::recursive_mutex> lock {callMutex_};
    peerAllowedMethods_ = std::move(methods);
}

bool
SIPCall::isSipMethodAllowedByPeer(const std::string_view method) const
{
    std::lock_guard<std::recursive_mutex> lock {callMutex_};

    return std::find(peerAllowedMethods_.begin(), peerAllowedMethods_.end(), method)
           != peerAllowedMethods_.end();
}

void
SIPCall::onPeerRinging()
{
    SIP_CORE_DBG("[call:%s] Peer ringing", getCallId().c_str());
    setState(ConnectionState::RINGING);
}

std::shared_ptr<AccountCodecInfo>
SIPCall::getVideoCodec() const
{
#ifdef ENABLE_VIDEO
    // Return first video codec as we negotiate only one codec for the call
    // Note: with multistream we can negotiate codecs/stream, but it's not the case
    // in practice (same for audio), so just return the first video codec.
    for (const auto& videoRtp : getRtpSessionList(MediaType::MEDIA_VIDEO))
        return videoRtp->getCodec();
#endif
    return {};
}

std::shared_ptr<AccountCodecInfo>
SIPCall::getAudioCodec() const
{
    // Return first video codec as we negotiate only one codec for the call
    for (const auto& audioRtp : getRtpSessionList(MediaType::MEDIA_AUDIO))
        return audioRtp->getCodec();
    return {};
}

void
SIPCall::addMediaStream(const MediaAttribute& mediaAttr)
{
    // Create and add the media stream with the provided attribute.
    // Do not create the RTP sessions yet.
    RtpStream stream;
    stream.mediaAttribute_ = std::make_shared<MediaAttribute>(mediaAttr);

    // Set default media source if empty. Kept for backward compatibility.
#ifdef ENABLE_VIDEO
    if (stream.mediaAttribute_->sourceUri_.empty()) {
        stream.mediaAttribute_->sourceUri_
            = Manager::instance().getVideoManager().videoDeviceMonitor.getMRLForDefaultDevice();
    }
#endif

    rtpStreams_.emplace_back(std::move(stream));
}

size_t
SIPCall::initMediaStreams(const std::vector<MediaAttribute>& mediaAttrList)
{
    for (size_t idx = 0; idx < mediaAttrList.size(); idx++) {
        auto const& mediaAttr = mediaAttrList.at(idx);
        if (mediaAttr.type_ != MEDIA_AUDIO && mediaAttr.type_ != MEDIA_VIDEO) {
            SIP_CORE_ERR("[call:%s] Unexpected media type %u", getCallId().c_str(), mediaAttr.type_);
            assert(false);
        }

        addMediaStream(mediaAttr);
        auto& stream = rtpStreams_.back();
        createRtpSession(stream);

        SIP_CORE_DEBUG("[call:{:s}] Added media @{:d}: {:s}",
                       getCallId(),
                       idx,
                       stream.mediaAttribute_->toString(true));
    }

    SIP_CORE_DEBUG("[call:{:s}] Created {:d} Media streams", getCallId(), rtpStreams_.size());

    return rtpStreams_.size();
}

bool
SIPCall::hasVideo() const
{
#ifdef ENABLE_VIDEO
    std::function<bool(const RtpStream& stream)> videoCheck = [](auto const& stream) {
        bool validVideo = stream.mediaAttribute_ && stream.mediaAttribute_->hasValidVideo();
        bool validRemoteVideo = stream.remoteMediaAttribute_
                                && stream.remoteMediaAttribute_->hasValidVideo();
        return validVideo || validRemoteVideo;
    };

    const auto iter = std::find_if(rtpStreams_.begin(), rtpStreams_.end(), videoCheck);

    return iter != rtpStreams_.end();
#else
    return false;
#endif
}

bool
SIPCall::isCaptureDeviceMuted(const MediaType& mediaType) const
{
    // Return true only if all media of type 'mediaType' that use capture devices
    // source, are muted.
    std::function<bool(const RtpStream& stream)> mutedCheck = [&mediaType](auto const& stream) {
        return (stream.mediaAttribute_->type_ == mediaType and not stream.mediaAttribute_->muted_);
    };
    const auto iter = std::find_if(rtpStreams_.begin(), rtpStreams_.end(), mutedCheck);
    return iter == rtpStreams_.end();
}

void
SIPCall::setupNegotiatedMedia()
{
    SIP_CORE_DBG("[call:%s] updating negotiated media", getCallId().c_str());

    if (not sipTransport_ or not sdp_) {
        SIP_CORE_ERR("[call:%s] the call is in invalid state", getCallId().c_str());
        return;
    }

    auto slots = sdp_->getMediaSlots();
    bool peer_holding {true};
    int streamIdx = -1;

    for (const auto& slot : slots) {
        streamIdx++;
        const auto& local = slot.first;
        const auto& remote = slot.second;

        if (static_cast<size_t>(streamIdx) >= rtpStreams_.size()) {
            SIP_CORE_WARN("[call:%s] Stream index is out-of-range, skipping", getCallId().c_str());
            continue;
        }

        auto const& rtpStream = rtpStreams_[streamIdx];

        if (not rtpStream.mediaAttribute_) {
            throw std::runtime_error("Missing media attribute");
        }

        rtpStream.mediaAttribute_->enabled_ = local.enabled;

        if (not rtpStream.rtpSession_)
            throw std::runtime_error("Must have a valid RTP Session");

        if (local.type != MEDIA_AUDIO && local.type != MEDIA_VIDEO) {
            SIP_CORE_ERR("[call:%s] Unexpected media type %u", getCallId().c_str(), local.type);
            throw std::runtime_error("Invalid media attribute");
        }

        if (local.type != remote.type) {
            SIP_CORE_ERR("[call:%s] [SDP:slot#%u] Inconsistent media type between local and remote",
                         getCallId().c_str(),
                         streamIdx);
            continue;
        }

        if (local.type == MediaType::MEDIA_VIDEO) {
            if (local.direction_ != MediaDirection::SENDRECV) {
                SIP_CORE_WARN(
                    "[call:%s] [SDP:slot#%u] Local video direction is '%s' (expected 'sendrecv'). "
                    "Keeping RTP mute-based handling.",
                    getCallId().c_str(),
                    streamIdx,
                    mediaDirectionToString(local.direction_));
            }
            if (remote.direction_ != MediaDirection::SENDRECV) {
                SIP_CORE_WARN(
                    "[call:%s] [SDP:slot#%u] Remote video direction is '%s' (expected 'sendrecv'). "
                    "Keeping RTP mute-based handling.",
                    getCallId().c_str(),
                    streamIdx,
                    mediaDirectionToString(remote.direction_));
            }
        }

        if (local.enabled and not local.codec) {
            SIP_CORE_WARN("[call:%s] [SDP:slot#%u] Missing local codec",
                          getCallId().c_str(),
                          streamIdx);
            continue;
        }

        if (remote.enabled and not remote.codec) {
            SIP_CORE_WARN("[call:%s] [SDP:slot#%u] Missing remote codec",
                          getCallId().c_str(),
                          streamIdx);
            continue;
        }

        if (isSrtpEnabled() and local.enabled and not local.crypto) {
            SIP_CORE_WARN("[call:%s] [SDP:slot#%u] Secure mode but no local crypto attributes. "
                          "Ignoring the media",
                          getCallId().c_str(),
                          streamIdx);
            continue;
        }

        if (isSrtpEnabled() and remote.enabled and not remote.crypto) {
            SIP_CORE_WARN("[call:%s] [SDP:slot#%u] Secure mode but no crypto remote attributes. "
                          "Ignoring the media",
                          getCallId().c_str(),
                          streamIdx);
            continue;
        }

        // Aggregate holding info over all remote streams
        peer_holding &= remote.onHold;

        configureRtpSession(rtpStream.rtpSession_, rtpStream.mediaAttribute_, local, remote);
    }

    if (pendingAudioSocketPair_) {
        SIP_CORE_WARN("[call:%s] Releasing unused pending audio transport reservation",
                      getCallId().c_str());
        pendingAudioSocketPair_.reset();
    }
#ifdef ENABLE_VIDEO
    if (pendingVideoSocketPair_) {
        SIP_CORE_WARN("[call:%s] Releasing unused pending video transport reservation",
                      getCallId().c_str());
        pendingVideoSocketPair_.reset();
    }
#endif

    // TODO. Do we really use this?
    if (not isSubcall() and peerHolding_ != peer_holding) {
        peerHolding_ = peer_holding;
        emitSignal<libsip_core::CallSignal::PeerHold>(getCallId(), peerHolding_);
    }
}

void
SIPCall::startAllMedia()
{
    SIP_CORE_DBG("[call:%s] Starting all media", getCallId().c_str());

    if (not sipTransport_ or not sdp_) {
        SIP_CORE_ERR("[call:%s] The call is in invalid state", getCallId().c_str());
        return;
    }

    if (isSrtpEnabled() && not sipTransport_->isSecure()) {
        SIP_CORE_WARN("[call:%s] Crypto (SRTP) is negotiated over an insecure signaling transport",
                      getCallId().c_str());
    }

    // reset
    readyToRecord_ = false;

    for (auto iter = rtpStreams_.begin(); iter != rtpStreams_.end(); iter++) {
        if (not iter->mediaAttribute_) {
            throw std::runtime_error("Missing media attribute");
        }

        // Not restarting media loop on hold as it's a huge waste of CPU ressources
        // because of the audio loop
        if (getState() != CallState::HOLD) {
            iter->rtpSession_->start();
        }
    }

#ifdef ENABLE_VIDEO
    if (getState() == CallState::HOLD) {
        // Keep audio stopped on local hold, but continue sending black video frames.
        applyLocalHoldVideoBlackout(true, true);
    }
#endif

    // Media is restarted, we can process the last holding request.
    if (remainingRequest_ != Request::NoRequest) {
        bool result = true;
        switch (remainingRequest_) {
        case Request::HoldingOn:
            result = hold();
            if (holdCb_) {
                holdCb_(result);
                holdCb_ = nullptr;
            }
            break;
        case Request::HoldingOff:
            result = unhold();
            if (offHoldCb_) {
                offHoldCb_(result);
                offHoldCb_ = nullptr;
            }
            break;
        case Request::SwitchInput:
            SIPSessionReinvite();
            break;
        default:
            break;
        }
        remainingRequest_ = Request::NoRequest;
    }
}

#ifdef ENABLE_VIDEO
void
SIPCall::applyLocalHoldVideoBlackout(bool enable, bool startSessionsIfNeeded)
{
    for (auto& stream : rtpStreams_) {
        if (!stream.rtpSession_ || !stream.mediaAttribute_
            || stream.mediaAttribute_->type_ != MediaType::MEDIA_VIDEO) {
            continue;
        }

        auto videoRtp = std::dynamic_pointer_cast<video::VideoRtpSession>(stream.rtpSession_);
        if (!videoRtp) {
            continue;
        }

        if (enable) {
            SIP_CORE_DBG("[call:%s] [%s] enabling hold video blackout",
                         getCallId().c_str(),
                         stream.mediaAttribute_->label_.c_str());
            videoRtp->setMuted(true, RtpSession::Direction::SEND);
            videoRtp->setMuted(true, RtpSession::Direction::RECV);
            if (startSessionsIfNeeded) {
                videoRtp->start();
            }
            continue;
        }

        const bool localMuted = stream.mediaAttribute_->muted_;
        const bool remoteMuted = stream.remoteMediaAttribute_ ? stream.remoteMediaAttribute_->muted_
                                                              : false;
        SIP_CORE_DBG("[call:%s] [%s] disabling hold video blackout (localMuted=%s, remoteMuted=%s)",
                     getCallId().c_str(),
                     stream.mediaAttribute_->label_.c_str(),
                     localMuted ? "true" : "false",
                     remoteMuted ? "true" : "false");
        videoRtp->setMuted(localMuted, RtpSession::Direction::SEND);
        videoRtp->setMuted(remoteMuted, RtpSession::Direction::RECV);
    }
}
#endif

void
SIPCall::restartMediaSender()
{
    SIP_CORE_DBG("[call:%s] restarting TX media streams", getCallId().c_str());
    for (const auto& rtpSession : getRtpSessionList())
        rtpSession->restartSender();
}

void
SIPCall::stopAllMedia()
{
    SIP_CORE_DBG("[call:%s] Stopping all media", getCallId().c_str());

#ifdef ENABLE_VIDEO
    {
        std::lock_guard<std::mutex> lk(sinksMtx_);
        for (auto it = callSinksMap_.begin(); it != callSinksMap_.end();) {
            for (const auto& videoRtp : getRtpSessionList(MediaType::MEDIA_VIDEO)) {
                auto& videoReceive = std::static_pointer_cast<video::VideoRtpSession>(videoRtp)
                                         ->getVideoReceive();
                if (videoReceive) {
                    auto& sink = videoReceive->getSink();
                    sink->detach(it->second.get());
                }
            }
            it->second->stop();
            it = callSinksMap_.erase(it);
        }
    }
#endif
    for (const auto& rtpSession : getRtpSessionList())
        rtpSession->stop();
}

void
SIPCall::updateRemoteMedia()
{
    SIP_CORE_DBG("[call:%s] Updating remote media", getCallId().c_str());

    auto remoteMediaList = Sdp::getMediaAttributeListFromSdp(sdp_->getActiveRemoteSdpSession());

    if (remoteMediaList.size() != rtpStreams_.size()) {
        SIP_CORE_ERR("[call:%s] Media size mismatch!", getCallId().c_str());
        return;
    }

    for (size_t idx = 0; idx < remoteMediaList.size(); idx++) {
        auto& rtpStream = rtpStreams_[idx];
        auto const& remoteMedia = rtpStream.remoteMediaAttribute_ = std::make_shared<MediaAttribute>(
            remoteMediaList[idx]);
        if (remoteMedia->type_ == MediaType::MEDIA_VIDEO) {
            rtpStream.rtpSession_->setMuted(remoteMedia->muted_, RtpSession::Direction::RECV);
            SIP_CORE_DEBUG("[call:{:s}] Remote media @ {:d}: {:s}",
                           getCallId(),
                           idx,
                           remoteMedia->toString());
            // Request a key-frame if we are un-muting the video
            if (not remoteMedia->muted_)
                requestKeyframe(findRtpStreamIndex(remoteMedia->label_));
        }
    }
}

void
SIPCall::muteMedia(const std::string& mediaType, bool mute)
{
    auto type = MediaAttribute::stringToMediaType(mediaType);

    if (type == MediaType::MEDIA_AUDIO) {
        SIP_CORE_WARN("[call:%s] %s all audio medias",
                      getCallId().c_str(),
                      mute ? "muting " : "un-muting ");

    } else if (type == MediaType::MEDIA_VIDEO) {
        SIP_CORE_WARN("[call:%s] %s all video medias",
                      getCallId().c_str(),
                      mute ? "muting" : "un-muting");
    } else {
        SIP_CORE_ERR("[call:%s] invalid media type %s", getCallId().c_str(), mediaType.c_str());
        assert(false);
    }

    // Get the current media attributes.
    auto mediaList = getMediaAttributeList();

    // Mute/Un-mute all medias with matching type.
    for (auto& mediaAttr : mediaList) {
        if (mediaAttr.type_ == type) {
            mediaAttr.muted_ = mute;
        }
    }

    // Apply
    requestMediaChange(MediaAttribute::mediaAttributesToMediaMaps(mediaList));
}

void
SIPCall::updateMediaStream(const MediaAttribute& newMediaAttr, size_t streamIdx)
{
    assert(streamIdx < rtpStreams_.size());

    auto const& rtpStream = rtpStreams_[streamIdx];
    assert(rtpStream.rtpSession_);

    auto const& mediaAttr = rtpStream.mediaAttribute_;
    assert(mediaAttr);

    bool notifyMute = false;

    if (newMediaAttr.muted_ == mediaAttr->muted_) {
        // Nothing to do. Already in the desired state.
        SIP_CORE_DBG("[call:%s] [%s] already %s",
                     getCallId().c_str(),
                     mediaAttr->label_.c_str(),
                     mediaAttr->muted_ ? "muted " : "un-muted ");

    } else {
        // Update
        mediaAttr->muted_ = newMediaAttr.muted_;
        notifyMute = true;
        SIP_CORE_DBG("[call:%s] %s [%s]",
                     getCallId().c_str(),
                     mediaAttr->muted_ ? "muting" : "un-muting",
                     mediaAttr->label_.c_str());
    }

    // Only update source and type if actually set.
    if (mediaAttr->type_ == MediaType::MEDIA_VIDEO) {
        mediaAttr->sourceUri_ = newMediaAttr.sourceUri_;
        rtpStream.rtpSession_->setMediaSource(mediaAttr->sourceUri_);
    }

    if (notifyMute and mediaAttr->type_ == MediaType::MEDIA_AUDIO) {
        rtpStream.rtpSession_->setMuted(mediaAttr->muted_);
        sendMuteState(mediaAttr->muted_);
        if (not isSubcall())
            emitSignal<libsip_core::CallSignal::AudioMuted>(getCallId(), mediaAttr->muted_);
        return;
    }

#ifdef ENABLE_VIDEO
    if (notifyMute and mediaAttr->type_ == MediaType::MEDIA_VIDEO) {
        rtpStream.rtpSession_->setMuted(mediaAttr->muted_);
        if (not isSubcall())
            emitSignal<libsip_core::CallSignal::VideoMuted>(getCallId(), mediaAttr->muted_);
    }
#endif
}

bool
SIPCall::updateAllMediaStreams(const std::vector<MediaAttribute>& mediaAttrList, bool isRemote)
{
    SIP_CORE_DBG("[call:%s] New local media", getCallId().c_str());
    (void) isRemote;

    if (mediaAttrList.size() > PJ_ICE_MAX_COMP / 2) {
        SIP_CORE_DEBUG("[call:{:s}] Too many medias, limit it ({:d} vs {:d})",
                       getCallId().c_str(),
                       mediaAttrList.size(),
                       PJ_ICE_MAX_COMP);
        return false;
    }

    unsigned idx = 0;
    for (auto const& newMediaAttr : mediaAttrList) {
        SIP_CORE_DBG("[call:%s] Media @%u: %s",
                     getCallId().c_str(),
                     idx++,
                     newMediaAttr.toString(true).c_str());
    }

    SIP_CORE_DBG("[call:%s] Updating local media streams", getCallId().c_str());

    for (auto const& newAttr : mediaAttrList) {
        auto streamIdx = findRtpStreamIndex(newAttr.label_);

        if (streamIdx < 0) {
            // Media does not exist, add a new one.
            auto normalizedAttr = newAttr;
            if (normalizedAttr.type_ == MediaType::MEDIA_VIDEO) {
                if (!normalizedAttr.muted_) {
                    SIP_CORE_DBG(
                        "[call:%s] New negotiated video stream [%s] forced muted by default policy",
                        getCallId().c_str(),
                        normalizedAttr.label_.c_str());
                } else {
                    SIP_CORE_DBG("[call:%s] New negotiated video stream [%s] is kept muted "
                                 "(default policy)",
                                 getCallId().c_str(),
                                 normalizedAttr.label_.c_str());
                }
                normalizedAttr.muted_ = true;
            }
            addMediaStream(normalizedAttr);
            auto& stream = rtpStreams_.back();
            createRtpSession(stream);
            SIP_CORE_DBG("[call:%s] Added a new media stream [%s] @ index %i",
                         getCallId().c_str(),
                         stream.mediaAttribute_->label_.c_str(),
                         streamIdx);
        } else {
            updateMediaStream(newAttr, streamIdx);
        }
    }

    if (mediaAttrList.size() < rtpStreams_.size()) {
#ifdef ENABLE_VIDEO
        // remove all reduntant video streams from conference
        for (auto i = mediaAttrList.size(); i < rtpStreams_.size(); ++i) {
            auto& stream = rtpStreams_[i];
            if (stream.rtpSession_->getMediaType() == MediaType::MEDIA_VIDEO)
                std::static_pointer_cast<video::VideoRtpSession>(stream.rtpSession_)
                    ->exitConference();
        }
#endif
        rtpStreams_.resize(mediaAttrList.size());
    }
    return true;
}

bool
SIPCall::isReinviteRequired(const std::vector<MediaAttribute>& mediaAttrList)
{
    if (mediaAttrList.size() != rtpStreams_.size())
        return true;

    for (auto const& newAttr : mediaAttrList) {
        auto streamIdx = findRtpStreamIndex(newAttr.label_);

        if (streamIdx < 0
            || // Always needs a re-invite when a new media is added.
               // newAttr.sourceUri_ != rtpStreams_[streamIdx].mediaAttribute_->sourceUri_ || //
               // Changing the video source currently does not work via reinvite :)
            newAttr.enabled_
                != rtpStreams_[streamIdx]
                       .mediaAttribute_->enabled_ // Also check if 'enabled' has changed
        ) {
            return true;
        }
    }

    return false;
}

bool
SIPCall::isRestartRequired(const std::vector<MediaAttribute>& mediaAttrList)
{
    // if (mediaAttrList.size() != rtpStreams_.size())
    //     return false;

    // for (auto const& newAttr : mediaAttrList) {
    //     auto streamIdx = findRtpStreamIndex(newAttr.label_);

    //     // Changing the source in current setup needs a restart
    //     if (newAttr.sourceUri_ != rtpStreams_[streamIdx].mediaAttribute_->sourceUri_) {
    //         return true;
    //     }
    // }
    return false;
}

bool
SIPCall::requestMediaChange(const std::vector<libsip_core::MediaMap>& mediaList)
{
    std::lock_guard<std::recursive_mutex> lk {callMutex_};
    auto mediaAttrList = MediaAttribute::buildMediaAttributesList(mediaList, isSrtpEnabled());

    // Disable video if disabled in the account.
    auto account = getSIPAccount();
    if (not account) {
        SIP_CORE_ERR("[call:%s] No account detected", getCallId().c_str());
        return false;
    }
    if (not account->isVideoEnabled()) {
        for (auto& mediaAttr : mediaAttrList) {
            if (mediaAttr.type_ == MediaType::MEDIA_VIDEO) {
                // This an API misuse. The new medialist should not contain video
                // if it was disabled in the account settings.
                SIP_CORE_ERR("[call:%s] New media has video, but it's disabled in the account. "
                             "Ignoring the change request!",
                             getCallId().c_str());
                return false;
            }
        }
    }

    MediaAttribute audioAttr;
    MediaAttribute videoAttr;

    // find audio, video media
    auto hasVideo = false, hasAudio = false;
    for (auto it = mediaAttrList.rbegin(); it != mediaAttrList.rend(); ++it) {
        if (it->type_ == MediaType::MEDIA_VIDEO && !hasVideo) {
            videoAttr = *it;
            videoAttr.label_ = sip_utils::DEFAULT_VIDEO_STREAMID;
            hasVideo = true;
        } else if (it->type_ == MediaType::MEDIA_AUDIO && !hasAudio) {
            audioAttr = *it;
            audioAttr.label_ = sip_utils::DEFAULT_AUDIO_STREAMID;
            hasAudio = true;
        }
        if (hasVideo && hasAudio)
            break;
    }
    mediaAttrList.clear();
    // Note: use the order VIDEO/AUDIO to avoid reinvite.
    mediaAttrList.emplace_back(audioAttr);
    if (hasVideo)
        mediaAttrList.emplace_back(videoAttr);
    SIP_CORE_DBG("[call:%s] Requesting media change. List of new media:", getCallId().c_str());

    unsigned idx = 0;
    for (auto const& newMediaAttr : mediaAttrList) {
        SIP_CORE_DBG("[call:%s] Media @%u: %s",
                     getCallId().c_str(),
                     idx++,
                     newMediaAttr.toString(true).c_str());
    }

    auto needReinvite = isReinviteRequired(mediaAttrList);
    auto needMediaRestart = isRestartRequired(mediaAttrList);

    if (!updateAllMediaStreams(mediaAttrList, false))
        return false;

    if (needReinvite) {
        SIP_CORE_DBG("[call:%s] Media change requires a new negotiation (re-invite)",
                     getCallId().c_str());
        requestReinvite(mediaAttrList);
    } else if (needMediaRestart) {
        SIP_CORE_DBG("[call:%s] Media change DOES NOT require a new negotiation (restart), but "
                     "requires a restart",
                     getCallId().c_str());
        this->mediaRestartRequired_ = true;
        onMediaNegotiationComplete();
    } else {
        SIP_CORE_DBG(
            "[call:%s] Media change DOES NOT require a new negotiation (re-invite) and restart",
            getCallId().c_str());
        reportMediaNegotiationStatus();
    }

    return true;
}

std::vector<std::map<std::string, std::string>>
SIPCall::currentMediaList() const
{
    return MediaAttribute::mediaAttributesToMediaMaps(getMediaAttributeList());
}

std::vector<MediaAttribute>
SIPCall::getMediaAttributeList() const
{
    std::lock_guard<std::recursive_mutex> lk {callMutex_};
    std::vector<MediaAttribute> mediaList;
    mediaList.reserve(rtpStreams_.size());
    for (auto const& stream : rtpStreams_)
        mediaList.emplace_back(*stream.mediaAttribute_);
    return mediaList;
}

void
SIPCall::onMediaNegotiationComplete()
{
    std::lock_guard<std::recursive_mutex> lk {callMutex_};
    SIP_CORE_DBG("[call:%s] Media negotiation complete", getCallId().c_str());

    // If the call has already ended, we don't need to start the media.
    if (not inviteSession_ or inviteSession_->state == PJSIP_INV_STATE_DISCONNECTED or not sdp_) {
        return;
    }

    // Update the negotiated media.
    if (mediaRestartRequired_) {
        setupNegotiatedMedia();
        // No ICE, start media now.
        SIP_CORE_WARN("[call:%s] ICE media disabled, using default media ports",
                      getCallId().c_str());
        // RESTART the media always....
        stopAllMedia();
        updateRemoteMedia();

        const bool startEarlyMedia = getCallType() == CallType::OUTGOING and inviteSession_
                                     and inviteSession_->state == PJSIP_INV_STATE_EARLY
                                     and earlyMediaRequested_
                                     and getConnectionState() != ConnectionState::CONNECTED;

        if (startEarlyMedia) {
            SIP_CORE_DBG("[call:%s] Starting early audio media after negotiation",
                         getCallId().c_str());
            startEarlyMediaLocked();
        } else {
            if (earlyMediaStarted_) {
                for (const auto& stream : rtpStreams_) {
                    if (!stream.rtpSession_
                        || stream.rtpSession_->getMediaType() != MediaType::MEDIA_AUDIO) {
                        continue;
                    }

                    auto audioRtp = std::dynamic_pointer_cast<AudioRtpSession>(stream.rtpSession_);
                    if (audioRtp) {
                        audioRtp->stopEarlyMedia();
                    }
                }
                earlyMediaStarted_ = false;
            }

            startAllMedia();
            if (inviteSession_ and inviteSession_->state != PJSIP_INV_STATE_EARLY) {
                earlyMediaRequested_ = false;
            }
        }
    }

    reportMediaNegotiationStatus();
}

void
SIPCall::reportMediaNegotiationStatus(const std::string& event)
{
    // Notify using the parent Id if it's a subcall.
    auto callId = isSubcall() ? parent_->getCallId() : getCallId();
    emitSignal<libsip_core::CallSignal::MediaNegotiationStatus>(callId, event, currentMediaList());
    auto previousState = isAudioOnly_;
    auto newState = !hasVideo();

    if (previousState != newState && Call::isRecording()) {
        deinitRecorder();
        toggleRecording();
        pendingRecord_ = true;
    }
    isAudioOnly_ = newState;

    if (pendingRecord_ && readyToRecord_) {
        toggleRecording();
    }
}

bool
SIPCall::checkMediaChangeRequest(const std::vector<libsip_core::MediaMap>& remoteMediaList)
{
    // The current media is considered to have changed if one of the
    // following condtions is true:
    //
    // - the number of media changed
    // - the type of one of the media changed (unlikely)
    // - one of the media was enabled/disabled

    SIP_CORE_DBG("[call:%s] Received a media change request", getCallId().c_str());

    auto remoteMediaAttrList = MediaAttribute::buildMediaAttributesList(remoteMediaList,
                                                                        isSrtpEnabled());
    if (remoteMediaAttrList.size() != rtpStreams_.size())
        return true;

    for (size_t i = 0; i < rtpStreams_.size(); i++) {
        if (remoteMediaAttrList[i].type_ != rtpStreams_[i].mediaAttribute_->type_)
            return true;
        if (remoteMediaAttrList[i].enabled_ != rtpStreams_[i].mediaAttribute_->enabled_)
            return true;
    }

    return false;
}

void
SIPCall::handleMediaChangeRequest(const std::vector<libsip_core::MediaMap>& remoteMediaList)
{
    SIP_CORE_DBG("[call:%s] Handling media change request", getCallId().c_str());

    auto account = getAccount().lock();
    if (not account) {
        SIP_CORE_ERR("No account detected");
        return;
    }

    // If the offered media does not differ from the current local media, the
    // request is answered using the current local media.
    if (not checkMediaChangeRequest(remoteMediaList)) {
        answerMediaChangeRequest(
            MediaAttribute::mediaAttributesToMediaMaps(getMediaAttributeList()));
        return;
    }

    if (account->isAutoAnswerEnabled()) {
        // NOTE:
        // Since the auto-answer is enabled in the account, newly
        // added media are accepted too.
        // This also means that if original call was an audio-only call,
        // the local camera will be enabled, unless the video is disabled
        // in the account settings.

        std::vector<libsip_core::MediaMap> newMediaList;
        newMediaList.reserve(remoteMediaList.size());
        for (auto const& stream : rtpStreams_) {
            newMediaList.emplace_back(MediaAttribute::toMediaMap(*stream.mediaAttribute_));
        }

        assert(remoteMediaList.size() > 0);
        if (remoteMediaList.size() > newMediaList.size()) {
            for (auto idx = newMediaList.size(); idx < remoteMediaList.size(); idx++) {
                newMediaList.emplace_back(remoteMediaList[idx]);
            }
        }
        answerMediaChangeRequest(newMediaList, true);
        return;
    }

    // Report the media change request.
    emitSignal<libsip_core::CallSignal::MediaChangeRequested>(getAccountId(),
                                                              getCallId(),
                                                              remoteMediaList);
}

pj_status_t
SIPCall::onReceiveReinvite(const pjmedia_sdp_session* offer, pjsip_rx_data* rdata)
{
    SIP_CORE_DBG("[call:%s] Received a re-invite", getCallId().c_str());

    pj_status_t res = PJ_SUCCESS;

    if (not sdp_) {
        SIP_CORE_ERR("SDP session is invalid");
        return res;
    }

    sdp_->setActiveRemoteSdpSession(nullptr);
    sdp_->setActiveLocalSdpSession(nullptr);

    auto acc = getSIPAccount();
    if (not acc) {
        SIP_CORE_ERR("No account detected");
        return res;
    }

    Sdp::printSession(offer, "Remote session (media change request)", SdpDirection::OFFER);

    sdp_->setReceivedOffer(offer);

    // Note: For multistream, here we must ignore disabled remote medias, because
    // we will answer from our medias and remote enabled medias.
    // Example: if remote disables its camera and share its screen, the offer will
    // have an active and a disabled media (with port = 0).
    // In this case, if we have only one video, we can just negotiate 1 video instead of 2
    // with 1 disabled.
    // cf. pjmedia_sdp_neg_modify_local_offer2 for more details.
    auto const& mediaAttrList = Sdp::getMediaAttributeListFromSdp(offer, true);
    if (mediaAttrList.empty()) {
        SIP_CORE_WARN("[call:%s] Media list is empty, ignoring", getCallId().c_str());
        return res;
    }

    pjsip_tx_data* tdata = nullptr;
    if (pjsip_inv_initial_answer(inviteSession_.get(), rdata, PJSIP_SC_TRYING, NULL, NULL, &tdata)
        != PJ_SUCCESS) {
        SIP_CORE_ERR("[call:%s] Could not create answer TRYING", getCallId().c_str());
        return res;
    }

    // Report the change request.
    auto const& remoteMediaList = MediaAttribute::mediaAttributesToMediaMaps(mediaAttrList);

    if (auto conf = getConference()) {
        conf->handleMediaChangeRequest(shared_from_this(), remoteMediaList);
    } else {
        handleMediaChangeRequest(remoteMediaList);
    }
    return res;
}

void
SIPCall::onReceiveOfferIn200OK(const pjmedia_sdp_session* offer)
{
    if (not rtpStreams_.empty()) {
        SIP_CORE_ERR("[call:%s] Unexpected offer in '200 OK' answer", getCallId().c_str());
        return;
    }

    auto acc = getSIPAccount();
    if (not acc) {
        SIP_CORE_ERR("No account detected");
        return;
    }

    if (not sdp_) {
        SIP_CORE_ERR("invalid SDP session");
        return;
    }

    SIP_CORE_DBG("[call:%s] Received an offer in '200 OK' answer", getCallId().c_str());

    auto mediaList = Sdp::getMediaAttributeListFromSdp(offer, false, false);
    // If this method is called, it means we are expecting an offer
    // in the 200OK answer.
    if (mediaList.empty()) {
        SIP_CORE_WARN("[call:%s] Remote media list is empty, ignoring", getCallId().c_str());
        return;
    }

    Sdp::printSession(offer, "Remote session (offer in 200 OK answer)", SdpDirection::OFFER);

    sdp_->setActiveRemoteSdpSession(nullptr);
    sdp_->setActiveLocalSdpSession(nullptr);

    sdp_->setReceivedOffer(offer);

    // If we send an empty offer, video will be accepted only if locally
    // enabled by the user.
    for (auto& mediaAttr : mediaList) {
        if (mediaAttr.type_ == MediaType::MEDIA_VIDEO and not acc->isVideoEnabled()) {
            mediaAttr.enabled_ = false;
        }
    }

    initMediaStreams(mediaList);

    if (!prepareLocalMediaReservations(mediaList)) {
        return;
    }

    if (!sdp_->processIncomingOffer(mediaList)) {
        clearPendingLocalReservations();
        SIP_CORE_ERR("[call:%s] Could not prepare local SDP answer for offer in 200 OK",
                     getCallId().c_str());
        return;
    }

    if (!sdp_->startNegotiation()) {
        clearPendingLocalReservations();
        SIP_CORE_ERR("[call:%s] Could not start media negotiation for offer in 200 OK",
                     getCallId().c_str());
        return;
    }

    if (pjsip_inv_set_sdp_answer(inviteSession_.get(), sdp_->getLocalSdpSession()) != PJ_SUCCESS) {
        clearPendingLocalReservations();
        SIP_CORE_ERR("[call:%s] Could not start media negotiation for a re-invite request",
                     getCallId().c_str());
    }
}

void
SIPCall::onTextMessage(std::map<std::string, std::string>&& messages)
{
    // call base class
    Call::onTextMessage(std::move(messages));
}

std::map<std::string, std::string>
SIPCall::getDetails() const
{
    auto acc = getSIPAccount();
    if (!acc) {
        SIP_CORE_ERR("No account detected");
        return {};
    }

    auto details = Call::getDetails();

    details.emplace(libsip_core::Call::Details::PEER_HOLDING, peerHolding_ ? TRUE_STR : FALSE_STR);
    details.emplace(libsip_core::Call::Details::INVITE_CALL_ID, inviteCallId_);

    for (auto const& stream : rtpStreams_) {
        if (stream.mediaAttribute_->type_ == MediaType::MEDIA_VIDEO) {
            details.emplace(libsip_core::Call::Details::VIDEO_SOURCE,
                            stream.mediaAttribute_->sourceUri_);
#ifdef ENABLE_VIDEO
            if (auto const& rtpSession = stream.rtpSession_) {
                if (auto codec = rtpSession->getCodec()) {
                    details.emplace(libsip_core::Call::Details::VIDEO_CODEC,
                                    codec->systemCodecInfo.name);
                    details.emplace(libsip_core::Call::Details::VIDEO_MIN_BITRATE,
                                    std::to_string(codec->systemCodecInfo.minBitrate));
                    details.emplace(libsip_core::Call::Details::VIDEO_MAX_BITRATE,
                                    std::to_string(codec->systemCodecInfo.maxBitrate));
                    if (const auto& curvideoRtpSession
                        = std::static_pointer_cast<video::VideoRtpSession>(rtpSession)) {
                        details.emplace(libsip_core::Call::Details::VIDEO_BITRATE,
                                        std::to_string(curvideoRtpSession->getVideoBitrateInfo()
                                                           .videoBitrateCurrent));

                        if (auto remote = curvideoRtpSession->getVideoReceive()) {
                            details.emplace(libsip_core::Call::Details::VIDEO_FPS,
                                            std::to_string(remote->getInfo().frameRate.real()));
                        }
                    }
                } else
                    details.emplace(libsip_core::Call::Details::VIDEO_CODEC, "");

                auto rrParams = rtpSession->getRtcpRR();
                details.emplace(libsip_core::Call::Details::VIDEO_FRACTION_LOST,
                                std::to_string(rrParams.fraction_lost));
                details.emplace(libsip_core::Call::Details::VIDEO_CUM_LOST_PACKET,
                                std::to_string(rrParams.cum_lost_packet));
                details.emplace(libsip_core::Call::Details::VIDEO_JITTER,
                                std::to_string(rrParams.jitter));
                details.emplace(libsip_core::Call::Details::VIDEO_EXT_HIGH,
                                std::to_string(rrParams.ext_high));
                details.emplace(libsip_core::Call::Details::VIDEO_LSR, std::to_string(rrParams.lsr));
                details.emplace(libsip_core::Call::Details::VIDEO_DLSR,
                                std::to_string(rrParams.dlsr));

                auto srParams = rtpSession->getRtcpSR();
                details.emplace(libsip_core::Call::Details::VIDEO_SPC, std::to_string(srParams.spc));
                details.emplace(libsip_core::Call::Details::VIDEO_SOC, std::to_string(srParams.soc));
                details.emplace(libsip_core::Call::Details::VIDEO_TIMESTAMP_MSB,
                                std::to_string(srParams.timestampMSB));
                details.emplace(libsip_core::Call::Details::VIDEO_TIMESTAMP_LSB,
                                std::to_string(srParams.timestampLSB));
                details.emplace(libsip_core::Call::Details::VIDEO_TIMESTAMP_RTP,
                                std::to_string(srParams.timestampRTP));

                auto rembParams = rtpSession->getRtcpREMB();
                details.emplace(libsip_core::Call::Details::VIDEO_BR_EXP,
                                std::to_string(rembParams.br_exp));
                details.emplace(libsip_core::Call::Details::VIDEO_BR_MANTIS,
                                std::to_string(rembParams.br_mantis));
            }
#endif
        } else if (stream.mediaAttribute_->type_ == MediaType::MEDIA_AUDIO) {
            if (auto const& rtpSession = stream.rtpSession_) {
                if (auto codec = rtpSession->getCodec()) {
                    details.emplace(libsip_core::Call::Details::AUDIO_CODEC,
                                    codec->systemCodecInfo.name);
                    const auto* codecInfo = static_cast<const SystemAudioCodecInfo*>(
                        &codec->systemCodecInfo);
                    details
                        .emplace(libsip_core::Call::Details::AUDIO_SAMPLE_RATE,
                                 codecInfo->getCodecSpecifications()
                                     [libsip_core::Account::ConfProperties::CodecInfo::SAMPLE_RATE]);
                } else {
                    details.emplace(libsip_core::Call::Details::AUDIO_CODEC, "");
                    details.emplace(libsip_core::Call::Details::AUDIO_SAMPLE_RATE, "");
                }

                auto rrParams = rtpSession->getRtcpRR();
                details.emplace(libsip_core::Call::Details::AUDIO_FRACTION_LOST,
                                std::to_string(rrParams.fraction_lost));
                details.emplace(libsip_core::Call::Details::AUDIO_CUM_LOST_PACKET,
                                std::to_string(rrParams.cum_lost_packet));
                details.emplace(libsip_core::Call::Details::AUDIO_JITTER,
                                std::to_string(rrParams.jitter));
                details.emplace(libsip_core::Call::Details::AUDIO_EXT_HIGH,
                                std::to_string(rrParams.ext_high));
                details.emplace(libsip_core::Call::Details::AUDIO_LSR, std::to_string(rrParams.lsr));
                details.emplace(libsip_core::Call::Details::AUDIO_DLSR,
                                std::to_string(rrParams.dlsr));

                auto srParams = rtpSession->getRtcpSR();
                details.emplace(libsip_core::Call::Details::AUDIO_SPC, std::to_string(srParams.spc));
                details.emplace(libsip_core::Call::Details::AUDIO_SOC, std::to_string(srParams.soc));
                details.emplace(libsip_core::Call::Details::AUDIO_TIMESTAMP_MSB,
                                std::to_string(srParams.timestampMSB));
                details.emplace(libsip_core::Call::Details::AUDIO_TIMESTAMP_LSB,
                                std::to_string(srParams.timestampLSB));
                details.emplace(libsip_core::Call::Details::AUDIO_TIMESTAMP_RTP,
                                std::to_string(srParams.timestampRTP));

                auto rembParams = rtpSession->getRtcpREMB();
                details.emplace(libsip_core::Call::Details::AUDIO_BR_EXP,
                                std::to_string(rembParams.br_exp));
                details.emplace(libsip_core::Call::Details::AUDIO_BR_MANTIS,
                                std::to_string(rembParams.br_mantis));
            }
        }
    }

    return details;
}

void
SIPCall::enterConference(std::shared_ptr<Conference> conference)
{
    SIP_CORE_DBG("[call:%s] Entering conference [%s]",
                 getCallId().c_str(),
                 conference->getConfId().c_str());
    conf_ = conference;

#ifdef ENABLE_VIDEO
    if (conference->isVideoEnabled())
        for (const auto& videoRtp : getRtpSessionList(MediaType::MEDIA_VIDEO))
            std::static_pointer_cast<video::VideoRtpSession>(videoRtp)->enterConference(*conference);
#endif
}

void
SIPCall::exitConference()
{
    std::lock_guard<std::recursive_mutex> lk {callMutex_};
    SIP_CORE_DBG("[call:%s] Leaving conference", getCallId().c_str());

    auto const hasAudio = !getRtpSessionList(MediaType::MEDIA_AUDIO).empty();
    if (hasAudio && !isCaptureDeviceMuted(MediaType::MEDIA_AUDIO)) {
        auto& rbPool = Manager::instance().getRingBufferPool();
        rbPool.bindCallID(getCallId(), RingBufferPool::DEFAULT_ID);
        rbPool.flush(RingBufferPool::DEFAULT_ID);
        detachAudioFromConference();
    }
#ifdef ENABLE_VIDEO
    for (const auto& videoRtp : getRtpSessionList(MediaType::MEDIA_VIDEO))
        std::static_pointer_cast<video::VideoRtpSession>(videoRtp)->exitConference();
#endif
    conf_.reset();
}

void
SIPCall::setActiveMediaStream(const std::string& accountUri,
                              const std::string& deviceId,
                              const std::string& streamId,
                              const bool& state)
{
    auto remoteStreamId = streamId;
#ifdef ENABLE_VIDEO
    {
        std::lock_guard<std::mutex> lk(sinksMtx_);
        const auto& localIt = local2RemoteSinks_.find(streamId);
        if (localIt != local2RemoteSinks_.end()) {
            remoteStreamId = localIt->second;
        }
    }
#endif

    if (Call::conferenceProtocolVersion() == 1) {
        Json::Value sinkVal;
        sinkVal["active"] = state;
        Json::Value mediasObj;
        mediasObj[remoteStreamId] = sinkVal;
        Json::Value deviceVal;
        deviceVal["medias"] = mediasObj;
        Json::Value deviceObj;
        deviceObj[deviceId] = deviceVal;
        Json::Value accountVal;
        deviceVal["devices"] = deviceObj;
        Json::Value root;
        root[accountUri] = deviceVal;
        root["version"] = 1;
        Call::sendConfOrder(root);
    } else if (Call::conferenceProtocolVersion() == 0) {
        Json::Value root;
        root["activeParticipant"] = accountUri;
        Call::sendConfOrder(root);
    }
}

#ifdef ENABLE_VIDEO

void
SIPCall::setRotation(int streamIdx, int rotation)
{
    // Retrigger on another thread to avoid to lock pjsip
    // dht::ThreadPool::io().run([w = weak(), streamIdx, rotation] {
    //     if (auto shared = w.lock()) {
    //         std::lock_guard<std::recursive_mutex> lk {shared->callMutex_};
    //         shared->rotation_ = rotation;
    //         if (streamIdx == -1) {
    //             for (const auto& videoRtp : shared->getRtpSessionList(MediaType::MEDIA_VIDEO))
    //                 std::static_pointer_cast<video::VideoRtpSession>(videoRtp)->setRotation(rotation);
    //         } else if (streamIdx > -1 && streamIdx < static_cast<int>(shared->rtpStreams_.size())) {
    //             // Apply request for wanted stream
    //             auto& stream = shared->rtpStreams_[streamIdx];
    //             if (stream.rtpSession_ && stream.rtpSession_->getMediaType() == MediaType::MEDIA_VIDEO)
    //                 std::static_pointer_cast<video::VideoRtpSession>(stream.rtpSession_)
    //                     ->setRotation(rotation);
    //         }
    //     }
    // });
}

// the main idea is to create sinks from receiving video of a call!
void
SIPCall::createSinks(ConfInfo& infos)
{
    std::lock_guard<std::recursive_mutex> lk(callMutex_);
    std::lock_guard<std::mutex> lkS(sinksMtx_);
    if (!hasVideo())
        return;

    // find ourself in patricipant list and take all sizes from local video
    for (auto& participant : infos) {
        if (string_remove_suffix(participant.uri, '@') == account_.lock()->getUsername()
            && participant.device
                   == Manager::instance()
                          .getVideoManager()
                          .videoDeviceMonitor.getMRLForDefaultDevice()) {
            for (auto iter = rtpStreams_.begin(); iter != rtpStreams_.end(); iter++) {
                if (!iter->mediaAttribute_
                    || iter->mediaAttribute_->type_ == MediaType::MEDIA_AUDIO) {
                    continue;
                }
                auto localVideo = std::static_pointer_cast<video::VideoRtpSession>(iter->rtpSession_)
                                      ->getVideoLocal()
                                      .get();
                auto size = std::make_pair(10, 10);
                if (localVideo) {
                    size = std::make_pair(localVideo->getWidth(), localVideo->getHeight());
                }
                const auto& mediaAttribute = iter->mediaAttribute_;
                if (participant.sinkId.find(mediaAttribute->label_) != std::string::npos) {
                    local2RemoteSinks_[mediaAttribute->sourceUri_] = participant.sinkId;
                    participant.sinkId = mediaAttribute->sourceUri_;
                    participant.videoMuted = mediaAttribute->muted_;
                    participant.w = size.first;
                    participant.h = size.second;
                    participant.x = 0;
                    participant.y = 0;
                }
            }
        }
    }

    // find VideoReceiveThead, get it't sink and create "child" sinks for it (one sink - one participant)
    std::vector<std::shared_ptr<video::VideoFrameActiveWriter>> sinks;
    for (const auto& videoRtp : getRtpSessionList(MediaType::MEDIA_VIDEO)) {
        auto& videoReceive = std::static_pointer_cast<video::VideoRtpSession>(videoRtp)
                                 ->getVideoReceive();
        if (!videoReceive)
            continue;
        sinks.emplace_back(
            std::static_pointer_cast<video::VideoFrameActiveWriter>(videoReceive->getSink()));
    }
    auto conf = conf_.lock();
    const auto& id = conf ? conf->getConfId() : getCallId();
    Manager::instance().createSinkClients(id, infos, sinks, callSinksMap_);
}

#endif

std::vector<std::shared_ptr<RtpSession>>
SIPCall::getRtpSessionList(MediaType type) const
{
    std::vector<std::shared_ptr<RtpSession>> rtpList;
    rtpList.reserve(rtpStreams_.size());
    for (auto const& stream : rtpStreams_) {
        if (type == MediaType::MEDIA_ALL || stream.rtpSession_->getMediaType() == type)
            rtpList.emplace_back(stream.rtpSession_);
    }
    return rtpList;
}

void
SIPCall::monitor() const
{
    if (isSubcall())
        return;
    auto acc = getSIPAccount();
    if (!acc) {
        SIP_CORE_ERR("No account detected");
        return;
    }
    SIP_CORE_DBG("- Call %s with %s:", getCallId().c_str(), getPeerNumber().c_str());
    for (const auto& stream : rtpStreams_)
        SIP_CORE_DBG("\t- Media: %s", stream.mediaAttribute_->toString(true).c_str());
#ifdef ENABLE_VIDEO
    if (auto codec = getVideoCodec())
        SIP_CORE_DBG("\t- Video codec: %s", codec->systemCodecInfo.name.c_str());
#endif
}

bool
SIPCall::toggleRecording()
{
    pendingRecord_ = true;
    if (not readyToRecord_)
        return true;

    // add streams to recorder before starting the record
    if (not Call::isRecording()) {
        auto account = getSIPAccount();
        if (!account) {
            SIP_CORE_ERR("No account detected");
            return false;
        }
        auto title = fmt::format("Conversation at %TIMESTAMP between {} and {}",
                                 account->getUserUri(),
                                 peerUri_);
        recorder_->setMetadata(title, ""); // use default description

        std::time_t t = std::time(nullptr);
        auto recTime = std::localtime(&t);
        char time[20];
        strftime(time, 20, "%Y-%m-%d %H-%M-%S", recTime);
        auto filename = fmt::format("{} {} call from {} to {} [id {}]",
                                    time,
                                    getCallType() == CallType::INCOMING ? "Incoming" : "Outgoing",
                                    getCallType() == CallType::INCOMING
                                        ? sip_utils::stripSipUriPrefix(getPeerNumber())
                                        : getSIPAccount()->getUsername(),
                                    getCallType() == CallType::INCOMING
                                        ? getSIPAccount()->getUsername()
                                        : sip_utils::stripSipUriPrefix(getPeerNumber()),
                                    getCallId());
        SIP_CORE_INFO() << "Recording call to filename -> " << filename;
        setRecordingFilename(filename);
        for (const auto& rtpSession : getRtpSessionList())
            rtpSession->initRecorder();
    } else {
        updateRecState(false);
    }
    pendingRecord_ = false;
    auto state = Call::toggleRecording();
    if (state)
        updateRecState(state);
    return state;
}

void
SIPCall::deinitRecorder()
{
    for (const auto& rtpSession : getRtpSessionList())
        rtpSession->deinitRecorder();
}

void
SIPCall::InvSessionDeleter::operator()(pjsip_inv_session* inv) const noexcept
{
    // prevent this from getting accessed in callbacks
    // SIP_CORE_WARN: this is not thread-safe!
    if (!inv)
        return;
    inv->mod_data[Manager::instance().sipVoIPLink().getModId()] = nullptr;
    // NOTE: the counter is incremented by sipvoiplink (transaction_request_cb)
    pjsip_inv_dec_ref(inv);
}

void
SIPCall::merge(Call& call)
{
    SIP_CORE_DBG("[call:%s] merge subcall %s", getCallId().c_str(), call.getCallId().c_str());

    // This static cast is safe as this method is private and overload Call::merge
    auto& subcall = static_cast<SIPCall&>(call);

    std::lock(callMutex_, subcall.callMutex_);
    std::lock_guard<std::recursive_mutex> lk1 {callMutex_, std::adopt_lock};
    std::lock_guard<std::recursive_mutex> lk2 {subcall.callMutex_, std::adopt_lock};
    inviteSession_ = std::move(subcall.inviteSession_);
    if (inviteSession_)
        inviteSession_->mod_data[Manager::instance().sipVoIPLink().getModId()] = this;
    setSipTransport(std::move(subcall.sipTransport_), std::move(subcall.contactHeader_));
    sdp_ = std::move(subcall.sdp_);
    peerHolding_ = subcall.peerHolding_;
    localAudioPort_ = subcall.localAudioPort_;
    localVideoPort_ = subcall.localVideoPort_;
    pendingAudioSocketPair_ = std::move(subcall.pendingAudioSocketPair_);
#ifdef ENABLE_VIDEO
    pendingVideoSocketPair_ = std::move(subcall.pendingVideoSocketPair_);
#endif
    peerUserAgent_ = subcall.peerUserAgent_;
    peerAllowedMethods_ = subcall.peerAllowedMethods_;
    Call::merge(subcall);
}

void
SIPCall::rtpSetupSuccess()
{
    std::lock_guard<std::mutex> lk {setupSuccessMutex_};

    readyToRecord_ = true; // We're ready to record whenever a stream is ready

    auto previousState = isAudioOnly_;
    auto newState = !hasVideo();

    if (previousState != newState && Call::isRecording()) {
        deinitRecorder();
        toggleRecording();
        pendingRecord_ = true;
    }
    isAudioOnly_ = newState;

    if (pendingRecord_ && readyToRecord_)
        toggleRecording();
}

void
SIPCall::peerRecording(bool state)
{
    auto conference = conf_.lock();
    const std::string& id = conference ? conference->getConfId() : getCallId();
    if (state) {
        SIP_CORE_WARN("[call:%s] Peer is recording", getCallId().c_str());
        emitSignal<libsip_core::CallSignal::RemoteRecordingChanged>(id, getPeerNumber(), true);
    } else {
        SIP_CORE_WARN("Peer stopped recording");
        emitSignal<libsip_core::CallSignal::RemoteRecordingChanged>(id, getPeerNumber(), false);
    }
    peerRecording_ = state;
    if (auto conf = conf_.lock())
        conf->updateRecording();
}

void
SIPCall::peerMuted(bool muted, int streamIdx)
{
    if (muted) {
        SIP_CORE_WARN("Peer muted");
    } else {
        SIP_CORE_WARN("Peer un-muted");
    }

    if (streamIdx == -1) {
        for (const auto& audioRtp : getRtpSessionList(MediaType::MEDIA_AUDIO))
            audioRtp->setMuted(muted, RtpSession::Direction::RECV);
    } else if (streamIdx > -1 && streamIdx < static_cast<int>(rtpStreams_.size())) {
        auto& stream = rtpStreams_[streamIdx];
        if (stream.rtpSession_ && stream.rtpSession_->getMediaType() == MediaType::MEDIA_AUDIO)
            stream.rtpSession_->setMuted(muted, RtpSession::Direction::RECV);
    }

    peerMuted_ = muted;
    if (auto conf = conf_.lock())
        conf->updateMuted();

    emitSignal<libsip_core::CallSignal::PeerMuted>(getCallId(), peerMuted_);
}

void
SIPCall::peerVoice(bool voice)
{
    peerVoice_ = voice;

    if (auto conference = conf_.lock()) {
        conference->setVoiceActivityForCall(getCallId(), voice);
    } else {
        // one-to-one call

        {
            std::lock_guard<std::mutex> lk(confInfoMutex_);
            // confID_ empty -> participant set confInfo with the received one
            auto participant = std::find_if(confInfo_.begin(),
                                            confInfo_.end(),
                                            [&](const ParticipantInfo& p) {
                                                return p.uri == this->getCallId();
                                            });

            if (participant != confInfo_.end()) {
                participant->voiceActivity = voice;
            }
        }

        // maybe emit signal with partner voice activity
    }
}

} // namespace sip_core
