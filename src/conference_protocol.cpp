/*
 *  Copyright (C) 2022 Savoir-faire Linux Inc.
 *
 *  Author: Sébastien Blin <sebastien.blin@savoirfairelinux.com>
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

#include "conference_protocol.h"

#include "string_utils.h"

#include <cstdlib>

namespace sip_core {

namespace ProtocolKeys {

constexpr static const char* PROTOVERSION = "version";
constexpr static const char* LAYOUT = "layout";
// V0
constexpr static const char* HANDRAISED = "handRaised";
constexpr static const char* HANDSTATE = "handState";
constexpr static const char* ACTIVEPART = "activeParticipant";
constexpr static const char* MUTEPART = "muteParticipant";
constexpr static const char* MUTESTATE = "muteState";
constexpr static const char* HANGUPPART = "hangupParticipant";
// V1
constexpr static const char* DEVICES = "devices";
constexpr static const char* MEDIAS = "medias";
constexpr static const char* RAISEHAND = "raiseHand";
constexpr static const char* HANGUP = "hangup";
constexpr static const char* ACTIVE = "active";
constexpr static const char* MUTEAUDIO = "muteAudio";
// Future
constexpr static const char* MUTEVIDEO = "muteVideo";
constexpr static const char* VOICEACTIVITY = "voiceActivity";

} // namespace ProtocolKeys

namespace ConfOrder {

namespace {

Json::Value
deviceOrder(const std::string& accountUri, const std::string& deviceId, Json::Value&& deviceVal)
{
    Json::Value devices;
    devices[deviceId] = std::move(deviceVal);
    Json::Value account;
    account[ProtocolKeys::DEVICES] = std::move(devices);
    Json::Value root;
    root[accountUri] = std::move(account);
    root[ProtocolKeys::PROTOVERSION] = 1;
    return root;
}

Json::Value
mediaOrder(const std::string& accountUri,
           const std::string& deviceId,
           const std::string& streamId,
           const char* key,
           bool state)
{
    Json::Value media;
    media[key] = state;
    Json::Value medias;
    medias[streamId] = std::move(media);
    Json::Value deviceVal;
    deviceVal[ProtocolKeys::MEDIAS] = std::move(medias);
    return deviceOrder(accountUri, deviceId, std::move(deviceVal));
}

} // namespace

Json::Value
raiseHand(const std::string& accountUri, const std::string& deviceId, bool state)
{
    Json::Value deviceVal;
    deviceVal[ProtocolKeys::RAISEHAND] = state;
    return deviceOrder(accountUri, deviceId, std::move(deviceVal));
}

Json::Value
hangupParticipant(const std::string& accountUri, const std::string& deviceId)
{
    Json::Value deviceVal;
    deviceVal[ProtocolKeys::HANGUP] = TRUE_STR;
    return deviceOrder(accountUri, deviceId, std::move(deviceVal));
}

Json::Value
muteAudio(const std::string& accountUri,
          const std::string& deviceId,
          const std::string& streamId,
          bool state)
{
    return mediaOrder(accountUri, deviceId, streamId, ProtocolKeys::MUTEAUDIO, state);
}

Json::Value
setActiveStream(const std::string& accountUri,
                const std::string& deviceId,
                const std::string& streamId,
                bool state)
{
    return mediaOrder(accountUri, deviceId, streamId, ProtocolKeys::ACTIVE, state);
}

} // namespace ConfOrder

bool
isConferenceControlPayload(const std::map<std::string, std::string>& payloads)
{
    for (const auto& part : payloads) {
        const auto& mime = part.first;
        if (mime == "application/confInfo+json" || mime == "application/confOrder+json"
            || mime == "application/confVoiceActivity+json")
            return true;
    }
    return false;
}

bool
confInfoOutOfDialogEnabledFromEnv(const char* envValue)
{
    // Out-of-dialog by default; only an explicit "1" restores legacy in-dialog.
    return !(envValue && std::string_view(envValue) == "1");
}

bool
confInfoOutOfDialogEnabled()
{
    static const bool enabled = confInfoOutOfDialogEnabledFromEnv(
        std::getenv("SIP_CORE_CONFINFO_IN_DIALOG"));
    return enabled;
}

void
ConfProtocolParser::parse()
{
    if (data_.isMember(ProtocolKeys::PROTOVERSION)) {
        uint32_t version = data_[ProtocolKeys::PROTOVERSION].asUInt();
        if (version_)
            version_(version);
        if (version == 1) {
            parseV1();
        } else {
            SIP_CORE_WARN() << "Unsupported protocol version " << version;
        }
    } else {
        parseV0();
    }
}

void
ConfProtocolParser::parseV0()
{
    // checkAuthorization_ is the only hard requirement: every other handler is
    // optional and skipped individually. Requiring the full handler set here
    // used to silently drop EVERY incoming order when one optional handler was
    // left unregistered.
    if (!checkAuthorization_) {
        SIP_CORE_ERR() << "Missing checkAuthorization method for ConfProtocolParser";
        return;
    }
    auto isPeerModerator = checkAuthorization_(peerId_);
    if (data_.isMember(ProtocolKeys::HANDRAISED)) {
        auto state = data_[ProtocolKeys::HANDSTATE].asString() == TRUE_STR;
        auto uri = data_[ProtocolKeys::HANDRAISED].asString();
        if (!raiseHandUri_) {
            SIP_CORE_WARN() << "No handler for conference order key "
                            << ProtocolKeys::HANDRAISED;
        } else if (peerId_ == uri) {
            // In this case, the user want to change their state
            raiseHandUri_(uri, state);
        } else if (!state && isPeerModerator) {
            // In this case a moderator can lower the hand
            raiseHandUri_(uri, state);
        }
    }
    if (!isPeerModerator) {
        SIP_CORE_WARN("Received conference order from a non master (%.*s)",
                  (int) peerId_.size(),
                  peerId_.data());
        return;
    }
    if (setLayout_ && data_.isMember(ProtocolKeys::LAYOUT)) {
        setLayout_(data_[ProtocolKeys::LAYOUT].asInt());
    }
    if (setActiveParticipant_ && data_.isMember(ProtocolKeys::ACTIVEPART)) {
        setActiveParticipant_(data_[ProtocolKeys::ACTIVEPART].asString());
    }
    if (muteParticipant_ && data_.isMember(ProtocolKeys::MUTEPART)
        && data_.isMember(ProtocolKeys::MUTESTATE)) {
        muteParticipant_(data_[ProtocolKeys::MUTEPART].asString(),
                         data_[ProtocolKeys::MUTESTATE].asString() == TRUE_STR);
    }
    if (kickParticipant_ && data_.isMember(ProtocolKeys::HANGUPPART)) {
        kickParticipant_(data_[ProtocolKeys::HANGUPPART].asString());
    }
}

void
ConfProtocolParser::parseV1()
{
    // checkAuthorization_ is the only hard requirement (see parseV0).
    if (!checkAuthorization_) {
        SIP_CORE_ERR() << "Missing checkAuthorization method for ConfProtocolParser";
        return;
    }

    auto isPeerModerator = checkAuthorization_(peerId_);
    for (Json::Value::const_iterator itr = data_.begin(); itr != data_.end(); itr++) {
        auto key = itr.key();
        if (key == ProtocolKeys::PROTOVERSION)
            continue;
        if (isPeerModerator && key == ProtocolKeys::LAYOUT) {
            // Note: can be removed soon
            if (setLayout_)
                setLayout_(itr->asInt());
        } else {
            auto accValue = *itr;
            // Non-account scalar keys (e.g. "layout" from a non-moderator):
            // isMember() on a non-object Json::Value throws Json::LogicError.
            if (!accValue.isObject())
                continue;
            if (accValue.isMember(ProtocolKeys::DEVICES)) {
                auto accountUri = key.asString();
                for (Json::Value::const_iterator itrd = accValue[ProtocolKeys::DEVICES].begin();
                     itrd != accValue[ProtocolKeys::DEVICES].end();
                     itrd++) {
                    auto deviceId = itrd.key().asString();
                    auto deviceValue = *itrd;
                    if (!deviceValue.isObject())
                        continue;
                    if (raiseHand_ && deviceValue.isMember(ProtocolKeys::RAISEHAND)) {
                        auto newState = deviceValue[ProtocolKeys::RAISEHAND].asBool();
                        if (peerId_ == accountUri || (!newState && isPeerModerator))
                            raiseHand_(accountUri, deviceId, newState);
                    }
                    if (hangupParticipant_ && isPeerModerator
                        && deviceValue.isMember(ProtocolKeys::HANGUP)) {
                        hangupParticipant_(accountUri, deviceId);
                    }
                    if (deviceValue.isMember(ProtocolKeys::MEDIAS)) {
                        // The media actions live under THIS device's "medias"
                        // object, not under the account object.
                        const auto& medias = deviceValue[ProtocolKeys::MEDIAS];
                        for (Json::Value::const_iterator itrm = medias.begin();
                             itrm != medias.end();
                             itrm++) {
                            auto streamId = itrm.key().asString();
                            auto mediaVal = *itrm;
                            if (!mediaVal.isObject())
                                continue;
                            if (voiceActivity_
                                && mediaVal.isMember(ProtocolKeys::VOICEACTIVITY)) {
                                voiceActivity_(streamId,
                                               mediaVal[ProtocolKeys::VOICEACTIVITY].asBool());
                            }
                            if (isPeerModerator) {
                                if (muteStreamVideo_
                                    && mediaVal.isMember(ProtocolKeys::MUTEVIDEO)) {
                                    // Dispatched only once an implementation
                                    // registers the handler.
                                    muteStreamVideo_(accountUri,
                                                     deviceId,
                                                     streamId,
                                                     mediaVal[ProtocolKeys::MUTEVIDEO].asBool());
                                }
                                if (muteStreamAudio_
                                    && mediaVal.isMember(ProtocolKeys::MUTEAUDIO)) {
                                    muteStreamAudio_(accountUri,
                                                     deviceId,
                                                     streamId,
                                                     mediaVal[ProtocolKeys::MUTEAUDIO].asBool());
                                }
                                if (setActiveStream_
                                    && mediaVal.isMember(ProtocolKeys::ACTIVE)) {
                                    setActiveStream_(streamId,
                                                     mediaVal[ProtocolKeys::ACTIVE].asBool());
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

} // namespace sip_core
