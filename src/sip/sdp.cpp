/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
 *  Author: Emmanuel Milou <emmanuel.milou@savoirfairelinux.com>
 *  Author: Alexandre Savard <alexandre.savard@savoirfairelinux.com>
 *  Author: Adrien Béraud <adrien.beraud@savoirfairelinux.com>
 *  Author: Eloi Bail <eloi.bail@savoirfairelinux.com>
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

#include "sdp.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "sip/sipvoiplink.h"
#include "string_utils.h"
#include "base64.h"

#include "manager.h"
#include "logger.h"
#include "libav_utils.h"

#include "media_codec.h"
#include "system_codec_container.h"
#include "compiler_intrinsics.h" // for UNUSED

#include <algorithm>
#include <cassert>

namespace sip_core {

using std::string;
using std::vector;
using std::stringstream;

static constexpr int POOL_INITIAL_SIZE = 16384;
static constexpr int POOL_INCREMENT_SIZE = POOL_INITIAL_SIZE;

static std::map<MediaDirection, const char*> DIRECTION_STR {{MediaDirection::SENDRECV, "sendrecv"},
                                                            {MediaDirection::SENDONLY, "sendonly"},
                                                            {MediaDirection::RECVONLY, "recvonly"},
                                                            {MediaDirection::INACTIVE, "inactive"},
                                                            {MediaDirection::UNKNOWN, "unknown"}};

Sdp::Sdp(const std::string& id)
    : memPool_(nullptr, [](pj_pool_t* pool) { pj_pool_release(pool); })
    , publishedIpAddr_()
    , publishedIpAddrType_()
    , telephoneEventPayload_(101) // same as asterisk
    , sessionName_("Call ID " + id)
{
    memPool_.reset(pj_pool_create(&Manager::instance().sipVoIPLink().getCachingPool()->factory,
                                  id.c_str(),
                                  POOL_INITIAL_SIZE,
                                  POOL_INCREMENT_SIZE,
                                  NULL));
    if (not memPool_)
        throw std::runtime_error("pj_pool_create() failed");
}

Sdp::~Sdp() {}

std::shared_ptr<AccountCodecInfo>
Sdp::findCodecBySpec(std::string_view codec, const unsigned clockrate) const
{
    // TODO : only manage a list?
    for (const auto& accountCodec : audio_codec_list_) {
        auto audioCodecInfo = std::static_pointer_cast<AccountAudioCodecInfo>(accountCodec);
        auto& sysCodecInfo = *static_cast<const SystemAudioCodecInfo*>(
            &audioCodecInfo->systemCodecInfo);
        if (sysCodecInfo.name == codec
            and (audioCodecInfo->isPCMG722() ? (clockrate == 8000)
                                             : (sysCodecInfo.audioformat.sample_rate == clockrate)))
            return accountCodec;
    }

    for (const auto& accountCodec : video_codec_list_) {
        auto sysCodecInfo = accountCodec->systemCodecInfo;
        if (sysCodecInfo.name == codec)
            return accountCodec;
    }
    return nullptr;
}

std::shared_ptr<AccountCodecInfo>
Sdp::findCodecByPayload(const unsigned payloadType)
{
    // TODO : only manage a list?
    for (const auto& accountCodec : audio_codec_list_) {
        auto& sysCodecInfo = accountCodec->systemCodecInfo;
        if (sysCodecInfo.payloadType == payloadType)
            return accountCodec;
    }

    for (const auto& accountCodec : video_codec_list_) {
        auto& sysCodecInfo = accountCodec->systemCodecInfo;
        if (sysCodecInfo.payloadType == payloadType)
            return accountCodec;
    }
    return nullptr;
}

void
Sdp::setActiveLocalSdpSession(const pjmedia_sdp_session* sdp)
{
    if (activeLocalSession_ != sdp)
        SIP_CORE_DBG("Set active local session to [%p]. Was [%p]", sdp, activeLocalSession_);
    activeLocalSession_ = sdp;
}

void
Sdp::setActiveRemoteSdpSession(const pjmedia_sdp_session* sdp)
{
    if (activeLocalSession_ != sdp)
        SIP_CORE_DBG("Set active remote session to [%p]. Was [%p]", sdp, activeRemoteSession_);
    activeRemoteSession_ = sdp;
}

pjmedia_sdp_attr*
Sdp::generateSdesAttribute()
{
    static constexpr const unsigned cryptoSuite = 0;
    std::vector<uint8_t> keyAndSalt;
    keyAndSalt.resize(sip_core::CryptoSuites[cryptoSuite].masterKeyLength / 8
                      + sip_core::CryptoSuites[cryptoSuite].masterSaltLength / 8);
    // generate keys
    // randomFill(keyAndSalt);

    std::string crypto_attr = "1 "s + sip_core::CryptoSuites[cryptoSuite].name
                              + " inline:" + base64::encode(keyAndSalt);
    pj_str_t val {sip_utils::CONST_PJ_STR(crypto_attr)};
    return pjmedia_sdp_attr_create(memPool_.get(), "crypto", &val);
}

char const*
Sdp::mediaDirection(const MediaAttribute& mediaAttr)
{
    if (not mediaAttr.enabled_) {
        return DIRECTION_STR[MediaDirection::INACTIVE];
    }

    if (mediaAttr.type_ == MediaType::MEDIA_AUDIO) {
        // ignore hold for video. It just can be stopped with sending black frames.
        // Audio will played to callee by the SIP server.
        if (mediaAttr.onHold_) {
            return DIRECTION_STR[MediaDirection::SENDONLY];
        }
    }

    // Since mute/un-mute audio is only done locally (RTP packets
    // are still sent to the peer), the media direction must be
    // set to "sendrecv" regardless of the mute state.
    return DIRECTION_STR[MediaDirection::SENDRECV];
}

MediaDirection
Sdp::getMediaDirection(pjmedia_sdp_media* media)
{
    if (pjmedia_sdp_attr_find2(media->attr_count,
                               media->attr,
                               DIRECTION_STR[MediaDirection::SENDRECV],
                               nullptr)
        != nullptr) {
        return MediaDirection::SENDRECV;
    }

    if (pjmedia_sdp_attr_find2(media->attr_count,
                               media->attr,
                               DIRECTION_STR[MediaDirection::SENDONLY],
                               nullptr)
        != nullptr) {
        return MediaDirection::SENDONLY;
    }

    if (pjmedia_sdp_attr_find2(media->attr_count,
                               media->attr,
                               DIRECTION_STR[MediaDirection::RECVONLY],
                               nullptr)
        != nullptr) {
        return MediaDirection::RECVONLY;
    }

    if (pjmedia_sdp_attr_find2(media->attr_count,
                               media->attr,
                               DIRECTION_STR[MediaDirection::INACTIVE],
                               nullptr)
        != nullptr) {
        return MediaDirection::INACTIVE;
    }

    return MediaDirection::UNKNOWN;
}

MediaTransport
Sdp::getMediaTransport(pjmedia_sdp_media* media)
{
    if (pj_stricmp2(&media->desc.transport, "RTP/SAVP") == 0)
        return MediaTransport::RTP_SAVP;
    else if (pj_stricmp2(&media->desc.transport, "RTP/AVP") == 0)
        return MediaTransport::RTP_AVP;

    return MediaTransport::UNKNOWN;
}

std::vector<std::string>
Sdp::getCrypto(pjmedia_sdp_media* media)
{
    std::vector<std::string> crypto;
    for (unsigned j = 0; j < media->attr_count; j++) {
        const auto attribute = media->attr[j];
        if (pj_stricmp2(&attribute->name, "crypto") == 0)
            crypto.emplace_back(attribute->value.ptr, attribute->value.slen);
    }

    return crypto;
}

pjmedia_sdp_media*
Sdp::addMediaDescription(const MediaAttribute& mediaAttr)
{
    auto type = mediaAttr.type_;
    auto secure = mediaAttr.secure_;

    SIP_CORE_DBG("Add media description [%s]", mediaAttr.toString(true).c_str());

    pjmedia_sdp_media* med = PJ_POOL_ZALLOC_T(memPool_.get(), pjmedia_sdp_media);

    switch (type) {
    case MediaType::MEDIA_AUDIO:
        med->desc.media = sip_utils::CONST_PJ_STR("audio");
        med->desc.port = mediaAttr.enabled_ ? localAudioRtpPort_ : 0;
        med->desc.fmt_count = audio_codec_list_.size();
        break;
    case MediaType::MEDIA_VIDEO:
        med->desc.media = sip_utils::CONST_PJ_STR("video");
        med->desc.port = mediaAttr.enabled_ ? localVideoRtpPort_ : 0;
        med->desc.fmt_count = video_codec_list_.size();
        break;
    default:
        throw SdpException("Unsupported media type! Only audio and video are supported");
        break;
    }

    med->desc.port_count = 1;

    // Set the transport protocol of the media
    med->desc.transport = secure ? sip_utils::CONST_PJ_STR("RTP/SAVP")
                                 : sip_utils::CONST_PJ_STR("RTP/AVP");

    unsigned dynamic_payload = 96;

    for (unsigned i = 0; i < med->desc.fmt_count; i++) {
        pjmedia_sdp_rtpmap rtpmap;
        rtpmap.param.slen = 0;

        std::string channels; // must have the lifetime of rtpmap
        std::string enc_name;
        unsigned payload;

        if (type == MediaType::MEDIA_AUDIO) {
            auto accountAudioCodec = std::static_pointer_cast<AccountAudioCodecInfo>(
                audio_codec_list_[i]);
            payload = accountAudioCodec->payloadType;
            enc_name = accountAudioCodec->systemCodecInfo.name;

            if (accountAudioCodec->audioformat.nb_channels > 1) {
                channels = std::to_string(accountAudioCodec->audioformat.nb_channels);
                rtpmap.param = sip_utils::CONST_PJ_STR(channels);
            }
            // G722 requires G722/8000 media description even though it's @ 16000 Hz
            // See http://tools.ietf.org/html/rfc3551#section-4.5.2
            // G729 also has fixed 8000 Hz rate
            if (accountAudioCodec->isPCMG722() || accountAudioCodec->isG729())
                rtpmap.clock_rate = 8000;
            else
                rtpmap.clock_rate = accountAudioCodec->audioformat.sample_rate;

        } else {
            // FIXME: get this key from header
            payload = dynamic_payload++;
            enc_name = video_codec_list_[i]->systemCodecInfo.name;
            rtpmap.clock_rate = 90000;
        }

        auto payloadStr = std::to_string(payload);
        auto pjPayload = sip_utils::CONST_PJ_STR(payloadStr);
        pj_strdup(memPool_.get(), &med->desc.fmt[i], &pjPayload);

        // Add a rtpmap field for each codec
        // We could add one only for dynamic payloads because the codecs with static RTP payloads
        // are entirely defined in the RFC 3351
        rtpmap.pt = med->desc.fmt[i];
        rtpmap.enc_name = sip_utils::CONST_PJ_STR(enc_name);

        pjmedia_sdp_attr* attr;
        pjmedia_sdp_rtpmap_to_attr(memPool_.get(), &rtpmap, &attr);
        med->attr[med->attr_count++] = attr;
        
        if (type == MediaType::MEDIA_AUDIO) {
            auto accountAudioCodec = std::static_pointer_cast<AccountAudioCodecInfo>(
                audio_codec_list_[i]);
            
            if (accountAudioCodec->isG729()) {
                // first try to negotiate with annexb enabled
                auto value = fmt::format("fmtp:{} annexb=yes", payload);
                med->attr[med->attr_count++] = pjmedia_sdp_attr_create(memPool_.get(),
                                                                        value.c_str(),
                                                                        NULL);
            }
        }
        
    

#ifdef ENABLE_VIDEO
        if (enc_name == "H264") {
            // FIXME: this should not be hardcoded, it will determine what profile and level
            // our peer will send us
            const auto accountVideoCodec = std::static_pointer_cast<AccountVideoCodecInfo>(
                video_codec_list_[i]);
            const auto& profileLevelID = accountVideoCodec->parameters.empty()
                                             ? libav_utils::DEFAULT_H264_PROFILE_LEVEL_ID
                                             : accountVideoCodec->parameters;
            auto value = fmt::format("fmtp:{} {}", payload, profileLevelID);
            med->attr[med->attr_count++] = pjmedia_sdp_attr_create(memPool_.get(),
                                                                   value.c_str(),
                                                                   NULL);
        }
#endif
    }

    if (type == MediaType::MEDIA_AUDIO) {
        setTelephoneEventRtpmap(med);
        if (localAudioRtcpPort_) {
            addRTCPAttribute(med, localAudioRtcpPort_);
        }
    } else if (type == MediaType::MEDIA_VIDEO and localVideoRtcpPort_) {
        addRTCPAttribute(med, localVideoRtcpPort_);
    }

    char const* direction = mediaDirection(mediaAttr);

    med->attr[med->attr_count++] = pjmedia_sdp_attr_create(memPool_.get(), direction, NULL);

    if (secure) {
        // if (pjmedia_sdp_media_add_attr(med, generateSdesAttribute()) != PJ_SUCCESS)
        //     throw SdpException("Could not add sdes attribute to media");
    }

    return med;
}

void
Sdp::addRTCPAttribute(pjmedia_sdp_media* med, uint16_t port)
{
    IpAddr addr {publishedIpAddr_};
    addr.setPort(port);
    pjmedia_sdp_attr* attr = pjmedia_sdp_attr_create_rtcp(memPool_.get(), addr.pjPtr());
    if (attr)
        pjmedia_sdp_attr_add(&med->attr_count, med->attr, attr);
}

void
Sdp::setPublishedIP(const std::string& addr, pj_uint16_t addr_type)
{
    publishedIpAddr_ = addr;
    publishedIpAddrType_ = addr_type;
    if (localSession_) {
        if (addr_type == pj_AF_INET6())
            localSession_->origin.addr_type = sip_utils::CONST_PJ_STR("IP6");
        else
            localSession_->origin.addr_type = sip_utils::CONST_PJ_STR("IP4");
        localSession_->origin.addr = sip_utils::CONST_PJ_STR(publishedIpAddr_);
        localSession_->conn->addr = localSession_->origin.addr;
        if (pjmedia_sdp_validate(localSession_) != PJ_SUCCESS)
            SIP_CORE_ERR("Could not validate SDP");
    }
}

void
Sdp::setPublishedIP(const IpAddr& ip_addr)
{
    setPublishedIP(ip_addr, ip_addr.getFamily());
}

void
Sdp::setTelephoneEventRtpmap(pjmedia_sdp_media* med)
{
    ++med->desc.fmt_count;
    pj_strdup2(memPool_.get(),
               &med->desc.fmt[med->desc.fmt_count - 1],
               std::to_string(telephoneEventPayload_).c_str());

    pjmedia_sdp_attr* attr_rtpmap = static_cast<pjmedia_sdp_attr*>(
        pj_pool_zalloc(memPool_.get(), sizeof(pjmedia_sdp_attr)));
    attr_rtpmap->name = sip_utils::CONST_PJ_STR("rtpmap");
    attr_rtpmap->value = sip_utils::CONST_PJ_STR("101 telephone-event/8000");

    med->attr[med->attr_count++] = attr_rtpmap;

    pjmedia_sdp_attr* attr_fmtp = static_cast<pjmedia_sdp_attr*>(
        pj_pool_zalloc(memPool_.get(), sizeof(pjmedia_sdp_attr)));
    attr_fmtp->name = sip_utils::CONST_PJ_STR("fmtp");
    attr_fmtp->value = sip_utils::CONST_PJ_STR("101 0-15");

    med->attr[med->attr_count++] = attr_fmtp;
}

void
Sdp::setLocalMediaCapabilities(MediaType type,
                               const std::vector<std::shared_ptr<AccountCodecInfo>>& selectedCodecs)
{
    switch (type) {
    case MediaType::MEDIA_AUDIO:
        audio_codec_list_ = selectedCodecs;
        break;

    case MediaType::MEDIA_VIDEO:
#ifdef ENABLE_VIDEO
        video_codec_list_ = selectedCodecs;
        // Do not expose H265 if accel is disactivated
        if (not sip_core::Manager::instance().videoPreferences.getEncodingAccelerated()) {
            video_codec_list_.erase(std::remove_if(video_codec_list_.begin(),
                                                   video_codec_list_.end(),
                                                   [](const std::shared_ptr<AccountCodecInfo>& i) {
                                                       return i->systemCodecInfo.name == "H265";
                                                   }),
                                    video_codec_list_.end());
        }
#else
        (void) selectedCodecs;
#endif
        break;

    default:
        throw SdpException("Unsupported media type");
        break;
    }
}

const char*
Sdp::getSdpDirectionStr(SdpDirection direction)
{
    if (direction == SdpDirection::OFFER)
        return "OFFER";
    if (direction == SdpDirection::ANSWER)
        return "ANSWER";
    return "NONE";
}

void
Sdp::printSession(const pjmedia_sdp_session* session, const char* header, SdpDirection direction)
{
    static constexpr size_t BUF_SZ = 4095;
    std::unique_ptr<pj_pool_t, decltype(&pj_pool_release)>
        tmpPool_(pj_pool_create(&Manager::instance().sipVoIPLink().getCachingPool()->factory,
                                "printSdp",
                                BUF_SZ,
                                BUF_SZ,
                                nullptr),
                 &pj_pool_release);

    auto cloned_session = pjmedia_sdp_session_clone(tmpPool_.get(), session);
    if (!cloned_session) {
        SIP_CORE_ERR("Could not clone SDP for printing");
        return;
    }

    // Filter-out sensible data like SRTP master key.
    for (unsigned i = 0; i < cloned_session->media_count; ++i) {
        pjmedia_sdp_media_remove_all_attr(cloned_session->media[i], "crypto");
    }

    std::array<char, BUF_SZ + 1> buffer;
    auto size = pjmedia_sdp_print(cloned_session, buffer.data(), BUF_SZ);
    if (size < 0) {
        SIP_CORE_ERR("%s SDP too big for dump", header);
        return;
    }

    SIP_CORE_DBG("[SDP %s] %s\n%.*s", getSdpDirectionStr(direction), header, size, buffer.data());
}

void
Sdp::createLocalSession(SdpDirection direction)
{
    sdpDirection_ = direction;
    localSession_ = PJ_POOL_ZALLOC_T(memPool_.get(), pjmedia_sdp_session);
    localSession_->conn = PJ_POOL_ZALLOC_T(memPool_.get(), pjmedia_sdp_conn);

    /* Initialize the fields of the struct */
    localSession_->origin.version = 0;
    pj_time_val tv;
    pj_gettimeofday(&tv);

    localSession_->origin.user = *pj_gethostname();

    // Use Network Time Protocol format timestamp to ensure uniqueness.
    localSession_->origin.id = tv.sec + 2208988800UL;
    localSession_->origin.net_type = sip_utils::CONST_PJ_STR("IN");
    if (publishedIpAddrType_ == pj_AF_INET6())
        localSession_->origin.addr_type = sip_utils::CONST_PJ_STR("IP6");
    else
        localSession_->origin.addr_type = sip_utils::CONST_PJ_STR("IP4");
    localSession_->origin.addr = sip_utils::CONST_PJ_STR(publishedIpAddr_);

    // Use the call IDs for s= line
    localSession_->name = sip_utils::CONST_PJ_STR(sessionName_);

    localSession_->conn->net_type = localSession_->origin.net_type;
    localSession_->conn->addr_type = localSession_->origin.addr_type;
    localSession_->conn->addr = localSession_->origin.addr;

    // RFC 3264: An offer/answer model session description protocol
    // As the session is created and destroyed through an external signaling mean (SIP), the line
    // should have a value of "0 0".
    localSession_->time.start = 0;
    localSession_->time.stop = 0;
}

int
Sdp::validateSession() const
{
    return pjmedia_sdp_validate(localSession_);
}

bool
Sdp::createOffer(const std::vector<MediaAttribute>& mediaList)
{
    if (mediaList.size() >= PJMEDIA_MAX_SDP_MEDIA) {
        throw SdpException("Media list size exceeds SDP media maximum size");
    }
    SIP_CORE_DEBUG("Creating SDP offer with {} media", mediaList.size());

    createLocalSession(SdpDirection::OFFER);

    if (validateSession() != PJ_SUCCESS) {
        SIP_CORE_ERR("Failed to create initial offer");
        return false;
    }

    localSession_->media_count = 0;

    for (auto const& media : mediaList) {
        if (media.enabled_) {
            localSession_->media[localSession_->media_count++] = addMediaDescription(media);
        }
    }

    if (validateSession() != PJ_SUCCESS) {
        SIP_CORE_ERR("Failed to add medias");
        return false;
    }

    if (pjmedia_sdp_neg_create_w_local_offer(memPool_.get(), localSession_, &negotiator_)
        != PJ_SUCCESS) {
        SIP_CORE_ERR("Failed to create an initial SDP negotiator");
        return false;
    }

    printSession(localSession_, "Local session (initial):", sdpDirection_);

    return true;
}

void
Sdp::setReceivedOffer(const pjmedia_sdp_session* remote)
{
    if (remote == nullptr) {
        SIP_CORE_ERR("Remote session is NULL");
        return;
    }
    remoteSession_ = pjmedia_sdp_session_clone(memPool_.get(), remote);
}

bool
Sdp::processIncomingOffer(const std::vector<MediaAttribute>& mediaList)
{
    if (not remoteSession_)
        return false;

    SIP_CORE_DEBUG("Processing received offer for [{:s}] with {:d} media",
                   sessionName_,
                   mediaList.size());

    printSession(remoteSession_, "Remote session:", SdpDirection::OFFER);

    createLocalSession(SdpDirection::ANSWER);
    if (validateSession() != PJ_SUCCESS) {
        SIP_CORE_ERR("Failed to create local session");
        return false;
    }

    localSession_->media_count = 0;

    for (auto const& media : mediaList) {
        if (media.enabled_) {
            localSession_->media[localSession_->media_count++] = addMediaDescription(media);
        }
    }

    printSession(localSession_, "Local session:\n", sdpDirection_);

    if (validateSession() != PJ_SUCCESS) {
        SIP_CORE_ERR("Failed to add medias");
        return false;
    }

    if (pjmedia_sdp_neg_create_w_remote_offer(memPool_.get(),
                                              localSession_,
                                              remoteSession_,
                                              &negotiator_)
        != PJ_SUCCESS) {
        SIP_CORE_ERR("Failed to initialize media negotiation");
        return false;
    }

    return true;
}

bool
Sdp::startNegotiation()
{
    SIP_CORE_DBG("Starting media negotiation for [%s]", sessionName_.c_str());

    if (negotiator_ == NULL) {
        SIP_CORE_ERR("Can't start negotiation with invalid negotiator");
        return false;
    }

    const pjmedia_sdp_session* active_local;
    const pjmedia_sdp_session* active_remote;

    if (pjmedia_sdp_neg_get_state(negotiator_) != PJMEDIA_SDP_NEG_STATE_WAIT_NEGO) {
        SIP_CORE_WARN("Negotiator not in right state for negotiation");
        return false;
    }

    if (pjmedia_sdp_neg_negotiate(memPool_.get(), negotiator_, 0) != PJ_SUCCESS) {
        SIP_CORE_ERR("Failed to start media negotiation");
        return false;
    }

    if (pjmedia_sdp_neg_get_active_local(negotiator_, &active_local) != PJ_SUCCESS)
        SIP_CORE_ERR("Could not retrieve local active session");

    setActiveLocalSdpSession(active_local);

    if (active_local != nullptr) {
        printSession(active_local, "Local active session:", sdpDirection_);
    }

    if (pjmedia_sdp_neg_get_active_remote(negotiator_, &active_remote) != PJ_SUCCESS
        or active_remote == nullptr) {
        SIP_CORE_ERR("Could not retrieve remote active session");
        return false;
    }

    setActiveRemoteSdpSession(active_remote);

    printSession(active_remote, "Remote active session:", sdpDirection_);

    return true;
}

std::string
Sdp::getFilteredSdp(const pjmedia_sdp_session* session, unsigned media_keep, unsigned pt_keep)
{
    static constexpr size_t BUF_SZ = 4096;
    std::unique_ptr<pj_pool_t, decltype(&pj_pool_release)>
        tmpPool_(pj_pool_create(&Manager::instance().sipVoIPLink().getCachingPool()->factory,
                                "tmpSdp",
                                BUF_SZ,
                                BUF_SZ,
                                nullptr),
                 &pj_pool_release);
    auto cloned = pjmedia_sdp_session_clone(tmpPool_.get(), session);
    if (!cloned) {
        SIP_CORE_ERR("Could not clone SDP");
        return "";
    }

    // deactivate non-video media
    bool hasKeep = false;
    for (unsigned i = 0; i < cloned->media_count; i++)
        if (i != media_keep) {
            if (pjmedia_sdp_media_deactivate(tmpPool_.get(), cloned->media[i]) != PJ_SUCCESS)
                SIP_CORE_ERR("Could not deactivate media");
        } else {
            hasKeep = true;
        }

    if (not hasKeep) {
        SIP_CORE_DBG("No media to keep present in SDP");
        return "";
    }

    // Leaking medias will be dropped with tmpPool_
    for (unsigned i = 0; i < cloned->media_count; i++)
        if (cloned->media[i]->desc.port == 0) {
            std::move(cloned->media + i + 1, cloned->media + cloned->media_count, cloned->media + i);
            cloned->media_count--;
            i--;
        }

    for (unsigned i = 0; i < cloned->media_count; i++) {
        auto media = cloned->media[i];

        // filter other codecs
        for (unsigned c = 0; c < media->desc.fmt_count; c++) {
            auto& pt = media->desc.fmt[c];
            if (pj_strtoul(&pt) == pt_keep)
                continue;

            while (auto attr = pjmedia_sdp_attr_find2(media->attr_count, media->attr, "rtpmap", &pt))
                pjmedia_sdp_attr_remove(&media->attr_count, media->attr, attr);

            while (auto attr = pjmedia_sdp_attr_find2(media->attr_count, media->attr, "fmt", &pt))
                pjmedia_sdp_attr_remove(&media->attr_count, media->attr, attr);

            std::move(media->desc.fmt + c + 1,
                      media->desc.fmt + media->desc.fmt_count,
                      media->desc.fmt + c);
            media->desc.fmt_count--;
            c--;
        }

        // we handle crypto ourselfs, don't tell libav about it
        pjmedia_sdp_media_remove_all_attr(media, "crypto");
    }

    char buffer[BUF_SZ];
    size_t size = pjmedia_sdp_print(cloned, buffer, sizeof(buffer));
    string sessionStr(buffer, std::min(size, sizeof(buffer)));

    return sessionStr;
}

std::vector<MediaDescription>
Sdp::getActiveMediaDescription(bool remote) const
{
    if (remote)
        return getMediaDescriptions(activeRemoteSession_, true);

    return getMediaDescriptions(activeLocalSession_, false);
}

std::vector<MediaDescription>
Sdp::getMediaDescriptions(const pjmedia_sdp_session* session, bool remote) const
{
    if (!session)
        return {};
    static constexpr pj_str_t STR_RTPMAP {sip_utils::CONST_PJ_STR("rtpmap")};
    static constexpr pj_str_t STR_FMTP {sip_utils::CONST_PJ_STR("fmtp")};
    static constexpr pj_str_t PCMA_PAYLOAD {sip_utils::CONST_PJ_STR("8")};
    static constexpr pj_str_t PCMU_PAYLOAD {sip_utils::CONST_PJ_STR("0")};
    static constexpr pj_str_t G729_PAYLOAD {sip_utils::CONST_PJ_STR("18")};

    std::vector<MediaDescription> ret;
    for (unsigned i = 0; i < session->media_count; i++) {
        auto media = session->media[i];
        ret.emplace_back(MediaDescription());
        MediaDescription& descr = ret.back();
        if (!pj_stricmp2(&media->desc.media, "audio"))
            descr.type = MEDIA_AUDIO;
        else if (!pj_stricmp2(&media->desc.media, "video"))
            descr.type = MEDIA_VIDEO;
        else {
            ret.pop_back();
            continue;
        }

        descr.enabled = media->desc.port;
        if (!descr.enabled)
            continue;

        // get connection info
        pjmedia_sdp_conn* conn = media->conn ? media->conn : session->conn;
        if (not conn) {
            SIP_CORE_ERR("Could not find connection information for media");
            continue;
        }
        descr.addr = std::string_view(conn->addr.ptr, conn->addr.slen);
        descr.addr.setPort(media->desc.port);

        // Get the "rtcp" address from the SDP if present. Otherwise,
        // infere it from endpoint (RTP) address.
        auto attr = pjmedia_sdp_attr_find2(media->attr_count, media->attr, "rtcp", NULL);
        if (attr) {
            pjmedia_sdp_rtcp_attr rtcp;
            auto status = pjmedia_sdp_attr_get_rtcp(attr, &rtcp);
            if (status == PJ_SUCCESS && rtcp.addr.slen) {
                descr.rtcp_addr = std::string_view(rtcp.addr.ptr, rtcp.addr.slen);
                descr.rtcp_addr.setPort(rtcp.port);
            }
        }

        descr.onHold = pjmedia_sdp_attr_find2(media->attr_count,
                                              media->attr,
                                              DIRECTION_STR[MediaDirection::RECVONLY],
                                              nullptr)
                       || pjmedia_sdp_attr_find2(media->attr_count,
                                                 media->attr,
                                                 DIRECTION_STR[MediaDirection::INACTIVE],
                                                 nullptr);

        descr.direction_ = getMediaDirection(media);
        if (descr.direction_ == MediaDirection::UNKNOWN) {
            SIP_CORE_ERR("Did not find media direction attribute in remote SDP");
        }

        // get codecs infos
        for (unsigned j = 0; j < media->desc.fmt_count; j++) {
            const auto rtpMapAttribute = pjmedia_sdp_media_find_attr(media,
                                                                     &STR_RTPMAP,
                                                                     &media->desc.fmt[j]);
            if (!rtpMapAttribute) {
                descr.enabled = false;
                SIP_CORE_ERR(
                    "Could not find rtpmap attribute for %s, trying to guess by payload type",
                    media->desc.fmt[j].ptr);
                if (!pj_strcmp(&media->desc.fmt[j], &PCMA_PAYLOAD)) {
                    SIP_CORE_WARN("Found that payload %s can be PCMA 8000", media->desc.fmt[j].ptr);
                    descr.codec = findCodecBySpec("PCMA", 8000);
                    if (not descr.codec) {
                        SIP_CORE_ERR("Could not find codec for %s", media->desc.fmt[j].ptr);
                    } else {
                        // for now, just keep the first codec only
                        descr.enabled = true;
                        descr.payload_type = 8;
                        descr.rtp_clockrate = 8000;
                        SIP_CORE_INFO("Found codec for %s", media->desc.fmt[j].ptr);
                        break;
                    }
                }

                if (!pj_strcmp(&media->desc.fmt[j], &PCMU_PAYLOAD)) {
                    SIP_CORE_WARN("Found that payload %s can be PCMU 8000", media->desc.fmt[j].ptr);
                    descr.codec = findCodecBySpec("PCMU", 8000);
                    if (not descr.codec) {
                        SIP_CORE_ERR("Could not find codec for %s", media->desc.fmt[j].ptr);
                    } else {
                        // for now, just keep the first codec only
                        descr.enabled = true;
                        descr.payload_type = 0;
                        descr.rtp_clockrate = 8000;
                        SIP_CORE_INFO("Found codec for %s", media->desc.fmt[j].ptr);
                        break;
                    }
                }

                if (!pj_strcmp(&media->desc.fmt[j], &G729_PAYLOAD)) {
                    SIP_CORE_WARN("Found that payload %s can be G729 8000", media->desc.fmt[j].ptr);
                    descr.codec = findCodecBySpec("G729", 8000);

                    // for now, just keep the first codec only
                    descr.enabled = true;
                    descr.payload_type = 18;
                    descr.rtp_clockrate = 8000;
                    pjmedia_sdp_attr *attr = pjmedia_sdp_media_find_attr2(media, "fmtp", &media->desc.fmt[j]);
                    if(attr) {
                        pjmedia_sdp_fmtp fmtp;
                        pjmedia_sdp_attr_get_fmtp(attr, &fmtp);
                        if(!pj_strcmp2(&fmtp.fmt_param, "annexb=no")) {
                            SIP_CORE_INFO("G.729 Annex B is not supported by the receiver. Disabling it.");
                            descr.annex_b = false;
                        }   
                    }
                    
                    SIP_CORE_INFO("Found codec for %s", media->desc.fmt[j].ptr);
                    break;
                }

                continue;
            }
            pjmedia_sdp_rtpmap rtpmap;
            if (pjmedia_sdp_attr_get_rtpmap(rtpMapAttribute, &rtpmap) != PJ_SUCCESS
                || rtpmap.enc_name.slen == 0) {
                SIP_CORE_ERR("Could not find payload type %.*s in SDP",
                             (int) media->desc.fmt[j].slen,
                             media->desc.fmt[j].ptr);
                descr.enabled = false;
                continue;
            }
            auto codec_raw = sip_utils::as_view(rtpmap.enc_name);
            descr.rtp_clockrate = rtpmap.clock_rate;
            descr.codec = findCodecBySpec(codec_raw, rtpmap.clock_rate);
            if (not descr.codec) {
                SIP_CORE_ERR("Could not find codec %.*s", (int) codec_raw.size(), codec_raw.data());
                descr.enabled = false;
                continue;
            }
            descr.payload_type = pj_strtoul(&rtpmap.pt);
            if (descr.type == MEDIA_VIDEO) {
                const auto fmtpAttr = pjmedia_sdp_media_find_attr(media,
                                                                  &STR_FMTP,
                                                                  &media->desc.fmt[j]);
                // descr.bitrate = getOutgoingVideoField(codec, "bitrate");
                if (fmtpAttr && fmtpAttr->value.ptr && fmtpAttr->value.slen) {
                    const auto& v = fmtpAttr->value;
                    descr.parameters = std::string(v.ptr, v.ptr + v.slen);
                }
            }
            // for now, just keep the first codec only
            descr.enabled = true;
            break;
        }

        if (not remote)
            descr.receiving_sdp = getFilteredSdp(session, i, descr.payload_type);

        // get crypto info
        std::vector<std::string> crypto;
        for (unsigned j = 0; j < media->attr_count; j++) {
            const auto attribute = media->attr[j];
            if (pj_stricmp2(&attribute->name, "crypto") == 0)
                crypto.emplace_back(attribute->value.ptr, attribute->value.slen);
        }
        descr.crypto = SdesNegotiator::negotiate(crypto);
    }
    return ret;
}

std::vector<Sdp::MediaSlot>
Sdp::getMediaSlots() const
{
    auto loc = getMediaDescriptions(activeLocalSession_, false);
    auto rem = getMediaDescriptions(activeRemoteSession_, true);
    size_t slot_n = std::min(loc.size(), rem.size());
    std::vector<MediaSlot> s;
    s.reserve(slot_n);
    for (decltype(slot_n) i = 0; i < slot_n; i++)
        s.emplace_back(std::move(loc[i]), std::move(rem[i]));
    return s;
}

std::vector<MediaAttribute>
Sdp::getMediaAttributeListFromSdp(const pjmedia_sdp_session* sdpSession,
                                  bool ignoreDisabled,
                                  bool remote)
{
    if (sdpSession == nullptr) {
        return {};
    }

    std::vector<MediaAttribute> mediaList;
    unsigned audioIdx = 0;
    unsigned videoIdx = 0;
    for (unsigned idx = 0; idx < sdpSession->media_count; idx++) {
        mediaList.emplace_back(MediaAttribute {});
        auto& mediaAttr = mediaList.back();

        auto const& media = sdpSession->media[idx];

        // Get media type.
        if (!pj_stricmp2(&media->desc.media, "audio"))
            mediaAttr.type_ = MediaType::MEDIA_AUDIO;
        else if (!pj_stricmp2(&media->desc.media, "video"))
            mediaAttr.type_ = MediaType::MEDIA_VIDEO;
        else {
            SIP_CORE_WARN("Media#%u only 'audio' and 'video' types are supported!", idx);
            // Disable the media. No need to parse the attributes.
            mediaAttr.enabled_ = false;
            mediaList.pop_back();
            continue;
        }

        // Set enabled flag
        mediaAttr.enabled_ = media->desc.port > 0;

        if (!mediaAttr.enabled_ && ignoreDisabled) {
            mediaList.pop_back();
            continue;
        }

        // Get mute state for remote perspective only.
        auto direction = getMediaDirection(media);
        if (remote) {
            mediaAttr.muted_ = direction == MediaDirection::RECVONLY;
        }

        // Get transport.
        auto transp = getMediaTransport(media);
        if (transp == MediaTransport::UNKNOWN) {
            SIP_CORE_WARN("Media#%u could not determine transport type!", idx);
        }

        // A media is secure if the transport is of type RTP/SAVP
        // and the crypto materials are present.
        mediaAttr.secure_ = transp == MediaTransport::RTP_SAVP and not getCrypto(media).empty();

        if (mediaAttr.type_ == MediaType::MEDIA_AUDIO) {
            mediaAttr.label_ = "audio_" + std::to_string(audioIdx++);
        } else if (mediaAttr.type_ == MediaType::MEDIA_VIDEO) {
            mediaAttr.label_ = "video_" + std::to_string(videoIdx++);
        }
    }

    return mediaList;
}

} // namespace sip_core
