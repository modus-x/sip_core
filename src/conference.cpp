/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
 *  Author: Alexandre Savard <alexandre.savard@savoirfairelinux.com>
 *  Author: Guillaume Roguez <guillaume.roguez@savoirfairelinux.com>
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

#include <regex>
#include <sstream>
#include <algorithm>

#include "conference.h"
#include "manager.h"
#include "audio/audiolayer.h"
#include "string_utils.h"
#include "sip/siptransport.h"

#include "client/videomanager.h"
#include "tracepoint.h"
#ifdef ENABLE_VIDEO
#include "call.h"
#include "video/video_input.h"
#include "video/video_mixer.h"
#include "video/video_source_utils.h"
#endif

#include "call_factory.h"

#include "logger.h"
#include "sip_core/media_const.h"
#include "audio/ringbufferpool.h"
#include "sip/sipcall.h"
#include "sip/sipaccount.h"

using namespace std::literals;

namespace sip_core {

Conference::Conference(const std::shared_ptr<Account>& account, const std::string& confId)
    : id_(confId.empty() ? Manager::instance().callFactory.getNewCallID() : confId)
    , account_(account)
#ifdef ENABLE_VIDEO
    , videoEnabled_(account->isVideoEnabled())
#endif
{
    SIP_CORE_INFO("Create new conference %s", id_.c_str());

    duration_start_ = clock::now();

#ifdef ENABLE_VIDEO
    videoMixer_ = std::make_shared<video::VideoMixer>(id_);
    // NOTE: the VideoMixer onSourcesUpdated callback is registered separately in
    // attachVideoMixerCallbacks(), called right after this Conference is owned by
    // a shared_ptr. It captures weak_from_this() by value so the mixer process()
    // thread never dereferences a raw `this` (the createSinks/teardown UAF). We
    // cannot do it here because weak_from_this() is invalid in the constructor.

    auto conf_res = split_string_to_unsigned(sip_core::Manager::instance()
                                                 .videoPreferences.getConferenceResolution(),
                                             'x');
    const auto voiceInactiveHoldMs = sip_core::Manager::instance()
                                         .videoPreferences.getConferenceVoiceInactiveHoldMs();
    if (conf_res.size() == 2u) {
#if defined(__APPLE__) && TARGET_OS_MAC
        auto params = video::VideoMixer::Parameters {(int) conf_res[0],
                                                     (int) conf_res[1],
                                                     AV_PIX_FMT_NV12};
#else
        auto params = video::VideoMixer::Parameters {(int) conf_res[0], (int) conf_res[1]};
#endif
        params.voice_inactive_hold_ms = voiceInactiveHoldMs;
        videoMixer_->setParameters(params);
    } else {
        SIP_CORE_ERR("Conference resolution is invalid");
    }
#endif

    parser_.onVersion([&](uint32_t) {}); // TODO
    parser_.onCheckAuthorization([&](std::string_view peerId) { return isModerator(peerId); });
    parser_.onHangupParticipant([&](const auto& accountUri, const auto& deviceId) {
        hangupParticipant(accountUri, deviceId);
    });
    parser_.onRaiseHand([&](const auto& accountUri, const auto& deviceId, bool state) {
        setHandRaised(accountUri, deviceId, state);
    });
    parser_.onSetActiveStream(
        [&](const auto& streamId, bool state) { setActiveStream(streamId, state); });
    parser_.onMuteStreamAudio(
        [&](const auto& accountUri, const auto& deviceId, const auto& streamId, bool state) {
            muteStream(accountUri, deviceId, streamId, state);
        });
    parser_.onSetLayout([&](int layout) { setLayout(layout); });
    parser_.onShareState([&](const auto& peerId, bool state) { onShareState(peerId, state); });

    // Version 0, deprecated
    parser_.onKickParticipant([&](const auto& participantId) { hangupParticipant(participantId); });
    parser_.onSetActiveParticipant(
        [&](const auto& participantId) { setActiveParticipant(participantId); });
    parser_.onMuteParticipant(
        [&](const auto& participantId, bool state) { muteParticipant(participantId, state); });
    parser_.onRaiseHandUri([&](const auto& uri, bool state) { setHandRaised(uri, "", state); });

    parser_.onVoiceActivity(
        [&](const auto& streamId, bool state) { setVoiceActivity(streamId, state); });
    sip_core_tracepoint(conference_begin, id_.c_str());
}

void
Conference::attachVideoMixerCallbacks()
{
#ifdef ENABLE_VIDEO
    if (!videoMixer_)
        return;
    // Capture weak_from_this() BY VALUE. The outer lambda runs on the VideoMixer
    // process() thread; evaluating weak() once here (after a shared_ptr owns this
    // Conference) means that thread never dereferences a raw `this`. w.lock()
    // inside the posted task is null-safe once the Conference has been destroyed,
    // and the captured weak_ptr keeps the control block alive for the lock. This
    // is the root-cause fix for the Conference::createSinks use-after-free.
    videoMixer_->setOnSourcesUpdated([w = weak()](std::vector<video::SourceInfo>&& infos) {
        runOnMainThread([w, infos = std::move(infos)] {
            auto shared = w.lock();
            if (!shared)
                return;
            auto acc = std::dynamic_pointer_cast<SIPAccount>(shared->account_.lock());
            if (!acc)
                return;
            ConfInfo newInfo;
            {
                std::lock_guard<std::mutex> lock(shared->confInfoMutex_);
                newInfo.w = shared->confInfo_.w;
                newInfo.h = shared->confInfo_.h;
                newInfo.layout = shared->confInfo_.layout;
            }
            auto hostAdded = false;
            // Handle participants showing their video
            for (const auto& info : infos) {
                std::string uri {};
                bool isLocalMuted = false, isPeerRecording = false;
                std::string deviceId {};
                auto active = false;
                // if callId is NON empty, then this is remote patricipant
                if (!info.callId.empty()) {
                    std::string callId = info.callId;
                    if (auto call = std::dynamic_pointer_cast<SIPCall>(getCall(callId))) {
                        uri = call->getPeerNumber();
                        isLocalMuted = call->isPeerMuted();
                        isPeerRecording = call->isPeerRecording();
                        deviceId = "";
                    }
                    std::string_view peerId = sip_utils::stripSipUriPrefix(uri);
                    auto isModerator = shared->isModerator(peerId);
                    auto isHandRaised = shared->isHandRaised(callId);
                    auto isModeratorMuted = shared->isMuted(callId);
                    auto isVoiceActive = shared->isVoiceActive(info.streamId);
                    if (auto videoMixer = shared->videoMixer_)
                        active = videoMixer->verifyActive(info.streamId);
                    newInfo.emplace_back(ParticipantInfo {std::move(uri),
                                                          deviceId,
                                                          std::move(info.streamId),
                                                          active,
                                                          info.x,
                                                          info.y,
                                                          info.w,
                                                          info.h,
                                                          !info.hasVideo,
                                                          isLocalMuted,
                                                          isModeratorMuted,
                                                          isModerator,
                                                          isHandRaised,
                                                          isVoiceActive,
                                                          isPeerRecording,
                                                          callId});
                } else {
                    auto isModeratorMuted = false;
                    // If not local
                    auto streamInfo = shared->videoMixer_->streamInfo(info.source);
                    std::string streamId = streamInfo.streamId;
                    std::string callId;
                    if (!streamId.empty()) {
                        // Retrieve calls participants
                        // TODO: this is a first version, we assume that the peer is not
                        // a master of a conference and there is only one remote
                        // In the future, we should retrieve confInfo from the call
                        // To merge layout information
                        // participantsMuted_ is keyed by call id, not stream id.
                        isModeratorMuted = shared->isMuted(streamInfo.callId);
                        if (auto videoMixer = shared->videoMixer_)
                            active = videoMixer->verifyActive(streamId);
                        if (auto call = std::dynamic_pointer_cast<SIPCall>(
                                getCall(streamInfo.callId))) {
                            uri = call->getPeerNumber();
                            isLocalMuted = call->isPeerMuted();
                            isPeerRecording = call->isPeerRecording();
                            deviceId = "";
                            callId = streamInfo.callId;
                        }
                    } else {
                        streamId = sip_utils::streamId("", sip_utils::DEFAULT_VIDEO_STREAMID);
                        if (auto videoMixer = shared->videoMixer_)
                            active = videoMixer->verifyActive(streamId);
                    }
                    std::string_view peerId = sip_utils::stripSipUriPrefix(uri);
                    auto isModerator = shared->isModerator(peerId);
                    if (uri.empty() && !hostAdded) {
                        hostAdded = true;
                        deviceId = "";
                        isLocalMuted = shared->isMediaSourceMuted(MediaType::MEDIA_AUDIO);
                        isPeerRecording = shared->isRecording();
                    }
                    auto isHandRaised = shared->isHandRaised(uri.empty() ? "host"
                                                                         : std::string_view(callId));
                    auto isVoiceActive = shared->isVoiceActive(streamId);
                    newInfo.emplace_back(ParticipantInfo {std::move(uri),
                                                          deviceId,
                                                          std::move(streamId),
                                                          active,
                                                          info.x,
                                                          info.y,
                                                          info.w,
                                                          info.h,
                                                          !info.hasVideo,
                                                          isLocalMuted,
                                                          isModeratorMuted,
                                                          isModerator,
                                                          isHandRaised,
                                                          isVoiceActive,
                                                          isPeerRecording,
                                                          callId});
                }
            }
            if (auto videoMixer = shared->videoMixer_) {
                newInfo.h = videoMixer->getHeight();
                newInfo.w = videoMixer->getWidth();
            }
            if (!hostAdded) {
                ParticipantInfo pi;
                pi.videoMuted = true;
                pi.audioLocalMuted = shared->isMediaSourceMuted(MediaType::MEDIA_AUDIO);
                pi.isModerator = true;
                newInfo.emplace_back(pi);
            }

            // Screen share: flag the active sharer's row so every client renders
            // the "X is sharing" UI, and detect share-stop (the sharer muted its
            // desktop source or left the conference) to restore the grid.
            {
                std::string sharer;
                bool hadVideo = false;
                {
                    std::lock_guard<std::mutex> lk(shared->sharerMtx_);
                    sharer = shared->activeSharerStreamId_;
                    hadVideo = shared->sharerHadVideo_;
                }
                if (!sharer.empty()) {
                    ParticipantInfo* row = nullptr;
                    for (auto& pi : newInfo) {
                        if (pi.sinkId == sharer) {
                            row = &pi;
                            break;
                        }
                    }
                    if (row && !row->videoMuted) {
                        row->isSharing = true;
                        if (!hadVideo) {
                            std::lock_guard<std::mutex> lk(shared->sharerMtx_);
                            shared->sharerHadVideo_ = true;
                        }
                    } else if (hadVideo) {
                        // Sharer left (row == null) or muted its desktop source.
                        runOnMainThread([w] {
                            if (auto s = w.lock())
                                s->endCurrentShare();
                        });
                    }
                }
            }

            shared->updateConferenceInfo(std::move(newInfo));
        });
    });
#endif
}

Conference::~Conference()
{
    SIP_CORE_INFO("Destroying conference %s", id_.c_str());

    // Clear all per-participant pool filters for this conference.
    auto& rbPool = Manager::instance().getRingBufferPool();
    for (const auto& p : getParticipantList()) {
        rbPool.setLocalPlaybackMuted(p, false);
        rbPool.setMicMuted(p, false);
    }

#ifdef ENABLE_VIDEO
    foreachCall([&](auto call) {
        call->exitConference();
        // Reset distant callInfo
        call->resetConfInfo();
        // Trigger the SIP negotiation to update the resolution for the remaining call

        // call->switchInput(
        //     Manager::instance().getVideoManager().videoDeviceMonitor.getMRLForDefaultDevice());

        // Continue the recording for the call if the conference was recorded
        // if (isRecording()) {
        //     SIP_CORE_DEBUG("Stop recording for conf {:s}", getConfId());
        //     toggleRecording();
        //     if (not call->isRecording()) {
        //         SIP_CORE_DEBUG("Conference was recorded, start recording for conf {:s}",
        //                        call->getCallId());
        //         call->toggleRecording();
        //     }
        // }
        // // Notify that the remaining peer is still recording after conference
        // if (call->isPeerRecording())
        //     call->peerRecording(true);
    });
    if (videoMixer_) {
        std::lock_guard<std::mutex> lk(sinksMtx_);
        auto sink = videoMixer_->getSink(); // strong copy: keep the sink alive for the loop
        for (auto it = confSinksMap_.begin(); it != confSinksMap_.end();) {
            sink->detach(it->second.get());
            it->second->stop();
            it = confSinksMap_.erase(it);
        }
    }
#endif // ENABLE_VIDEO
    if (shutdownCb_)
        shutdownCb_(getDuration().count());
    sip_core_tracepoint(conference_end, id_.c_str());
}

Conference::State
Conference::getState() const
{
    return confState_;
}

void
Conference::setState(State state)
{
    SIP_CORE_DEBUG("[conf {:s}] Set state to [{:s}] (was [{:s}])",
                   id_,
                   getStateStr(state),
                   getStateStr());

    confState_ = state;
}

void
Conference::setLocalHostDefaultMediaSource(bool addVideo, const std::string& source)
{
    hostSources_.clear();
    // Setup local audio source
    MediaAttribute audioAttr;
    audioAttr = {MediaType::MEDIA_AUDIO, false, false, true, {}, sip_utils::DEFAULT_AUDIO_STREAMID};

    SIP_CORE_DEBUG("[conf {:s}] Setting local host audio source to [{:s}]",
                   id_,
                   audioAttr.toString());
    hostSources_.emplace_back(audioAttr);

#ifdef ENABLE_VIDEO
    if (isVideoEnabled() && addVideo) {
        MediaAttribute videoAttr = {MediaType::MEDIA_VIDEO,
                                    false,
                                    false,
                                    true,
                                    source.empty()
                                        ? Manager::instance()
                                              .getVideoManager()
                                              .videoDeviceMonitor.getMRLForDefaultDevice()
                                        : source,
                                    sip_utils::DEFAULT_VIDEO_STREAMID};
        SIP_CORE_DEBUG("[conf {:s}] Setting local host video source to [{:s}]",
                       id_,
                       videoAttr.toString());
        hostSources_.emplace_back(videoAttr);
    }

#endif
    reportMediaNegotiationStatus();
}

void
Conference::reportMediaNegotiationStatus(const std::string& event)
{
    emitSignal<libsip_core::CallSignal::MediaNegotiationStatus>(
        getConfId(),
        event,
        currentMediaList());
}

std::vector<std::map<std::string, std::string>>
Conference::currentMediaList() const
{
    return MediaAttribute::mediaAttributesToMediaMaps(hostSources_);
}

void
Conference::setLocalHostMuteState(MediaType type, bool muted)
{
    for (auto& source : hostSources_)
        if (source.type_ == type)
            source.muted_ = muted;
}

bool
Conference::isMediaSourceMuted(MediaType type) const
{
    if (getState() != State::ACTIVE_ATTACHED) {
        // Assume muted if not attached.
        return true;
    }

    if (type != MediaType::MEDIA_AUDIO and type != MediaType::MEDIA_VIDEO) {
        SIP_CORE_ERR("Unsupported media type");
        return true;
    }

    for (const auto& source : hostSources_) {
        if (source.muted_ && source.type_ == type)
            return true;
        if (source.type_ == MediaType::MEDIA_NONE) {
            SIP_CORE_WARN("The host source for %s is not set. The mute state is meaningless",
                          source.mediaTypeToString(source.type_));
            // Assume muted if the media is not present.
            return true;
        }
    }
    return false;
}

void
Conference::takeOverMediaSourceControl(const std::string& callId)
{
    auto call = getCall(callId);
    if (not call) {
        SIP_CORE_ERR("No call matches participant %s", callId.c_str());
        return;
    }

    auto account = call->getAccount().lock();
    if (not account) {
        SIP_CORE_ERR("No account detected for call %s", callId.c_str());
        return;
    }

    auto mediaList = call->getMediaAttributeList();

#ifdef ENABLE_VIDEO
    const bool participantWasAudioOnly = std::none_of(mediaList.begin(),
                                                      mediaList.end(),
                                                      [](const MediaAttribute& media) {
                                                          return media.hasValidVideo();
                                                      });
    if (participantWasAudioOnly) {
        if (isVideoEnabled()) {
            auto videoIter = std::find_if(mediaList.begin(),
                                          mediaList.end(),
                                          [](const auto& mediaAttr) {
                                              return mediaAttr.type_ == MediaType::MEDIA_VIDEO;
                                          });
            const auto defaultVideoSource
                = Manager::instance().getVideoManager().videoDeviceMonitor.getMRLForDefaultDevice();

            if (videoIter == mediaList.end()) {
                mediaList.emplace_back(MediaType::MEDIA_VIDEO,
                                       false,
                                       false,
                                       true,
                                       defaultVideoSource,
                                       sip_utils::DEFAULT_VIDEO_STREAMID,
                                       false);
            } else {
                videoIter->enabled_ = true;
                if (videoIter->label_.empty())
                    videoIter->label_ = sip_utils::DEFAULT_VIDEO_STREAMID;
                if (videoIter->sourceUri_.empty())
                    videoIter->sourceUri_ = defaultVideoSource;
            }

            SIP_CORE_INFO(
                "[Call: %s] Forcing conference media upgrade from audio-only to audio+video",
                callId.c_str());
        } else {
            SIP_CORE_WARN("[Call: %s] Participant is audio-only, but conference/account video is "
                          "disabled. Skipping forced video media upgrade",
                          callId.c_str());
        }
    }
#endif

    std::vector<MediaType> mediaTypeList {MediaType::MEDIA_AUDIO, MediaType::MEDIA_VIDEO};

    for (auto mediaType : mediaTypeList) {
        // Try to find a media with a valid source type
        auto check = [mediaType](auto const& mediaAttr) {
            return (mediaAttr.type_ == mediaType);
        };

        auto iter = std::find_if(mediaList.begin(), mediaList.end(), check);

        if (iter == mediaList.end()) {
            // Nothing to do if the call does not have a stream with
            // the requested media.
            SIP_CORE_DEBUG("[Call: {:s}] Does not have an active [{:s}] media source",
                           callId,
                           MediaAttribute::mediaTypeToString(mediaType));
            continue;
        }

        if (getState() == State::ACTIVE_ATTACHED) {
            // If it's the first participant, just use its mute state as local
            if (participants_.size() == 1) {
                setLocalHostMuteState(iter->type_, iter->muted_);
            }
            // Otherwise leave the host mute state untouched: the mute flags
            // of a joining call must not clear (or set) the host-wide mute.
        }

        // The call may still be in HOLD when ManagerPimpl::bindCallToConference()
        // invokes addParticipant() and only calls offHoldCall() afterwards.
        // Clear the per-stream hold bit before requestMediaChange(), otherwise
        // the conference takeover re-INVITE replays the stale hold state and
        // advertises sendonly SDP for the newly added participant.
        iter->onHold_ = false;

        // Un-mute media in the call. The mute/un-mute state will be handled
        // by the conference/mixer from now on.
        iter->muted_ = false;
    }

    // Update the media states in the newly added call.
    call->requestMediaChange(MediaAttribute::mediaAttributesToMediaMaps(mediaList));

    // Notify the client
    for (auto mediaType : mediaTypeList) {
        if (mediaType == MediaType::MEDIA_AUDIO) {
            bool muted = isMediaSourceMuted(MediaType::MEDIA_AUDIO);
            SIP_CORE_WARN(
                "Take over [AUDIO] control from call %s - current local source state [%s]",
                callId.c_str(),
                muted ? "muted" : "un-muted");
            emitSignal<libsip_core::CallSignal::AudioMuted>(id_, muted);
        } else {
            bool muted = isMediaSourceMuted(MediaType::MEDIA_VIDEO);
            SIP_CORE_WARN(
                "Take over [VIDEO] control from call %s - current local source state [%s]",
                callId.c_str(),
                muted ? "muted" : "un-muted");
            emitSignal<libsip_core::CallSignal::VideoMuted>(id_, muted);
        }
    }
}

bool
Conference::requestMediaChange(const std::vector<libsip_core::MediaMap>& mediaList)
{
    if (getState() != State::ACTIVE_ATTACHED) {
        SIP_CORE_ERR("[conf %s] Request media change can be performed only in attached mode",
                     getConfId().c_str());
        return false;
    }

    SIP_CORE_DEBUG("[conf {:s}] Request media change", getConfId());

    auto mediaAttrList = MediaAttribute::buildMediaAttributesList(mediaList, false);

    for (auto const& mediaAttr : mediaAttrList) {
        SIP_CORE_DEBUG("[conf {:s}] New requested media: {:s}",
                       getConfId(),
                       mediaAttr.toString(true));
    }

    for (auto& mediaAttr : mediaAttrList) {
        // Find media
        auto oldIdx = std::find_if(hostSources_.begin(), hostSources_.end(), [&](auto oldAttr) {
            return oldAttr.sourceUri_ == mediaAttr.sourceUri_ && oldAttr.type_ == mediaAttr.type_;
        });

        // if it new source, we should switch inputs
        if (oldIdx == hostSources_.end()) {
            std::vector<std::string> newVideoInputs;
            // If video, add to newVideoInputs (if not specified, set default device)
            // NOTE: For now, only supports video
            if (mediaAttr.type_ == MediaType::MEDIA_VIDEO) {
                if (mediaAttr.sourceUri_ == "") {
                    mediaAttr.sourceUri_ = Manager::instance()
                                               .getVideoManager()
                                               .videoDeviceMonitor.getMRLForDefaultDevice();
                }

                newVideoInputs.emplace_back(mediaAttr.sourceUri_);

                // if videoMixer_ is defined, switch inputs!
                if (videoMixer_) {
                    videoMixer_->switchInputs(newVideoInputs);
                    // Remove the host's audio-only placeholder now that real
                    // video is being attached (mirrors handleMediaChangeRequest
                    // logic for remote participants).
                    videoMixer_->removeAudioOnlySource(
                        "",
                        sip_utils::streamId("", sip_utils::DEFAULT_VIDEO_STREAMID));
                }
            }
        }

        // Check if muted status changes OR if new device is added
        if (oldIdx == hostSources_.end() || mediaAttr.muted_ != oldIdx->muted_) {
            // If the current media source is muted, just call un-mute, it
            // will attach / detach input to / from mixer
            muteLocalHost(mediaAttr.muted_,
                          mediaAttr.type_ == MediaType::MEDIA_AUDIO
                              ? libsip_core::Media::Details::MEDIA_TYPE_AUDIO
                              : libsip_core::Media::Details::MEDIA_TYPE_VIDEO);
        }
    }

    hostSources_ = mediaAttrList; // New medias

    // It's host medias, so no need to negotiate anything, but inform the client.
    reportMediaNegotiationStatus();

    return true;
}

// handle media change request OF CALL -> should auto - add / auto - delete patricipant video from mixer!
void
Conference::handleMediaChangeRequest(const std::shared_ptr<Call>& call,
                                     const std::vector<libsip_core::MediaMap>& remoteMediaList)
{
    SIP_CORE_DEBUG("Conf [{:s}] Answer to media change request", getConfId());
    auto currentMediaList = hostSources_;

#ifdef ENABLE_VIDEO
    // If the new media list has video, remove the participant from audioonlylist.
    auto remoteHasVideo
        = MediaAttribute::hasMediaType(MediaAttribute::buildMediaAttributesList(remoteMediaList,
                                                                                false),
                                       MediaType::MEDIA_VIDEO);
    if (videoMixer_ && remoteHasVideo) {
        auto callId = call->getCallId();
        videoMixer_->removeAudioOnlySource(
            callId, std::string(sip_utils::streamId(callId, sip_utils::DEFAULT_AUDIO_STREAMID)));
    }
#endif

    auto remoteList = remoteMediaList;
    for (auto it = remoteList.begin(); it != remoteList.end();) {
        if (it->at(libsip_core::Media::MediaAttributeKey::MUTED) == TRUE_STR
            or it->at(libsip_core::Media::MediaAttributeKey::ENABLED) == FALSE_STR) {
            it = remoteList.erase(it);
        } else {
            ++it;
        }
    }
    // Create minimum media list (ignore muted and disabled medias)
    std::vector<libsip_core::MediaMap> newMediaList;
    newMediaList.reserve(remoteMediaList.size());
    for (auto const& media : currentMediaList) {
        if (media.enabled_ and not media.muted_)
            newMediaList.emplace_back(MediaAttribute::toMediaMap(media));
    }
    for (auto idx = newMediaList.size(); idx < remoteMediaList.size(); idx++)
        newMediaList.emplace_back(remoteMediaList[idx]);

    // NOTE:
    // Since this is a conference, newly added media will be also
    // accepted.
    // This also means that if original call was an audio-only call,
    // the local camera will be enabled, unless the video is disabled
    // in the account settings.
    call->answerMediaChangeRequest(newMediaList);
    // Only (re-)enter the conference if the call is not already in THIS
    // conference.  When the call is already a member, the conference
    // pipelines will be set up by VideoRtpSession::start() during SDP
    // completion — calling enterConference() again would cause a
    // redundant detach/reattach cycle that creates transient duplicate
    // sources in the video mixer (the detach is deferred while the
    // attach is immediate).
    if (call->getConference().get() != this)
        call->enterConference(shared_from_this());
}

void
Conference::addParticipant(const std::string& participant_id)
{
    SIP_CORE_DEBUG("Adding call {:s} to conference {:s}", participant_id, id_);

    sip_core_tracepoint(conference_add_participant, id_.c_str(), participant_id.c_str());

    {
        std::lock_guard<std::mutex> lk(participantsMtx_);
        if (!participants_.insert(participant_id).second)
            return;
    }

    if (auto call = std::dynamic_pointer_cast<SIPCall>(getCall(participant_id))) {
#ifdef ENABLE_VIDEO
        const auto mediaBeforeTakeover = call->getMediaAttributeList();
        const bool participantWasAudioOnly = std::none_of(mediaBeforeTakeover.begin(),
                                                          mediaBeforeTakeover.end(),
                                                          [](const MediaAttribute& media) {
                                                              return media.hasValidVideo();
                                                          });
#endif
        // Check if participant was muted before conference
        if (call->isPeerMuted())
            participantsMuted_.emplace(call->getCallId());
        // Mark conference audio management before takeover-triggered
        // renegotiation can restart RTP receive threads.  Otherwise the
        // restart path may briefly treat this as a 1:1 call and bind its
        // audio directly to local playback.
        call->setConferenceAudioManaged(true);

        // NOTE:
        // When a call joins a conference, the media source of the call
        // will be set to the output of the conference mixer.
        takeOverMediaSourceControl(participant_id);

        // If local playback is muted, mark this new participant in the
        // ring buffer pool so getData(DEFAULT_ID) skips its audio.
        if (localPlaybackMuted_)
            Manager::instance().getRingBufferPool().setLocalPlaybackMuted(
                call->getCallId(), true);

        auto w = call->getAccount();
        auto account = w.lock();
        if (account) {
            // Add defined moderators for the account link to the call
            for (const auto& mod : account->getDefaultModerators()) {
                moderators_.emplace(mod);
            }

            // Check for localModeratorsEnabled preference
            if (account->isLocalModeratorsEnabled() && not localModAdded_) {
                auto accounts = sip_core::Manager::instance().getAllAccounts<SIPAccount>();
                for (const auto& account : accounts) {
                    moderators_.emplace(account->getUsername());
                }
                localModAdded_ = true;
            }

            // Check for allModeratorEnabled preference
            if (account->isAllModerators())
                moderators_.emplace(getRemoteId(call));
        }
#ifdef ENABLE_VIDEO
        // Keep a visible placeholder while the call is upgraded to include video.
        if (videoMixer_ && participantWasAudioOnly) {
            videoMixer_->addAudioOnlySource(call->getCallId(),
                                            sip_utils::streamId(call->getCallId(),
                                                                sip_utils::DEFAULT_AUDIO_STREAMID),
                                            call->getPeerNumber());
        }
#endif // ENABLE_VIDEO
        call->enterConference(shared_from_this());
#ifdef ENABLE_VIDEO
        // Continue the recording for the conference if one participant was recording
        if (call->isRecording()) {
            SIP_CORE_DEBUG("Stop recording for call {:s}", call->getCallId());
            call->toggleRecording();
            if (not this->isRecording()) {
                SIP_CORE_DEBUG("One participant was recording, start recording for conference {:s}",
                               getConfId());
                this->toggleRecording();
            }
        }
#endif // ENABLE_VIDEO
    } else
        SIP_CORE_ERR("no call associate to participant %s", participant_id.c_str());
}

bool
Conference::moveParticipant(const std::string& participant_id, size_t to)
{
    SIP_CORE_DEBUG("Moving participant {:s} to position {:s} in conference {:s}",
                   participant_id,
                   std::to_string(to),
                   id_);
    // todo: add finding participant id
    return false;
}

bool
Conference::moveParticipant(size_t from, size_t to)
{
    SIP_CORE_DEBUG("Moving participant from position {:s} to position {:s} in conference {:s}",
                   std::to_string(from),
                   std::to_string(to),
                   id_);

    if (!videoMixer_)
        return false;

    return videoMixer_->moveSource(from, to);
}

void
Conference::setActiveParticipant(const std::string& participant_id)
{
#ifdef ENABLE_VIDEO
    if (!videoMixer_)
        return;
    // Route through setActiveStream() so this deprecated no-sink / V0 fallback
    // also stamps confInfo_.active and pushes it to remotes (previously it only
    // poked the mixer, so a spotlight requested before per-sink metadata arrived
    // never reached remote participants).
    if (isHost(participant_id)) {
        setActiveStream(sip_utils::streamId("", sip_utils::DEFAULT_VIDEO_STREAMID), true);
        return;
    }
    if (auto call = getCallFromPeerID(participant_id)) {
        setActiveStream(sip_utils::streamId(call->getCallId(), sip_utils::DEFAULT_VIDEO_STREAMID),
                        true);
        return;
    }

    auto remoteHost = findHostforRemoteParticipant(participant_id);
    if (not remoteHost.empty()) {
        // This logic will be handled client side
        SIP_CORE_WARN("Change remote layout is not supported");
        return;
    }
    // Unset active participant by default
    setActiveStream("", false);
#endif
}

void
Conference::setActiveStream(const std::string& streamId, bool state)
{
#ifdef ENABLE_VIDEO
    if (!videoMixer_)
        return;
    if (state)
        videoMixer_->setActiveStream(streamId);
    else
        videoMixer_->resetActiveStream();

    // Stamp `active` into confInfo_ and push it to every remote immediately,
    // mirroring setLayout()'s synchronous isSharing stamp + send. The async
    // mixer onSourcesUpdated_ callback also recomputes active (verifyActive)
    // and re-broadcasts, but only when the render loop next emits
    // (needsUpdate && !layoutInvalidated) — a pure spotlight toggle need not
    // change geometry, so that path is racy/deferred and left remote
    // participants on the old grid while the host UI already reflected the
    // spotlight. Exactly one row (sinkId == streamId) is active while
    // spotlighting; un-spotlighting clears them all. This produces the same
    // result the async verifyActive() path would (activeStream_ == pi.sinkId),
    // so the two stay consistent — this one is just immediate and deterministic.
    {
        std::lock_guard<std::mutex> lk(confInfoMutex_);
        for (auto& pi : confInfo_)
            pi.active = (state && !streamId.empty() && pi.sinkId == streamId);
    }
    sendConferenceInfos();
#endif
}

void
Conference::setLayout(int layout)
{
#ifdef ENABLE_VIDEO
    if (layout < 0 || layout > 2) {
        SIP_CORE_ERR("Unknown layout %u", layout);
        return;
    }
    if (!videoMixer_)
        return;
    // Read the active sharer OUTSIDE confInfoMutex_ to avoid a lock-order
    // inversion with the mixer callback / onShareState.
    std::string sharer;
    {
        std::lock_guard<std::mutex> lk(sharerMtx_);
        sharer = activeSharerStreamId_;
    }
    {
        std::lock_guard<std::mutex> lk(confInfoMutex_);
        confInfo_.layout = layout;
        // Stamp isSharing synchronously so the IMMEDIATE send below already
        // carries it to remotes. Otherwise this synchronous send races the
        // async mixer-driven resend and remotes can latch a layout change with
        // isSharing=false (host is unaffected — it reads the later local emit).
        for (auto& pi : confInfo_)
            pi.isSharing = (!sharer.empty() && pi.sinkId == sharer);
    }
    videoMixer_->setVideoLayout(static_cast<video::Layout>(layout));
    // Push metadata immediately so remote peers receive the layout change
    // even before mixer coordinates are refreshed asynchronously.
    sendConferenceInfos();
#endif
}

void
Conference::onShareState(const std::string& peerId, bool state)
{
#ifdef ENABLE_VIDEO
    if (!videoMixer_)
        return;

    // Resolve the sharer's mixer stream id and whether it may preempt an
    // existing share. The local host (empty peerId) may always share/take over.
    std::string streamId;
    bool sharerMayOverride = false;
    if (peerId.empty() || isHost(peerId)) {
        streamId = sip_utils::streamId("", sip_utils::DEFAULT_VIDEO_STREAMID);
        sharerMayOverride = true;
    } else if (auto call = getCallFromPeerID(peerId)) {
        streamId = sip_utils::streamId(call->getCallId(), sip_utils::DEFAULT_VIDEO_STREAMID);
        sharerMayOverride = isModerator(peerId);
    } else {
        SIP_CORE_WARN("[Conf:%s] onShareState: cannot resolve sharer '%s'",
                      id_.c_str(),
                      peerId.c_str());
        return;
    }

    if (state) {
        {
            std::lock_guard<std::mutex> lk(sharerMtx_);
            if (!activeSharerStreamId_.empty() && activeSharerStreamId_ != streamId
                && !sharerMayOverride) {
                // Someone else is already sharing and this peer is not a
                // moderator: deny. Their client self-reverts because their own
                // confInfo isSharing stays false.
                SIP_CORE_WARN("[Conf:%s] onShareState: '%s' denied (already sharing)",
                              id_.c_str(),
                              peerId.c_str());
                return;
            }
            activeSharerStreamId_ = streamId;
            sharerHadVideo_ = false;
        }
        // Promote the sharer to a full-screen ONE_BIG layout for everyone.
        setActiveStream(streamId, true);
        setLayout(static_cast<int>(video::Layout::ONE_BIG));
    } else {
        {
            std::lock_guard<std::mutex> lk(sharerMtx_);
            if (activeSharerStreamId_ != streamId)
                return; // not the current sharer; ignore
            activeSharerStreamId_.clear();
            sharerHadVideo_ = false;
        }
        setActiveStream(streamId, false);
        setLayout(static_cast<int>(video::Layout::GRID));
    }
#endif
}

void
Conference::endCurrentShare()
{
#ifdef ENABLE_VIDEO
    {
        std::lock_guard<std::mutex> lk(sharerMtx_);
        if (activeSharerStreamId_.empty())
            return;
        activeSharerStreamId_.clear();
        sharerHadVideo_ = false;
    }
    setActiveStream("", false); // resetActiveStream()
    setLayout(static_cast<int>(video::Layout::GRID));
#endif
}

std::vector<std::map<std::string, std::string>>
ConfInfo::toVectorMapStringString() const
{
    // Inject canvas dimensions (the host mixer's total width/height) into
    // every participant entry. The OnConferenceInfosUpdated signal is the
    // only channel that crosses into Dart for both host- and remote-side
    // conference state — denormalising `cw`/`ch` onto every row lets the
    // UI lay tiles out against the host's canvas rather than guessing it
    // from the bounding box of participant rects (which collapses any
    // host-authored padding around the grid). The values are identical
    // for all rows since they describe the conference canvas, not a tile.
    const auto cw = std::to_string(w);
    const auto ch = std::to_string(h);
    std::vector<std::map<std::string, std::string>> infos;
    infos.reserve(size());
    for (const auto& info : *this) {
        auto entry = info.toMap();
        entry["cw"] = cw;
        entry["ch"] = ch;
        infos.emplace_back(std::move(entry));
    }
    return infos;
}

std::string
ConfInfo::toString() const
{
    Json::Value val = {};
    for (const auto& info : *this) {
        val["p"].append(info.toJson());
    }
    val["w"] = w;
    val["h"] = h;
    val["v"] = v;
    val["layout"] = layout;
    return Json::writeString(Json::StreamWriterBuilder {}, val);
}

void
Conference::sendConferenceInfos()
{
#if CONFERENCE_METADATA
    // Inform calls that the layout has changed
    foreachCall([&](auto call) {
        // Produce specific JSON for each participant (2 separate accounts can host ...
        // a conference on a same device, the conference is not link to one account).
        auto w = call->getAccount();
        auto account = w.lock();
        if (!account)
            return;

        auto ci = getConfInfoHostUri(account->getUsername() + "@server", call->getPeerNumber());
        int shareCount = 0;
        for (const auto& p : ci)
            if (p.isSharing)
                ++shareCount;
        SIP_CORE_WARN("[sharedbg] host send confInfo to %s: participants=%zu sharing=%d layout=%d",
                      call->getPeerNumber().c_str(),
                      ci.size(),
                      shareCount,
                      ci.layout);
        call->sendConfInfo(ci.toString());
    });
#endif

    auto confInfo = getConfInfoHostUri("", "");
#ifdef ENABLE_VIDEO
    createSinks(confInfo);
#endif

    {
        int shareCount = 0;
        for (const auto& p : confInfo)
            if (p.isSharing)
                ++shareCount;
        SIP_CORE_WARN("[sharedbg] host local emit confInfo: participants=%zu sharing=%d layout=%d",
                      confInfo.size(),
                      shareCount,
                      confInfo.layout);
    }
    // Inform client that layout has changed
    sip_core::emitSignal<libsip_core::CallSignal::OnConferenceInfosUpdated>(
        id_, confInfo.toVectorMapStringString());
}

void
Conference::sendVoiceActivity()
{
    // Throttle: voice activity toggles many times per second (16+/s observed),
    // and emitting a confVoiceActivity INFO per flip floods remote participants,
    // tripping SIP-server flood protection (peer dropped ~30s in). Send the first
    // change immediately for responsiveness, then coalesce subsequent flips into a
    // single trailing send that carries the latest state.
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lk(voiceActivityMutex_);
        if (voiceActivitySendPending_)
            return; // a trailing send is already scheduled; it will carry the latest state

        const auto elapsed = now - lastVoiceActivitySent_;
        if (elapsed < VOICE_ACTIVITY_MIN_INTERVAL) {
            voiceActivitySendPending_ = true;
            std::weak_ptr<Conference> w = weak_from_this();
            Manager::instance().scheduleTaskIn(
                [w] {
                    if (auto shared = w.lock())
                        shared->flushVoiceActivity();
                },
                VOICE_ACTIVITY_MIN_INTERVAL - elapsed);
            return;
        }
        lastVoiceActivitySent_ = now;
    }
    doSendVoiceActivity();
}

void
Conference::flushVoiceActivity()
{
    {
        std::lock_guard<std::mutex> lk(voiceActivityMutex_);
        voiceActivitySendPending_ = false;
        lastVoiceActivitySent_ = std::chrono::steady_clock::now();
    }
    doSendVoiceActivity();
}

void
Conference::doSendVoiceActivity()
{
    // Inform calls that voiceActivity changed
    foreachCall([&](auto call) {
        // Produce specific JSON for each participant (2 separate accounts can host ...
        // a conference on a same device, the conference is not link to one account).
        auto w = call->getAccount();
        auto account = w.lock();
        if (!account)
            return;

        // send voice activity without additional ConfInfo parameters
        call->sendVoiceActivity(voiceActivivtyToString(
            getConfInfoHostUri(account->getUsername() + "@server", call->getPeerNumber())));
    });

    auto confInfo = getConfInfoHostUri("", "");

    // Inform client that layout has changed
    sip_core::emitSignal<libsip_core::CallSignal::OnConferenceInfosUpdated>(
        id_, confInfo.toVectorMapStringString());
}

#ifdef ENABLE_VIDEO
void
Conference::createSinks(const ConfInfo& infos)
{
    std::lock_guard<std::mutex> lk(sinksMtx_);
    // Pin the mixer and take a STRONG COPY of its sink for the duration of the
    // call. getSink() returns a reference into VideoMixer::sink_; copying it
    // keeps the SinkClient control block owned here (use_count >= 2) so the
    // temporary vector's element can never become the last owner and dispatch
    // _M_dispose() through a freed control block. Defense-in-depth alongside the
    // weak_from_this()-captured mixer callback (see attachVideoMixerCallbacks()).
    auto mixer = videoMixer_;
    if (!mixer)
        return;
    auto sink = mixer->getSink();
    if (!sink)
        return;
    Manager::instance().createSinkClients(getConfId(),
                                          infos,
                                          {std::static_pointer_cast<video::VideoFrameActiveWriter>(
                                              sink)},
                                          confSinksMap_);
}
#endif

void
Conference::removeParticipant(const std::string& participant_id)
{
    SIP_CORE_DEBUG("Remove call {:s} in conference {:s}", participant_id, id_);
    // Clear the per-participant pool filters for the departing participant
    // so a follow-up 1:1 call on the same id is not affected.
    auto& rbPool = Manager::instance().getRingBufferPool();
    rbPool.setLocalPlaybackMuted(participant_id, false);
    rbPool.setMicMuted(participant_id, false);
    {
        std::lock_guard<std::mutex> lk(participantsMtx_);
        if (!participants_.erase(participant_id))
            return;
    }
    if (auto call = std::dynamic_pointer_cast<SIPCall>(getCall(participant_id))) {
        participantsMuted_.erase(call->getCallId());
        handsRaised_.erase(call->getCallId());
#ifdef ENABLE_VIDEO
        // TODO all streams
        if (videoMixer_->verifyActive(
                sip_utils::streamId(participant_id, sip_utils::DEFAULT_VIDEO_STREAMID)))
            videoMixer_->resetActiveStream();
#endif // ENABLE_VIDEO
        call->exitConference();
#ifdef ENABLE_VIDEO
        if (call->isPeerRecording())
            call->peerRecording(false);
#endif // ENABLE_VIDEO
    }
}

void
Conference::attachLocalParticipant()
{
    SIP_CORE_INFO("Attach local participant to conference %s", id_.c_str());

    if (getState() == State::ACTIVE_DETACHED) {
        setState(State::ACTIVE_ATTACHED);

        auto& rbPool = Manager::instance().getRingBufferPool();
        const bool hostMuted = isMediaSourceMuted(MediaType::MEDIA_AUDIO) or isMuted("host"sv);
        for (const auto& participant : getParticipantList()) {
            if (auto call = Manager::instance().getCallFromCallID(participant)) {
                rbPool.setMicMuted(participant, hostMuted);
                const bool participantSilenced = localPlaybackMuted_
                                                 || isMuted(call->getCallId());
                if (hostMuted and participantSilenced) {
                    // No direct audio either way; bindings are re-established
                    // by bindHost() / bindParticipant() on un-mute.
                } else if (hostMuted)
                    rbPool.bindHalfDuplexOut(RingBufferPool::DEFAULT_ID, participant);
                else if (participantSilenced)
                    rbPool.bindHalfDuplexOut(participant, RingBufferPool::DEFAULT_ID);
                else
                    rbPool.bindCallID(participant, RingBufferPool::DEFAULT_ID);
                rbPool.flush(participant);
            }

            // Reset ringbuffer's readpointers
            rbPool.flush(participant);
        }
        rbPool.flush(RingBufferPool::DEFAULT_ID);

#ifdef ENABLE_VIDEO
        if (videoMixer_) {
            std::vector<std::string> videoInputs;
            for (const auto& source : hostSources_) {
                if (source.type_ == MediaType::MEDIA_VIDEO)
                    videoInputs.emplace_back(source.sourceUri_);
            }

            videoMixer_->switchInputs(videoInputs);

            const auto hostStreamId = sip_utils::streamId("", sip_utils::DEFAULT_VIDEO_STREAMID);
            if (videoInputs.empty()) {
                // Host has no video — add placeholder so it appears in the layout
                videoMixer_->addAudioOnlySource("", hostStreamId);
            } else {
                // Host has video — remove stale audio-only placeholder if any
                videoMixer_->removeAudioOnlySource("", hostStreamId);
            }
        }
#endif
    } else {
        SIP_CORE_WARN(
            "Invalid conference state in attach participant: current \"%s\" - expected \"%s\"",
            getStateStr(),
            "ACTIVE_DETACHED");
    }
}

void
Conference::detachLocalParticipant()
{
    SIP_CORE_INFO("Detach local participant from conference %s", id_.c_str());

    if (getState() == State::ACTIVE_ATTACHED) {
        foreachCall([&](auto call) {
            Manager::instance().getRingBufferPool().unBindCallID(call->getCallId(),
                                                                 RingBufferPool::DEFAULT_ID);
        });

#ifdef ENABLE_VIDEO
        if (videoMixer_) {
            videoMixer_->stopInputs();
            // Remove local host from audio only sources when detaching
            videoMixer_
                ->removeAudioOnlySource("",
                                        sip_utils::streamId("", sip_utils::DEFAULT_VIDEO_STREAMID));
        }
#endif
    } else {
        SIP_CORE_WARN(
            "Invalid conference state in detach participant: current \"%s\" - expected \"%s\"",
            getStateStr(),
            "ACTIVE_ATTACHED");
        return;
    }

    setLocalHostDefaultMediaSource(false);
    setState(State::ACTIVE_DETACHED);
}

void
Conference::bindParticipant(const std::string& participant_id)
{
    SIP_CORE_INFO("Bind participant %s to conference %s", participant_id.c_str(), id_.c_str());

    auto& rbPool = Manager::instance().getRingBufferPool();

    for (const auto& item : getParticipantList()) {
        if (participant_id != item) {
            // Do not attach muted participants
            if (auto call = Manager::instance().getCallFromCallID(item)) {
                if (isMuted(call->getCallId()))
                    rbPool.bindHalfDuplexOut(item, participant_id);
                else
                    rbPool.bindCallID(participant_id, item);
            }
        }
        rbPool.flush(item);
    }

    // Bind local participant to other participants only if the
    // local is attached to the conference.
    if (getState() == State::ACTIVE_ATTACHED) {
        const bool hostMuted = isMediaSourceMuted(MediaType::MEDIA_AUDIO) or isMuted("host"sv);
        // Keep the data-plane mic filter consistent for (re-)bound
        // participants, including ones joining while the host is muted.
        rbPool.setMicMuted(participant_id, hostMuted);
        if (localPlaybackMuted_)
            rbPool.bindHalfDuplexOut(participant_id, RingBufferPool::DEFAULT_ID);
        else if (hostMuted)
            rbPool.bindHalfDuplexOut(RingBufferPool::DEFAULT_ID, participant_id);
        else
            rbPool.bindCallID(participant_id, RingBufferPool::DEFAULT_ID);
        rbPool.flush(RingBufferPool::DEFAULT_ID);
    }
}

void
Conference::unbindParticipant(const std::string& participant_id)
{
    SIP_CORE_INFO("Unbind participant %s from conference %s", participant_id.c_str(), id_.c_str());
    Manager::instance().getRingBufferPool().unBindAllHalfDuplexOut(participant_id);
}

void
Conference::bindHost()
{
    SIP_CORE_INFO("Bind host to conference %s", id_.c_str());

    auto& rbPool = Manager::instance().getRingBufferPool();

    for (const auto& item : getParticipantList()) {
        if (auto call = Manager::instance().getCallFromCallID(item)) {
            // Clear the data-plane mic filter set by unbindHost().
            rbPool.setMicMuted(item, false);
            if (isMuted(call->getCallId()))
                continue;
            if (localPlaybackMuted_)
                rbPool.bindHalfDuplexOut(item, RingBufferPool::DEFAULT_ID);
            else
                rbPool.bindCallID(item, RingBufferPool::DEFAULT_ID);
            rbPool.flush(RingBufferPool::DEFAULT_ID);
        }
    }
}

void
Conference::unbindHost()
{
    SIP_CORE_INFO("Unbind host from conference %s", id_.c_str());
    auto& rbPool = Manager::instance().getRingBufferPool();
    for (const auto& item : getParticipantList()) {
        // Sever the participant→mic binding directly. Iterating the
        // participant list (instead of unBindAllHalfDuplexOut(DEFAULT_ID),
        // which derives the mic readers from the host's own read bindings)
        // keeps this correct even when the bindings are asymmetric — e.g.
        // local playback muted, a moderator host-mute, or a re-bind that
        // raced a re-INVITE.
        rbPool.unBindHalfDuplexOut(item, RingBufferPool::DEFAULT_ID);
        // Race-proof data-plane mute (mirrors localPlaybackMutedIds_): even
        // if an async re-bind re-attaches the capture buffer to this reader,
        // its mix will not contain the host microphone.
        rbPool.setMicMuted(item, true);
    }
}

ParticipantSet
Conference::getParticipantList() const
{
    std::lock_guard<std::mutex> lk(participantsMtx_);
    return participants_;
}

bool
Conference::toggleRecording()
{
    bool newState = not isRecording();
    if (newState)
        initRecorder(recorder_);
    else if (recorder_)
        deinitRecorder(recorder_);

    // Notify each participant
    foreachCall([&](auto call) { call->updateRecState(newState); });

    std::time_t t = std::time(nullptr);
    auto recTime = std::localtime(&t);
    char time[20];
    strftime(time, 20, "%Y-%m-%d %H-%M-%S", recTime);
    auto filename = fmt::format("{} Conference [id {}]", time, getConfId());
    SIP_CORE_INFO() << "Recording conference to filename -> " << filename;
    setRecordingFilename(filename);

    auto res = Recordable::toggleRecording();
    updateRecording();
    return res;
}

std::string
Conference::getAccountId() const
{
    if (auto account = getAccount())
        return account->getAccountID();
    return {};
}

bool
Conference::switchInput(const std::string& input)
{
#ifdef ENABLE_VIDEO
    const auto normalizedInput = video::normalizeVideoSwitchSource(input);
    SIP_CORE_DEBUG("[Conf:{:s}] Setting video input to {:s}", id_, normalizedInput);
    if (!video::isValidVideoSwitchSource(
            normalizedInput,
            Manager::instance().getVideoManager().videoDeviceMonitor.getDeviceList())) {
        reportMediaNegotiationStatus(libsip_core::Media::MediaNegotiationStatusEvents::NEGOTIATION_FAIL);
        return false;
    }

    std::vector<MediaAttribute> newSources;
    auto firstVideo = true;
    // Rewrite hostSources (remove all except one video input)
    // This method is replaced by requestMediaChange
    for (auto& source : hostSources_) {
        if (source.type_ == MediaType::MEDIA_VIDEO) {
            if (firstVideo) {
                firstVideo = false;
                source.sourceUri_ = normalizedInput;
                newSources.emplace_back(source);
            }
        } else {
            newSources.emplace_back(source);
        }
    }

    // Done if the video is disabled
    if (not isVideoEnabled()) {
        reportMediaNegotiationStatus(libsip_core::Media::MediaNegotiationStatusEvents::NEGOTIATION_FAIL);
        return false;
    }

    if (auto mixer = videoMixer_) {
        // Pass the current mute state so that switchInputs creates
        // new source entries already muted — zero frame leak.
        mixer->switchInputs({normalizedInput},
                            isMediaSourceMuted(MediaType::MEDIA_VIDEO));
    }

    // Host screen-share: promote to ONE_BIG when the host switches its own
    // conference input to a desktop source, and restore the grid when it
    // switches away (share-stop via muting is handled by the confInfo builder).
    onShareState("", normalizedInput.rfind("display", 0) == 0);

    reportMediaNegotiationStatus();
    return true;
#endif
    return false;
}

bool
Conference::isVideoEnabled() const
{
    if (auto shared = account_.lock())
        return shared->isVideoEnabled();
    return false;
}

#ifdef ENABLE_VIDEO
std::shared_ptr<video::VideoMixer>
Conference::getVideoMixer()
{
    return videoMixer_;
}

std::string
Conference::getVideoInput() const
{
    for (const auto& source : hostSources_) {
        if (source.type_ == MediaType::MEDIA_VIDEO)
            return source.sourceUri_;
    }
    return {};
}
#endif

void
Conference::initRecorder(std::shared_ptr<MediaRecorder>& rec)
{
#ifdef ENABLE_VIDEO
    // Video
    if (videoMixer_) {
        if (auto ob = rec->addStream(videoMixer_->getStream("v:mixer"))) {
            videoMixer_->attach(ob);
        }
    }
#endif

    // Audio
    // Create ghost participant for ringbufferpool
    auto& rbPool = Manager::instance().getRingBufferPool();
    ghostRingBuffer_ = rbPool.createRingBuffer(getConfId());

    // Bind it to ringbufferpool in order to get the all mixed frames
    bindParticipant(getConfId());

    // Add stream to recorder
    audioMixer_ = sip_core::getAudioInput(getConfId());
    if (auto ob = rec->addStream(audioMixer_->getInfo("a:mixer"))) {
        audioMixer_->attach(ob);
    }
}

void
Conference::deinitRecorder(std::shared_ptr<MediaRecorder>& rec)
{
#ifdef ENABLE_VIDEO
    // Video
    if (videoMixer_) {
        if (auto ob = rec->getStream("v:mixer")) {
            videoMixer_->detach(ob);
        }
    }
#endif

    // Audio
    if (auto ob = rec->getStream("a:mixer"))
        audioMixer_->detach(ob);
    audioMixer_.reset();
    Manager::instance().getRingBufferPool().unBindAll(getConfId());
    ghostRingBuffer_.reset();
}

void
Conference::onConfOrder(const std::string& callId, const std::string& confOrder)
{
    // Check if the peer is a master
    if (auto call = getCall(callId)) {
        const auto& peerId = getRemoteId(call);
        std::string err;
        Json::Value root;
        Json::CharReaderBuilder rbuilder;
        auto reader = std::unique_ptr<Json::CharReader>(rbuilder.newCharReader());
        if (!reader->parse(confOrder.c_str(), confOrder.c_str() + confOrder.size(), &root, &err)) {
            SIP_CORE_WARN("Couldn't parse conference order from %s", peerId.c_str());
            return;
        }

        parser_.initData(std::move(root), peerId);
        parser_.parse();
    }
}

std::shared_ptr<Call>
Conference::getCall(const std::string& callId)
{
    return Manager::instance().callFactory.getCall(callId);
}

bool
Conference::isModerator(std::string_view uri) const
{
    return moderators_.find(uri) != moderators_.end() or isHost(uri);
}

bool
Conference::isHandRaised(std::string_view id) const
{
    // `id` is a host-side call id, or "host" for the local host.
    return handsRaised_.find(id) != handsRaised_.end();
}

void
Conference::setHandRaised(const std::string& accountUri,
                          const std::string& deviceId,
                          const bool& state)
{
    // Hands are keyed by the host-side call id ("host" for the local host):
    // it is the only unique participant key over plain SIP, where transport
    // device ids are always empty and peer numbers may be duplicated
    // (specs/conference-actions.md, D6).
    const auto uri = std::string(sip_utils::stripSipUriPrefix(accountUri));
    std::string key;
    if (isHost(uri)) {
        key = "host";
    } else if (auto call = getCallWith(uri, deviceId)) {
        key = call->getCallId();
    } else if (auto call = getCallFromPeerID(uri)) {
        key = call->getCallId();
    } else {
        SIP_CORE_WARN("Fail to raise %s hand (participant not found)", accountUri.c_str());
        return;
    }

    auto isPeerRequiringAttention = isHandRaised(key);
    if (state and not isPeerRequiringAttention) {
        SIP_CORE_DEBUG("Raise {:s} hand", key);
        handsRaised_.emplace(key);
        updateHandsRaised();
    } else if (not state and isPeerRequiringAttention) {
        SIP_CORE_DEBUG("Remove {:s} raised hand", key);
        handsRaised_.erase(key);
        updateHandsRaised();
    }
}

bool
Conference::isVoiceActive(std::string_view streamId) const
{
    return streamsVoiceActive.find(streamId) != streamsVoiceActive.end();
}

void
Conference::setVoiceActivity(const std::string& streamId, const bool& newState)
{
    // verify that streamID exists in conference info (local or remote-host propagated)
    bool exists = false;
    {
        std::lock_guard<std::mutex> lk(confInfoMutex_);
        auto hasSink = [&streamId](const auto& participantInfo) {
            return participantInfo.sinkId == streamId;
        };
        exists = std::any_of(confInfo_.begin(), confInfo_.end(), hasSink);
        if (!exists) {
            for (const auto& [_, remoteConfInfo] : remoteHosts_) {
                if (std::any_of(remoteConfInfo.begin(), remoteConfInfo.end(), hasSink)) {
                    exists = true;
                    break;
                }
            }
        }
    }

    if (!exists) {
        SIP_CORE_ERR("participant not found with streamId: %s", streamId.c_str());
        return;
    }

    auto previousState = isVoiceActive(streamId);

    if (previousState == newState) {
        // no change, do not send out updates
        return;
    }

    if (newState and not previousState) {
        // voice going from inactive to active
        streamsVoiceActive.emplace(streamId);
        updateVoiceActivity();
        return;
    }

    if (not newState and previousState) {
        // voice going from active to inactive
        streamsVoiceActive.erase(streamId);
        updateVoiceActivity();
        return;
    }
}

void
Conference::setVoiceActivityForCall(const std::string& callId, const bool& newState)
{
    ConfInfo confInfoSnapshot;
    {
        std::lock_guard<std::mutex> lk(confInfoMutex_);
        confInfoSnapshot = confInfo_;
    }

    std::set<std::string> sinkIds;
    auto call = getCall(callId);
    for (const auto& participantInfo : confInfoSnapshot) {
        if (participantInfo.sinkId.empty())
            continue;
        if (!participantInfo.callId.empty() && participantInfo.callId == callId) {
            sinkIds.emplace(participantInfo.sinkId);
            continue;
        }
        if (call && participantInfo.uri == call->getPeerNumber())
            sinkIds.emplace(participantInfo.sinkId);
    }

    if (sinkIds.empty()) {
        SIP_CORE_DBG("No conference participant found for callId: %s", callId.c_str());
        return;
    }

    bool needsUpdate = false;
    for (const auto& sinkId : sinkIds) {
        auto previousState = isVoiceActive(sinkId);
        if (previousState == newState)
            continue;

        if (newState)
            streamsVoiceActive.emplace(sinkId);
        else
            streamsVoiceActive.erase(sinkId);

        needsUpdate = true;
    }

    if (needsUpdate)
        updateVoiceActivity();
}

void
Conference::setVoiceActivity(const Json::Value& json)
{
    auto applyVoiceState = [this](const Json::Value& participantInfo, bool& needsUpdate) {
        if (!participantInfo.isObject() || !participantInfo.isMember("sinkId")
            || !participantInfo.isMember("state"))
            return;

        auto sinkId = participantInfo["sinkId"].asString();
        auto state = participantInfo["state"].asBool();
        if (sinkId.empty())
            return;

        bool exists = false;
        {
            std::lock_guard<std::mutex> lk(confInfoMutex_);
            auto hasSink = [&sinkId](const auto& p) {
                return p.sinkId == sinkId;
            };
            exists = std::any_of(confInfo_.begin(), confInfo_.end(), hasSink);
            if (!exists) {
                for (const auto& [_, remoteConfInfo] : remoteHosts_) {
                    if (std::any_of(remoteConfInfo.begin(), remoteConfInfo.end(), hasSink)) {
                        exists = true;
                        break;
                    }
                }
            }
        }

        if (!exists) {
            SIP_CORE_ERR("participant not found with streamId: %s", sinkId.c_str());
            return;
        }

        auto previousState = isVoiceActive(sinkId);

        if (previousState == state) {
            // no change, do not send out updates
            return;
        }

        if (state and not previousState) {
            // voice going from inactive to active
            streamsVoiceActive.emplace(sinkId);
            needsUpdate = true;
            return;
        }

        if (not state and previousState) {
            // voice going from active to inactive
            streamsVoiceActive.erase(sinkId);
            needsUpdate = true;
            return;
        }
    };

    bool needsUpdate = false;
    if (json.isArray()) {
        for (const auto& participantInfo : json)
            applyVoiceState(participantInfo, needsUpdate);
    } else if (json.isObject() && json.isMember("p") && json["p"].isArray()) {
        for (const auto& participantInfo : json["p"])
            applyVoiceState(participantInfo, needsUpdate);
    } else if (json.isObject()) {
        applyVoiceState(json, needsUpdate);
    }

    if (needsUpdate)
        updateVoiceActivity();
}

void
Conference::setVoiceInactiveHoldMs(int holdMs)
{
#ifdef ENABLE_VIDEO
    if (videoMixer_)
        videoMixer_->setVoiceInactiveHoldMs(holdMs);
#else
    (void) holdMs;
#endif
}

void
Conference::setModerator(const std::string& participant_uri, const bool& state)
{
    const auto participant_id = std::string(sip_utils::stripSipUriPrefix(participant_uri));
    for (const auto& p : getParticipantList()) {
        if (auto call = getCall(p)) {
            auto isPeerModerator = isModerator(participant_id);
            if (participant_id == getRemoteId(call)) {
                if (state and not isPeerModerator) {
                    SIP_CORE_DEBUG("Add {:s} as moderator", participant_id);
                    moderators_.emplace(participant_id);
                    updateModerators();
                } else if (not state and isPeerModerator) {
                    SIP_CORE_DEBUG("Remove {:s} as moderator", participant_id);
                    moderators_.erase(participant_id);
                    updateModerators();
                }
                return;
            }
        }
    }
    SIP_CORE_WARN("Fail to set %s as moderator (participant not found)", participant_id.c_str());
}

void
Conference::updateModerators()
{
    {
        std::lock_guard<std::mutex> lk(confInfoMutex_);
        for (auto& info : confInfo_) {
            info.isModerator = isModerator(sip_utils::stripSipUriPrefix(info.uri));
        }
    }
    // Call sendConferenceInfos() outside the lock to avoid deadlocks
    // since it iterates calls and may acquire other locks
    sendConferenceInfos();
}

void
Conference::updateHandsRaised()
{
    {
        std::lock_guard<std::mutex> lk(confInfoMutex_);
        for (auto& info : confInfo_)
            info.handRaised = info.uri.empty() ? isHandRaised("host"sv)
                                               : isHandRaised(info.callId);
    }
    // Call sendConferenceInfos() outside the lock to avoid deadlocks
    sendConferenceInfos();
}

void
Conference::updateVoiceActivity()
{
    std::map<std::string, bool> voiceStates;
    {
        std::lock_guard<std::mutex> lk(confInfoMutex_);

        // streamId is actually sinkId
        for (ParticipantInfo& participantInfo : confInfo_) {
            bool newActivity;

            if (auto call = getCallWith(std::string(string_remove_suffix(participantInfo.uri, '@')),
                                        participantInfo.device)) {
                // if this participant is in a direct call with us
                // grab voice activity info directly from the call
                newActivity = call->hasPeerVoice();
            } else {
                // check for it
                newActivity = isVoiceActive(participantInfo.sinkId);
            }

            participantInfo.voiceActivity = newActivity;
            voiceStates[participantInfo.sinkId] = participantInfo.voiceActivity;
        }

        for (auto& [_, remoteConfInfo] : remoteHosts_) {
            for (auto& participantInfo : remoteConfInfo) {
                participantInfo.voiceActivity = isVoiceActive(participantInfo.sinkId);
                voiceStates[participantInfo.sinkId] = participantInfo.voiceActivity;
            }
        }
    }
    // NOTE: All operations below are done OUTSIDE the confInfoMutex_ lock
    // to avoid deadlocks with video mixer and call mutexes

    if (videoMixer_)
        videoMixer_->setVoiceActivity(std::move(voiceStates));

    // sendVoiceActivity() iterates calls and emits signals, do NOT hold confInfoMutex_
    sendVoiceActivity();
}

void
Conference::foreachCall(const std::function<void(const std::shared_ptr<Call>& call)>& cb)
{
    for (const auto& p : getParticipantList())
        if (auto call = getCall(p))
            cb(call);
}

bool
Conference::isMuted(std::string_view callId) const
{
    return participantsMuted_.find(callId) != participantsMuted_.end();
}

void
Conference::muteStream(const std::string& accountUri,
                       const std::string& deviceId,
                       const std::string& streamId,
                       const bool& state)
{
    if (auto acc = std::dynamic_pointer_cast<SIPAccount>(account_.lock())) {
        const auto uri = std::string(sip_utils::stripSipUriPrefix(accountUri));
        if (uri == acc->getUsername()
            || (uri.empty() && streamId.rfind("host_", 0) == 0)) {
            muteHost(state);
            return;
        }
        // Participant streams are "<callId>_<label>" — the call id embedded
        // in the stream id is the only unique addressing over plain SIP
        // (device ids are empty, peer numbers may be duplicated, D7).
        for (const auto& p : getParticipantList()) {
            if (!streamId.empty() && streamId.rfind(p + "_", 0) == 0) {
                muteCall(p, state);
                return;
            }
        }
        if (auto call = getCallWith(uri, deviceId)) {
            muteCall(call->getCallId(), state);
        } else if (auto call = getCallFromPeerID(uri)) {
            muteCall(call->getCallId(), state);
        } else {
            SIP_CORE_WARN("No call with %s - %s", accountUri.c_str(), deviceId.c_str());
        }
    }
}

void
Conference::muteHost(bool state)
{
    auto isHostMuted = isMuted("host"sv);
    if (state and not isHostMuted) {
        participantsMuted_.emplace("host"sv);
        if (not isMediaSourceMuted(MediaType::MEDIA_AUDIO)) {
            SIP_CORE_DBG("Mute host");
            unbindHost();
        } else {
            // Bindings already severed by muteLocalHost(); make sure the
            // data-plane mic filter is set regardless.
            auto& rbPool = Manager::instance().getRingBufferPool();
            for (const auto& item : getParticipantList())
                rbPool.setMicMuted(item, true);
        }
    } else if (not state and isHostMuted) {
        participantsMuted_.erase("host");
        if (not isMediaSourceMuted(MediaType::MEDIA_AUDIO)) {
            SIP_CORE_DBG("Unmute host");
            bindHost();
        }
        // When the media source is still muted, keep the filter set; it is
        // cleared by bindHost() once muteLocalHost(false) runs.
    }
    updateMuted();
}

void
Conference::muteCall(const std::string& callId, bool state)
{
    auto isPartMuted = isMuted(callId);
    if (state and not isPartMuted) {
        SIP_CORE_DEBUG("Mute participant {:s}", callId);
        participantsMuted_.emplace(callId);
        unbindParticipant(callId);
        updateMuted();
    } else if (not state and isPartMuted) {
        SIP_CORE_DEBUG("Unmute participant {:s}", callId);
        participantsMuted_.erase(callId);
        bindParticipant(callId);
        updateMuted();
    }
}

void
Conference::muteLocalPlayback(bool muted)
{
    if (localPlaybackMuted_ == muted) {
        SIP_CORE_DEBUG("Re-applying local conference playback state %s for %s",
                       muted ? "muted" : "un-muted",
                       id_.c_str());
    } else {
        SIP_CORE_INFO("Set local conference playback to %s for %s",
                      muted ? "muted" : "un-muted",
                      id_.c_str());
    }
    localPlaybackMuted_ = muted;

    // Primary mute mechanism: tell the ring buffer pool to skip these
    // participants when mixing audio for the local speaker (DEFAULT_ID).
    // This is race-proof — no async re-bind can override it.
    auto& rbPool = Manager::instance().getRingBufferPool();
    const auto participants = getParticipantList();
    for (const auto& participantId : participants)
        rbPool.setLocalPlaybackMuted(participantId, muted);

    if (getState() != State::ACTIVE_ATTACHED)
        return;

    // Secondary: also adjust bindings for correctness when unmuting.
    const bool hostAudioMuted = isMediaSourceMuted(MediaType::MEDIA_AUDIO) or isMuted("host"sv);
    for (const auto& participantId : participants) {
        // Authoritatively recompute the data-plane mic filter on every
        // transition; the bindings below are a routing optimization only.
        rbPool.setMicMuted(participantId, hostAudioMuted);
        rbPool.unBindHalfDuplexOut(RingBufferPool::DEFAULT_ID, participantId);
        if (!muted && !isMuted(participantId)) {
            if (hostAudioMuted)
                rbPool.bindHalfDuplexOut(RingBufferPool::DEFAULT_ID, participantId);
            else
                rbPool.bindCallID(participantId, RingBufferPool::DEFAULT_ID);
        }

        rbPool.flush(participantId);
    }

    rbPool.flush(RingBufferPool::DEFAULT_ID);
}

void
Conference::muteParticipant(const std::string& participant_id, const bool& state)
{
    // Prioritize remote mute, otherwise the mute info is lost during
    // the conference merge (we don't send back info to remoteHost,
    // cf. getConfInfoHostUri method)

    // Transfert remote participant mute
    auto remoteHost = findHostforRemoteParticipant(participant_id);
    if (not remoteHost.empty()) {
        if (auto call = getCallFromPeerID(string_remove_suffix(remoteHost, '@'))) {
            auto w = call->getAccount();
            auto account = w.lock();
            if (!account)
                return;
            Json::Value root;
            root["muteParticipant"] = participant_id;
            root["muteState"] = state ? TRUE_STR : FALSE_STR;
            call->sendConfOrder(root);
            return;
        }
    }

    // NOTE: For now we only have one audio per call, and no way to only
    // mute one stream
    if (isHost(participant_id))
        muteHost(state);
    else if (auto call = getCallFromPeerID(participant_id))
        muteCall(call->getCallId(), state);
}

void
Conference::updateRecording()
{
    {
        std::lock_guard<std::mutex> lk(confInfoMutex_);
        for (auto& info : confInfo_) {
            if (info.uri.empty()) {
                info.recording = isRecording();
            } else if (auto call = getCallWith(std::string(string_remove_suffix(info.uri, '@')),
                                               info.device)) {
                info.recording = call->isPeerRecording();
            }
        }
    }
    // Call sendConferenceInfos() outside the lock to avoid deadlocks
    sendConferenceInfos();
}

void
Conference::updateMuted()
{
    // Collect mute state from call objects OUTSIDE confInfoMutex_ to avoid
    // deadlocks with callMutex_ (getCallWith/isPeerMuted may interact with
    // call-level locks that are also acquired by sendConferenceInfos path).
    struct MuteState {
        std::string uri;   // stripped, without '@'
        std::string device;
        std::string callId;
        bool audioModeratorMuted {false};
        bool audioLocalMuted {false};
    };
    std::vector<MuteState> callStates;

    // Step 1: snapshot URI/device pairs under the lock (cheap, no call access)
    {
        std::lock_guard<std::mutex> lk(confInfoMutex_);
        for (const auto& info : confInfo_) {
            if (!info.uri.empty()) {
                callStates.push_back(
                    {std::string(string_remove_suffix(info.uri, '@')), info.device, {}, false, false});
            }
        }
    }

    // Step 2: query call objects outside the lock
    for (auto& st : callStates) {
        if (auto call = getCallWith(st.uri, st.device)) {
            st.callId = call->getCallId();
            st.audioModeratorMuted = isMuted(st.callId);
            st.audioLocalMuted = call->isPeerMuted();
        }
    }

    // Step 3: apply collected data back under the lock
    {
        std::lock_guard<std::mutex> lk(confInfoMutex_);
        for (auto& info : confInfo_) {
            if (info.uri.empty()) {
                info.audioModeratorMuted = isMuted("host"sv);
                info.audioLocalMuted = isMediaSourceMuted(MediaType::MEDIA_AUDIO);
            } else {
                auto stripped = std::string(string_remove_suffix(info.uri, '@'));
                auto it = std::find_if(callStates.begin(), callStates.end(),
                    [&](const MuteState& s) {
                        return s.uri == stripped && s.device == info.device;
                    });
                if (it != callStates.end() && !it->callId.empty()) {
                    info.audioModeratorMuted = it->audioModeratorMuted;
                    info.audioLocalMuted = it->audioLocalMuted;
                }
            }
        }
    }
    // Call sendConferenceInfos() outside the lock to avoid deadlocks
    sendConferenceInfos();
}

ConfInfo
Conference::getConfInfoHostUri(std::string_view localHostURI, std::string_view destURI)
{
    std::lock_guard<std::mutex> lk(confInfoMutex_);
    ConfInfo newInfo = confInfo_;

    for (auto it = newInfo.begin(); it != newInfo.end();) {
        bool isRemoteHost = remoteHosts_.find(it->uri) != remoteHosts_.end();
        if (it->uri.empty() and not destURI.empty()) {
            // fill the empty uri with the local host URI, let void for local client
            it->uri = localHostURI;
        }
        if (isRemoteHost) {
            // Don't send back the ParticipantInfo for remote Host
            // For other than remote Host, the new info is in remoteHosts_
            it = newInfo.erase(it);
        } else {
            ++it;
        }
    }
    // Add remote Host info
    for (const auto& [hostUri, confInfo] : remoteHosts_) {
        // Add remote info for remote host destination
        // Example: ConfA, ConfB & ConfC
        // ConfA send ConfA and ConfB for ConfC
        // ConfA send ConfA and ConfC for ConfB
        // ...
        if (destURI != hostUri)
            newInfo.insert(newInfo.end(), confInfo.begin(), confInfo.end());
    }
    return newInfo;
}

std::string
Conference::voiceActivivtyToString(const ConfInfo& confInfo)
{
    Json::Value val = {};
    for (const auto& part : confInfo) {
        Json::Value p;
        p["uri"] = part.uri;
        p["sinkId"] = part.sinkId;
        p["state"] = part.voiceActivity;

        val.append(p);
    }

    return Json::writeString(Json::StreamWriterBuilder {}, val);
}

bool
Conference::isHost(std::string_view uri) const
{
    if (uri.empty())
        return true;

    // Check if the URI is a local URI (AccountID) for at least one of the subcall
    // (a local URI can be in the call with another device)
    for (const auto& p : getParticipantList()) {
        if (auto call = getCall(p)) {
            if (auto account = call->getAccount().lock()) {
                if (account->getUsername() == uri)
                    return true;
            }
        }
    }
    return false;
}

void
Conference::updateConferenceInfo(ConfInfo confInfo)
{
    {
        std::lock_guard<std::mutex> lk(confInfoMutex_);
        confInfo_ = std::move(confInfo);
    }
    // Call sendConferenceInfos() outside the lock to avoid deadlocks
    sendConferenceInfos();
}

void
Conference::hangupParticipant(const std::string& participantUri, const std::string& deviceId)
{
    if (auto acc = std::dynamic_pointer_cast<SIPAccount>(account_.lock())) {
        const auto accountUri = std::string(sip_utils::stripSipUriPrefix(participantUri));
        if (deviceId.empty()) {
            // If deviceId is empty, hangup all calls with device
            while (auto call = getCallFromPeerID(accountUri)) {
                Manager::instance().hangupCall(acc->getAccountID(), call->getCallId());
            }
            return;
        } else {
            if (accountUri == acc->getUsername()) {
                Manager::instance().detachLocalParticipant(shared_from_this());
                return;
            } else if (auto call = getCallWith(accountUri, deviceId)) {
                Manager::instance().hangupCall(acc->getAccountID(), call->getCallId());
                return;
            }
        }
        // Else, it may be a remote host
        auto remoteHost = findHostforRemoteParticipant(accountUri, deviceId);
        if (remoteHost.empty()) {
            SIP_CORE_WARN("Can't hangup %s, peer not found", accountUri.c_str());
            return;
        }
        if (auto call = getCallFromPeerID(string_remove_suffix(remoteHost, '@'))) {
            // Forward to the remote host.
            libsip_core::hangupParticipant(acc->getAccountID(),
                                           call->getCallId(),
                                           accountUri,
                                           deviceId);
        }
    }
}

void
Conference::muteLocalHost(bool is_muted, const std::string& mediaType)
{
    if (mediaType.compare(libsip_core::Media::Details::MEDIA_TYPE_AUDIO) == 0) {
        const bool attached = getState() == State::ACTIVE_ATTACHED;
        if (attached and is_muted == isMediaSourceMuted(MediaType::MEDIA_AUDIO)) {
            SIP_CORE_DEBUG("Local audio source already in [{:s}] state",
                           is_muted ? "muted" : "un-muted");
            return;
        }

        auto isHostMuted = isMuted("host"sv);
        if (attached and not isHostMuted) {
            if (is_muted) {
                SIP_CORE_DBG("Muting local audio source");
                unbindHost();
            } else {
                SIP_CORE_DBG("Un-muting local audio source");
                bindHost();
            }
        } else if (not attached) {
            // Not attached (e.g. mute requested between ConferenceCreated and
            // attachLocalParticipant): there are no host bindings to adjust,
            // but record the data-plane filter so the mute survives the
            // attach regardless of the bindings it sets up.
            auto& rbPool = Manager::instance().getRingBufferPool();
            for (const auto& item : getParticipantList())
                rbPool.setMicMuted(item, is_muted);
        }
        setLocalHostMuteState(MediaType::MEDIA_AUDIO, is_muted);
        updateMuted();
        emitSignal<libsip_core::CallSignal::AudioMuted>(id_, is_muted);
        return;
    } else if (mediaType.compare(libsip_core::Media::Details::MEDIA_TYPE_VIDEO) == 0) {
#ifdef ENABLE_VIDEO
        if (not isVideoEnabled()) {
            SIP_CORE_ERR("Cant't mute, the video is disabled!");
            return;
        }

        if (is_muted == isMediaSourceMuted(MediaType::MEDIA_VIDEO)) {
            SIP_CORE_DEBUG("Local video source already in [{:s}] state",
                           is_muted ? "muted" : "un-muted");
            return;
        }
        setLocalHostMuteState(MediaType::MEDIA_VIDEO, is_muted);
        if (is_muted) {
            if (auto mixer = videoMixer_) {
                SIP_CORE_DBG("Muting local video sources");
                mixer->muteInputs(true);
                // No audio-only placeholder needed here: the muted video
                // source stays in sources_ and already renders black frames.
            }
        } else {
            if (auto mixer = videoMixer_) {
                SIP_CORE_DBG("Un-muting local video sources");
                mixer->muteInputs(false);
                // Remove the host audio-only placeholder since the real video
                // source is now rendering.
                mixer->removeAudioOnlySource(
                    "",
                    sip_utils::streamId("", sip_utils::DEFAULT_VIDEO_STREAMID));
            }
        }
        emitSignal<libsip_core::CallSignal::VideoMuted>(id_, is_muted);
        return;
#endif
    }
}

#ifdef ENABLE_VIDEO
void
Conference::resizeRemoteParticipants(ConfInfo& confInfo, std::string_view peerURI)
{
    int remoteFrameHeight = confInfo.h;
    int remoteFrameWidth = confInfo.w;

    if (remoteFrameHeight == 0 or remoteFrameWidth == 0) {
        // get the size of the remote frame from receiveThread
        // if the one from confInfo is empty
        if (auto call = std::dynamic_pointer_cast<SIPCall>(
                getCallFromPeerID(string_remove_suffix(peerURI, '@')))) {
            for (auto const& videoRtp : call->getRtpSessionList(MediaType::MEDIA_VIDEO)) {
                auto recv = std::static_pointer_cast<video::VideoRtpSession>(videoRtp)
                                ->getVideoReceive();
                remoteFrameHeight = recv->getHeight();
                remoteFrameWidth = recv->getWidth();
                // NOTE: this may be not the behavior we want, but this is only called
                // when we receive conferences informations from a call, so the peer is
                // mixing the video and send only one stream, so we can break here
                break;
            }
        }
    }

    if (remoteFrameHeight == 0 or remoteFrameWidth == 0) {
        SIP_CORE_WARN("Remote frame size not found.");
        return;
    }

    // get the size of the local frame
    ParticipantInfo localCell;
    {
        std::lock_guard<std::mutex> lk(confInfoMutex_);
        for (const auto& p : confInfo_) {
            if (p.uri == peerURI) {
                localCell = p;
                break;
            }
        }
    }

    const float zoomX = (float) remoteFrameWidth / localCell.w;
    const float zoomY = (float) remoteFrameHeight / localCell.h;
    // Do the resize for each remote participant
    for (auto& remoteCell : confInfo) {
        remoteCell.x = remoteCell.x / zoomX + localCell.x;
        remoteCell.y = remoteCell.y / zoomY + localCell.y;
        remoteCell.w = remoteCell.w / zoomX;
        remoteCell.h = remoteCell.h / zoomY;
    }
}
#endif

void
Conference::mergeConfInfo(ConfInfo& newInfo, const std::string& peerURI)
{
    if (newInfo.empty()) {
        SIP_CORE_DBG("confInfo empty, remove remoteHost");
        {
            std::lock_guard<std::mutex> lk(confInfoMutex_);
            remoteHosts_.erase(peerURI);
        }
        // Call sendConferenceInfos() outside the lock to avoid deadlocks
        sendConferenceInfos();
        return;
    }

#ifdef ENABLE_VIDEO
    resizeRemoteParticipants(newInfo, peerURI);
#endif

    bool updateNeeded = false;
    {
        std::lock_guard<std::mutex> lk(confInfoMutex_);
        auto it = remoteHosts_.find(peerURI);
        if (it != remoteHosts_.end()) {
            // Compare confInfo before update
            if (it->second != newInfo) {
                it->second = newInfo;
                updateNeeded = true;
            } else
                SIP_CORE_WARN("No change in confInfo, don't update");
        } else {
            remoteHosts_.emplace(peerURI, newInfo);
            updateNeeded = true;
        }
    }
    // Send confInfo only if needed to avoid loops
#ifdef ENABLE_VIDEO
    if (updateNeeded and videoMixer_) {
        // Trigger the layout update in the mixer because the frame resolution may
        // change from participant to conference and cause a mismatch between
        // confInfo layout and rendering layout.
        videoMixer_->updateLayout();
    }
#endif
}

std::string_view
Conference::findHostforRemoteParticipant(std::string_view uri, std::string_view deviceId)
{
    std::lock_guard<std::mutex> lk(confInfoMutex_);
    for (const auto& host : remoteHosts_) {
        for (const auto& p : host.second) {
            if (uri == string_remove_suffix(p.uri, '@') && (deviceId == "" || deviceId == p.device))
                return host.first;
        }
    }
    return "";
}

std::shared_ptr<Call>
Conference::getCallFromPeerID(std::string_view peerID)
{
    peerID = sip_utils::stripSipUriPrefix(peerID);
    for (const auto& p : getParticipantList()) {
        auto call = getCall(p);
        if (call && getRemoteId(call) == peerID) {
            return call;
        }
    }
    return nullptr;
}

std::shared_ptr<Call>
Conference::getCallWith(const std::string& accountUri, const std::string& deviceId)
{
    // Strip both sides: confInfo publishes the raw peer number (possibly a
    // full bracketed URI), so clients legitimately pass it back verbatim.
    const auto uri = sip_utils::stripSipUriPrefix(accountUri);
    for (const auto& p : getParticipantList()) {
        if (auto call = std::dynamic_pointer_cast<SIPCall>(getCall(p))) {
            auto transport = call->getTransport();
            const auto callDeviceId = transport ? transport->deviceId() : std::string_view {};
            if (uri == sip_utils::stripSipUriPrefix(call->getPeerNumber())
                && deviceId == callDeviceId) {
                return call;
            }
        }
    }
    return {};
}

std::string
Conference::getRemoteId(const std::shared_ptr<sip_core::Call>& call) const
{
    // The peer username (the user part of the peer URI) is the conference-
    // protocol peer identity: it is what remote clients put as the account
    // uri in confOrders and what the moderator preferences contain. Returning
    // the call id here (as this used to) split the identity namespace and
    // broke every uri-addressed action (specs/conference-actions.md, D5).
    // getPeerNumber() may be a full bracketed URI ("<sip:009@dom>"), so the
    // full stripper is required, not just the @domain suffix removal.
    return std::string(sip_utils::stripSipUriPrefix(call->getPeerNumber()));
}

void
Conference::stopRecording()
{
    Recordable::stopRecording();
    updateRecording();
}

bool
Conference::startRecording(const std::string& path)
{
    auto res = Recordable::startRecording(path);
    updateRecording();
    return res;
}

int
Conference::getLayout() const
{
    std::lock_guard<std::mutex> lk(confInfoMutex_);
    return confInfo_.layout;
}

} // namespace sip_core
