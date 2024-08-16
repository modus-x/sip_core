/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
 *  Author: Pierre-Luc Beaudoin <pierre-luc.beaudoin@savoirfairelinux.com>
 *  Author: Guillaume Roguez <Guillaume.Roguez@savoirfairelinux.com>
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

#ifndef LIBSIP_CORE_CALLMANAGERI_H
#define LIBSIP_CORE_CALLMANAGERI_H

#include "def.h"

#include <stdexcept>
#include <map>
#include <memory>
#include <vector>
#include <string>
#include <cstdint>

#include "sip_core.h"
#include "pjsip-simple/evsub.h"

namespace libsip_core {

[[deprecated("Replaced by registerSignalHandlers")]] LIBSIP_CORE_PUBLIC void registerCallHandlers(
    const std::map<std::string, std::shared_ptr<CallbackWrapperBase>>&);

/* Call related methods */
LIBSIP_CORE_PUBLIC std::string placeCall(const std::string& accountId, const std::string& to);

LIBSIP_CORE_PUBLIC std::string placeCallWithMedia(
    const std::string& accountId,
    const std::string& to,
    const std::vector<std::map<std::string, std::string>>& mediaList);
LIBSIP_CORE_PUBLIC bool refuse(const std::string& accountId, const std::string& callId);
LIBSIP_CORE_PUBLIC bool accept(const std::string& accountId, const std::string& callId);
LIBSIP_CORE_PUBLIC bool hangUp(const std::string& accountId, const std::string& callId);
LIBSIP_CORE_PUBLIC bool hold(const std::string& accountId, const std::string& callId);
LIBSIP_CORE_PUBLIC bool unhold(const std::string& accountId, const std::string& callId);
LIBSIP_CORE_PUBLIC bool muteLocalMedia(const std::string& accountId,
                                       const std::string& callId,
                                       const std::string& mediaType,
                                       bool mute);
LIBSIP_CORE_PUBLIC bool muteRemoteMedia(const std::string& accountId,
                                       const std::string& callId,
                                       const std::string& mediaType,
                                       bool mute);
LIBSIP_CORE_PUBLIC bool transfer(const std::string& accountId,
                                 const std::string& callId,
                                 const std::string& to);
LIBSIP_CORE_PUBLIC bool attendedTransfer(const std::string& accountId,
                                         const std::string& callId,
                                         const std::string& targetID);
LIBSIP_CORE_PUBLIC std::map<std::string, std::string> getCallDetails(const std::string& accountId,
                                                                     const std::string& callId);
LIBSIP_CORE_PUBLIC std::vector<std::string> getCallList(const std::string& accountId);

/* APIs that supports an arbitrary number of media */
LIBSIP_CORE_PUBLIC bool acceptWithMedia(const std::string& accountId,
                                        const std::string& callId,
                                        const std::vector<libsip_core::MediaMap>& mediaList);
LIBSIP_CORE_PUBLIC bool requestMediaChange(const std::string& accountId,
                                           const std::string& callId,
                                           const std::vector<libsip_core::MediaMap>& mediaList);

/**
 * Answer a media change request
 * @param accountId
 * @param callId
 * @param mediaList the list of media attributes. The client can
 * control the media through the attributes. The list should have
 * the same size as the list reported in the media change request.
 * The client can ignore the media update request by not calling this
 * method, or calling it with an empty media list.
 */
LIBSIP_CORE_PUBLIC bool answerMediaChangeRequest(const std::string& accountId,
                                                 const std::string& callId,
                                                 const std::vector<libsip_core::MediaMap>& mediaList);

/* Conference related methods */
LIBSIP_CORE_PUBLIC bool joinParticipant(const std::string& accountId,
                                        const std::string& sel_callId,
                                        const std::string& account2Id,
                                        const std::string& drag_callId,
                                        bool attached);

LIBSIP_CORE_PUBLIC void createConfFromParticipantList(const std::string& accountId,
                                                      const std::vector<std::string>& participants);
LIBSIP_CORE_PUBLIC void setConferenceLayout(const std::string& accountId,
                                            const std::string& confId,
                                            uint32_t layout);
LIBSIP_CORE_PUBLIC bool isConferenceParticipant(const std::string& accountId,
                                                const std::string& callId);
LIBSIP_CORE_PUBLIC bool addParticipant(const std::string& accountId,
                                       const std::string& callId,
                                       const std::string& account2Id,
                                       const std::string& confId);
LIBSIP_CORE_PUBLIC bool addMainParticipant(const std::string& accountId, const std::string& confId);
LIBSIP_CORE_PUBLIC bool detachLocalParticipant();
LIBSIP_CORE_PUBLIC bool detachParticipant(const std::string& accountId, const std::string& callId);
LIBSIP_CORE_PUBLIC bool joinConference(const std::string& accountId,
                                       const std::string& sel_confId,
                                       const std::string& account2Id,
                                       const std::string& drag_confId);
LIBSIP_CORE_PUBLIC bool hangUpConference(const std::string& accountId, const std::string& confId);
LIBSIP_CORE_PUBLIC bool holdConference(const std::string& accountId, const std::string& confId);
LIBSIP_CORE_PUBLIC bool unholdConference(const std::string& accountId, const std::string& confId);
LIBSIP_CORE_PUBLIC std::vector<std::string> getConferenceList(const std::string& accountId);
LIBSIP_CORE_PUBLIC std::vector<std::string> getParticipantList(const std::string& accountId,
                                                               const std::string& confId);
LIBSIP_CORE_PUBLIC std::string getConferenceId(const std::string& accountId,
                                               const std::string& callId);
LIBSIP_CORE_PUBLIC std::map<std::string, std::string> getConferenceDetails(
    const std::string& accountId, const std::string& callId);
LIBSIP_CORE_PUBLIC std::vector<libsip_core::MediaMap> currentMediaList(const std::string& accountId,
                                                                       const std::string& callId);
LIBSIP_CORE_PUBLIC std::vector<std::map<std::string, std::string>> getConferenceInfos(
    const std::string& accountId, const std::string& confId);
LIBSIP_CORE_PUBLIC void setModerator(const std::string& accountId,
                                     const std::string& confId,
                                     const std::string& accountUri,
                                     const bool& state);
/// DEPRECATED USE muteStream
LIBSIP_CORE_PUBLIC void muteParticipant(const std::string& accountId,
                                        const std::string& confId,
                                        const std::string& accountUri,
                                        const bool& state);
// Note: muting Audio not supported yet
LIBSIP_CORE_PUBLIC void muteStream(const std::string& accountId,
                                   const std::string& confId,
                                   const std::string& accountUri,
                                   const std::string& deviceId,
                                   const std::string& streamId,
                                   const bool& state);
/// DEPRECATED, USE setActiveStream
LIBSIP_CORE_PUBLIC void setActiveParticipant(const std::string& accountId,
                                             const std::string& confId,
                                             const std::string& callId);
LIBSIP_CORE_PUBLIC void setActiveStream(const std::string& accountId,
                                        const std::string& confId,
                                        const std::string& accountUri,
                                        const std::string& deviceId,
                                        const std::string& streamId,
                                        const bool& state);
LIBSIP_CORE_PUBLIC void hangupParticipant(const std::string& accountId,
                                          const std::string& confId,
                                          const std::string& accountUri,
                                          const std::string& deviceId);
/// DEPRECATED, use raiseHand
LIBSIP_CORE_PUBLIC void raiseParticipantHand(const std::string& accountId,
                                             const std::string& confId,
                                             const std::string& peerId,
                                             const bool& state);
LIBSIP_CORE_PUBLIC void raiseHand(const std::string& accountId,
                                  const std::string& confId,
                                  const std::string& accountUri,
                                  const std::string& deviceId,
                                  const bool& state);

/* Statistic related methods */
LIBSIP_CORE_PUBLIC void startSmartInfo(uint32_t refreshTimeMs);
LIBSIP_CORE_PUBLIC void stopSmartInfo();

/* File Playback methods */
LIBSIP_CORE_PUBLIC bool startRecordedFilePlayback(const std::string& filepath);
LIBSIP_CORE_PUBLIC void stopRecordedFilePlayback();

/* General audio methods */
LIBSIP_CORE_PUBLIC bool toggleRecording(const std::string& accountId, const std::string& callId);
/* DEPRECATED */
LIBSIP_CORE_PUBLIC void setRecording(const std::string& accountId, const std::string& callId);

LIBSIP_CORE_PUBLIC void recordPlaybackSeek(double value);
LIBSIP_CORE_PUBLIC bool getIsRecording(const std::string& accountId, const std::string& callId);
LIBSIP_CORE_PUBLIC void playDTMF(const std::string& accountId, const std::string& callId, const std::string& dtmfEvents);
LIBSIP_CORE_PUBLIC void startTone(int32_t start, int32_t type);

LIBSIP_CORE_PUBLIC bool switchInput(const std::string& accountId,
                                    const std::string& callId,
                                    const std::string& resource);
LIBSIP_CORE_PUBLIC bool switchSecondaryInput(const std::string& accountId,
                                             const std::string& confId,
                                             const std::string& resource);

/* Instant messaging */
LIBSIP_CORE_PUBLIC void sendTextMessage(const std::string& accountId,
                                        const std::string& callId,
                                        const std::map<std::string, std::string>& messages,
                                        const std::string& from,
                                        bool isMixed);

// Call signal type definitions
struct LIBSIP_CORE_PUBLIC CallSignal
{
    struct LIBSIP_CORE_PUBLIC VideoSenderNatResolved
    {
        constexpr static const char* name = "VideoSenderNatResolved";
        using cb_type = void(const std::string&);
    };
    struct LIBSIP_CORE_PUBLIC StateChange
    {
        constexpr static const char* name = "StateChange";
        using cb_type = void(const std::string&, const std::string&, const std::string&, int);
    };
    struct LIBSIP_CORE_PUBLIC TransferStateChange
    {
        constexpr static const char* name = "TransferStateChange";
        using cb_type = void(const std::string&, const std::string&, const pjsip_evsub_state, int, const std::string&);
    };
    struct LIBSIP_CORE_PUBLIC TransferFailed
    {
        constexpr static const char* name = "TransferFailed";
        using cb_type = void(void);
    };
    struct LIBSIP_CORE_PUBLIC TransferSucceeded
    {
        constexpr static const char* name = "TransferSucceeded";
        using cb_type = void(void);
    };
    struct LIBSIP_CORE_PUBLIC RecordPlaybackStopped
    {
        constexpr static const char* name = "RecordPlaybackStopped";
        using cb_type = void(const std::string&);
    };
    struct LIBSIP_CORE_PUBLIC VoiceMailNotify
    {
        constexpr static const char* name = "VoiceMailNotify";
        using cb_type = void(const std::string&, int32_t, int32_t, int32_t);
    };
    struct LIBSIP_CORE_PUBLIC IncomingMessage
    {
        constexpr static const char* name = "IncomingMessage";
        using cb_type = void(const std::string&,
                             const std::string&,
                             const std::string&,
                             const std::map<std::string, std::string>&);
    };
    struct LIBSIP_CORE_PUBLIC IncomingCall
    {
        constexpr static const char* name = "IncomingCall";
        using cb_type = void(const std::string&, const std::string&, const std::string&);
    };
    struct LIBSIP_CORE_PUBLIC IncomingCallWithMedia
    {
        constexpr static const char* name = "IncomingCallWithMedia";
        using cb_type = void(const std::string&,
                             const std::string&,
                             const std::string&,
                             const std::vector<std::map<std::string, std::string>>&,
                             const std::map<std::string, std::string>&);
    };
    struct LIBSIP_CORE_PUBLIC MediaChangeRequested
    {
        constexpr static const char* name = "MediaChangeRequested";
        using cb_type = void(const std::string&,
                             const std::string&,
                             const std::vector<std::map<std::string, std::string>>&);
    };
    struct LIBSIP_CORE_PUBLIC RecordPlaybackFilepath
    {
        constexpr static const char* name = "RecordPlaybackFilepath";
        using cb_type = void(const std::string&, const std::string&);
    };
    struct LIBSIP_CORE_PUBLIC ConferenceCreated
    {
        constexpr static const char* name = "ConferenceCreated";
        using cb_type = void(const std::string&, const std::string&);
    };
    struct LIBSIP_CORE_PUBLIC ConferenceChanged
    {
        constexpr static const char* name = "ConferenceChanged";
        using cb_type = void(const std::string&, const std::string&, const std::string&);
    };
    struct LIBSIP_CORE_PUBLIC UpdatePlaybackScale
    {
        constexpr static const char* name = "UpdatePlaybackScale";
        using cb_type = void(const std::string&, unsigned, unsigned);
    };
    struct LIBSIP_CORE_PUBLIC ConferenceRemoved
    {
        constexpr static const char* name = "ConferenceRemoved";
        using cb_type = void(const std::string&, const std::string&);
    };
    struct LIBSIP_CORE_PUBLIC RecordingStateChanged
    {
        constexpr static const char* name = "RecordingStateChanged";
        using cb_type = void(const std::string&, int);
    };
    struct LIBSIP_CORE_PUBLIC RtcpReportReceived
    {
        constexpr static const char* name = "RtcpReportReceived";
        using cb_type = void(const std::string&, const std::map<std::string, int>&);
    };
    struct LIBSIP_CORE_PUBLIC PeerHold
    {
        constexpr static const char* name = "PeerHold";
        using cb_type = void(const std::string&, bool);
    };
    struct LIBSIP_CORE_PUBLIC VideoMuted
    {
        constexpr static const char* name = "VideoMuted";
        using cb_type = void(const std::string&, bool);
    };
    struct LIBSIP_CORE_PUBLIC AudioMuted
    {
        constexpr static const char* name = "AudioMuted";
        using cb_type = void(const std::string&, bool);
    };
    struct LIBSIP_CORE_PUBLIC PeerMuted
    {
        constexpr static const char* name = "PeerMuted";
        using cb_type = void(const std::string&, bool);
    };
    struct LIBSIP_CORE_PUBLIC SmartInfo
    {
        constexpr static const char* name = "SmartInfo";
        using cb_type = void(const std::map<std::string, std::string>&);
    };
    struct LIBSIP_CORE_PUBLIC ConnectionUpdate
    {
        constexpr static const char* name = "ConnectionUpdate";
        using cb_type = void(const std::string&, int);
    };
    struct LIBSIP_CORE_PUBLIC OnConferenceInfosUpdated
    {
        constexpr static const char* name = "OnConferenceInfosUpdated";
        using cb_type = void(const std::string&,
                             const std::vector<std::map<std::string, std::string>>&);
    };
    struct LIBSIP_CORE_PUBLIC RemoteRecordingChanged
    {
        constexpr static const char* name = "RemoteRecordingChanged";
        using cb_type = void(const std::string&, const std::string&, bool);
    };
    // Report media negotiation status
    struct LIBSIP_CORE_PUBLIC MediaNegotiationStatus
    {
        constexpr static const char* name = "MediaNegotiationStatus";
        using cb_type = void(const std::string&,
                             const std::string&,
                             const std::vector<std::map<std::string, std::string>>&);
    };
};

} // namespace libsip_core

#endif // LIBSIP_CORE_CALLMANAGERI_H
