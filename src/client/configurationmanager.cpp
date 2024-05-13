/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
 *  Author: Pierre-Luc Beaudoin <pierre-luc.beaudoin@savoirfairelinux.com>
 *  Author: Emmanuel Milou <emmanuel.milou@savoirfairelinux.com>
 *  Author: Guillaume Carmel-Archambault <guillaume.carmel-archambault@savoirfairelinux.com>
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "configurationmanager_interface.h"
#include "account_schema.h"
#include "manager.h"
#include "logger.h"
#include "fileutils.h"
#include "connectivity/ip_utils.h"
#include "connectivity/transport.h"
#include "sip/sipaccount.h"
#include "sip/sipaccount_config.h"
#include "audio/audiolayer.h"
#include "system_codec_container.h"
#include "client/ring_signal.h"
#include "audio/ringbufferpool.h"

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#ifdef _MSC_VER
#include "windirent.h"
#else
#include <dirent.h>
#endif

#include <cerrno>
#include <cstring>
#include <sstream>

#ifdef _WIN32
#undef interface
#endif

namespace libsip_core {

constexpr unsigned CODECS_NOT_LOADED = 0x1000; /** Codecs not found */

using sip_core::SIPAccount;
using sip_core::AudioDeviceType;

void
registerConfHandlers(const std::map<std::string, std::shared_ptr<CallbackWrapperBase>>& handlers)
{
    registerSignalHandlers(handlers);
}

void
setAutoAnswer(const std::string& accountId, bool enable)
{
    sip_core::Manager::instance().setAutoAnswer(accountId, enable);
}

std::map<std::string, std::string>
getAccountDetails(const std::string& accountID)
{
    return sip_core::Manager::instance().getAccountDetails(accountID);
}

std::map<std::string, std::string>
getVolatileAccountDetails(const std::string& accountID)
{
    return sip_core::Manager::instance().getVolatileAccountDetails(accountID);
}

bool
isInitialized()
{
    return sip_core::Manager::initialized;
}

void
setAccountDetails(const std::string& accountID, const std::map<std::string, std::string>& details)
{
    sip_core::Manager::instance().setAccountDetails(accountID, details);
}

void
setAccountActive(const std::string& accountID, bool enable, bool shutdownConnections)
{
    sip_core::Manager::instance().setAccountActive(accountID, enable, shutdownConnections);
}

void
playDigitSound(const std::string& digit)
{
    // play first
    sip_core::Manager::instance().playDtmf(*digit.c_str());
}

void
sendRegister(const std::string& accountID, bool enable)
{
    sip_core::Manager::instance().sendRegister(accountID, enable);
}

void
registerAllAccounts()
{
    sip_core::Manager::instance().registerAccounts();
}

bool
switchTransport(const std::string& accountID, Account::Transport type)
{
    switch (type) {
    case Account::Transport::UDP:
        return sip_core::Manager::instance().switchTransport(accountID,
                                                             sip_core::TransportType::UDP);
    case Account::Transport::TCP:
        return sip_core::Manager::instance().switchTransport(accountID,
                                                             sip_core::TransportType::TCP);
    default:
        return false;
    }
}

uint64_t
sendAccountTextMessage(const std::string& accountID,
                       const std::string& to,
                       const std::map<std::string, std::string>& payloads)
{
    return sip_core::Manager::instance().sendTextMessage(accountID, to, payloads);
}

std::vector<Message>
getLastMessages(const std::string& accountID, const uint64_t& base_timestamp)
{
    if (const auto acc = sip_core::Manager::instance().getAccount(accountID))
        return acc->getLastMessages(base_timestamp);
    return {};
}

int
getMessageStatus(uint64_t messageId)
{
    return sip_core::Manager::instance().getMessageStatus(messageId);
}

int
getMessageStatus(const std::string& accountID, uint64_t messageId)
{
    return sip_core::Manager::instance().getMessageStatus(accountID, messageId);
}

bool
cancelMessage(const std::string& accountID, uint64_t messageId)
{
    if (const auto acc = sip_core::Manager::instance().getAccount(accountID))
        return acc->cancelMessage(messageId);
    return {};
}

void
setIsComposing(const std::string& accountID, const std::string& conversationUri, bool isWriting)
{
    if (const auto acc = sip_core::Manager::instance().getAccount(accountID))
        acc->setIsComposing(conversationUri, isWriting);
}

bool
setMessageDisplayed(const std::string& accountID,
                    const std::string& conversationUri,
                    const std::string& messageId,
                    int status)
{
    if (const auto acc = sip_core::Manager::instance().getAccount(accountID))
        return acc->setMessageDisplayed(conversationUri, messageId, status);
    return false;
}

/// This function is used as a base for new accounts for clients that support it
std::map<std::string, std::string>
getAccountTemplate(const std::string& accountType)
{
    if (accountType == Account::ProtocolNames::SIP)
        return sip_core::SipAccountConfig().toMap();
    return {};
}

std::string
addAccount(const std::map<std::string, std::string>& details, const std::string& accountID)
{
    return sip_core::Manager::instance().addAccount(details, accountID);
}

void
monitor(bool continuous)
{
    return sip_core::Manager::instance().monitor(continuous);
}

void
removeAccount(const std::string& accountID)
{
    return sip_core::Manager::instance().removeAccount(accountID, true); // with 'flush' enabled
}

std::vector<std::string>
getAccountList()
{
    return sip_core::Manager::instance().getAccountList();
}

/**
 * Send the list of all codecs loaded to the client through DBus.
 * Can stay global, as only the active codecs will be set per accounts
 */
std::vector<unsigned>
getCodecList()
{
    std::vector<unsigned> list {
        sip_core::getSystemCodecContainer()->getSystemCodecInfoIdList(sip_core::MEDIA_ALL)};
    if (list.empty())
        sip_core::emitSignal<ConfigurationSignal::Error>(CODECS_NOT_LOADED);
    return list;
}

void
setDND(const std::string& accountID, bool isDND)
{
    auto acc = sip_core::Manager::instance().getAccount(accountID);
    if (!acc) {
        SIP_CORE_ERR("Could not find account %s. can not set codec details", accountID.c_str());
        return;
    }

    acc->setDND(isDND);
}

bool
setCodecDetails(const std::string& accountID,
                const unsigned& codecId,
                const std::map<std::string, std::string>& details)
{
    auto acc = sip_core::Manager::instance().getAccount(accountID);
    if (!acc) {
        SIP_CORE_ERR("Could not find account %s. can not set codec details", accountID.c_str());
        return false;
    }

    auto codec = acc->searchCodecById(codecId, sip_core::MEDIA_ALL);
    if (!codec) {
        SIP_CORE_ERR("can not find codec %d", codecId);
        return false;
    }
    try {
        if (codec->systemCodecInfo.mediaType & sip_core::MEDIA_AUDIO) {
            if (auto foundCodec = std::static_pointer_cast<sip_core::AccountAudioCodecInfo>(codec)) {
                foundCodec->setCodecSpecifications(details);
                sip_core::emitSignal<ConfigurationSignal::MediaParametersChanged>(accountID);
                return true;
            }
        }

        if (codec->systemCodecInfo.mediaType & sip_core::MEDIA_VIDEO) {
            if (auto foundCodec = std::static_pointer_cast<sip_core::AccountVideoCodecInfo>(codec)) {
                foundCodec->setCodecSpecifications(details);
                SIP_CORE_WARN("parameters for %s changed ",
                              foundCodec->systemCodecInfo.name.c_str());
                if (auto call = sip_core::Manager::instance().getCurrentCall()) {
                    if (call->getVideoCodec() == foundCodec) {
                        SIP_CORE_WARN("%s running. Need to restart encoding",
                                      foundCodec->systemCodecInfo.name.c_str());
                        call->restartMediaSender();
                    }
                }
                sip_core::emitSignal<ConfigurationSignal::MediaParametersChanged>(accountID);
                return true;
            }
        }
    } catch (const std::exception& e) {
        SIP_CORE_ERR("Cannot set codec specifications: %s", e.what());
    }

    return false;
}

std::map<std::string, std::string>
getCodecDetails(const std::string& accountID, const unsigned& codecId)
{
    auto acc = sip_core::Manager::instance().getAccount(accountID);
    if (!acc) {
        SIP_CORE_ERR("Could not find account %s return default codec details", accountID.c_str());
        return sip_core::Account::getDefaultCodecDetails(codecId);
    }

    auto codec = acc->searchCodecById(codecId, sip_core::MEDIA_ALL);
    if (!codec) {
        sip_core::emitSignal<ConfigurationSignal::Error>(CODECS_NOT_LOADED);
        return {};
    }

    if (codec->systemCodecInfo.mediaType & sip_core::MEDIA_AUDIO)
        if (auto foundCodec = std::static_pointer_cast<sip_core::AccountAudioCodecInfo>(codec))
            return foundCodec->getCodecSpecifications();

    if (codec->systemCodecInfo.mediaType & sip_core::MEDIA_VIDEO)
        if (auto foundCodec = std::static_pointer_cast<sip_core::AccountVideoCodecInfo>(codec))
            return foundCodec->getCodecSpecifications();

    sip_core::emitSignal<ConfigurationSignal::Error>(CODECS_NOT_LOADED);
    return {};
}

std::vector<unsigned>
getActiveCodecList(const std::string& accountID)
{
    if (auto acc = sip_core::Manager::instance().getAccount(accountID))
        return acc->getActiveCodecs();
    SIP_CORE_ERR("Could not find account %s, returning default", accountID.c_str());
    return sip_core::Account::getDefaultCodecsId();
}

void
setActiveCodecList(const std::string& accountID, const std::vector<unsigned>& list)
{
    if (auto acc = sip_core::Manager::instance().getAccount(accountID)) {
        acc->setActiveCodecs(list);
        sip_core::Manager::instance().saveConfig(acc);
    } else {
        SIP_CORE_ERR("Could not find account %s", accountID.c_str());
    }
}

std::vector<std::string>
getAudioPluginList()
{
    return {PCM_DEFAULT, PCM_DMIX_DSNOOP};
}

void
setAudioPlugin(const std::string& audioPlugin)
{
    return sip_core::Manager::instance().setAudioPlugin(audioPlugin);
}

std::vector<std::string>
getAudioOutputDeviceList()
{
    return sip_core::Manager::instance().getAudioOutputDeviceList();
}

std::vector<std::string>
getAudioInputDeviceList()
{
    return sip_core::Manager::instance().getAudioInputDeviceList();
}

void
setAudioOutputDevice(int32_t index)
{
    return sip_core::Manager::instance().setAudioDevice(index, AudioDeviceType::PLAYBACK);
}

void
setAudioInputDevice(int32_t index)
{
    return sip_core::Manager::instance().setAudioDevice(index, AudioDeviceType::CAPTURE);
}

void
startAudio()
{
    sip_core::Manager::instance().startAudio();
}

void
setAudioRingtoneDevice(int32_t index)
{
    return sip_core::Manager::instance().setAudioDevice(index, AudioDeviceType::RINGTONE);
}

std::vector<int>
getCurrentAudioDevicesIndex()
{
    return sip_core::Manager::instance().getCurrentAudioDevicesIndex();
}

int32_t
getAudioInputDeviceIndex(const std::string& name)
{
    return sip_core::Manager::instance().getAudioInputDeviceIndex(name);
}

int32_t
getAudioOutputDeviceIndex(const std::string& name)
{
    return sip_core::Manager::instance().getAudioOutputDeviceIndex(name);
}

std::string
getCurrentAudioOutputPlugin()
{
    auto plugin = sip_core::Manager::instance().getCurrentAudioOutputPlugin();
    SIP_CORE_DBG("Get audio plugin %s", plugin.c_str());
    return plugin;
}

std::string
getNoiseSuppressState()
{
    return sip_core::Manager::instance().getNoiseSuppressState();
}

void
setNoiseSuppressState(const std::string& state)
{
    sip_core::Manager::instance().setNoiseSuppressState(state);
}

std::string
getEchoCancellerState()
{
    return sip_core::Manager::instance().getEchoCancellerState();
}

void
setEchoCancellerState(const std::string& state)
{
    sip_core::Manager::instance().setEchoCancellerState(state);
}

bool
isAgcEnabled()
{
    return sip_core::Manager::instance().isAGCEnabled();
}

void
setAgcState(bool enabled)
{
    sip_core::Manager::instance().setAGCState(enabled);
}

bool
isVADEnabled()
{
    return sip_core::Manager::instance().isVADEnabled();
}

void
setVADState(bool enabled)
{
    sip_core::Manager::instance().setVADState(enabled);
}

std::string
getRecordPath()
{
    return sip_core::Manager::instance().audioPreference.getRecordPath();
}

std::string
getHomePath()
{
    return sip_core::Manager::instance().getHomePath();
}

void
setRecordPath(const std::string& recPath)
{
    sip_core::Manager::instance().audioPreference.setRecordPath(recPath);
}

bool
getIsAlwaysRecording()
{
    return sip_core::Manager::instance().getIsAlwaysRecording();
}

void
setIsAlwaysRecording(bool rec)
{
    sip_core::Manager::instance().setIsAlwaysRecording(rec);
}

bool
getRecordPreview()
{
#ifdef ENABLE_VIDEO
    return sip_core::Manager::instance().videoPreferences.getRecordPreview();
#else
    return false;
#endif
}

void
setRecordPreview(bool rec)
{
#ifdef ENABLE_VIDEO
    sip_core::Manager::instance().videoPreferences.setRecordPreview(rec);
    sip_core::Manager::instance().saveConfig();
#endif
}

int32_t
getRecordQuality()
{
#ifdef ENABLE_VIDEO
    return sip_core::Manager::instance().videoPreferences.getRecordQuality();
#else
    return 0;
#endif
}

void
setRecordQuality(int32_t quality)
{
#ifdef ENABLE_VIDEO
    sip_core::Manager::instance().videoPreferences.setRecordQuality(quality);
    sip_core::Manager::instance().saveConfig();
#endif
}

int32_t
getHistoryLimit()
{
    return sip_core::Manager::instance().getHistoryLimit();
}

void
setHistoryLimit(int32_t days)
{
    sip_core::Manager::instance().setHistoryLimit(days);
}

int32_t
getRingingTimeout()
{
    return sip_core::Manager::instance().getRingingTimeout();
}

void
setRingingTimeout(int32_t timeout)
{
    sip_core::Manager::instance().setRingingTimeout(timeout);
}

std::vector<std::string>
getSupportedAudioManagers()
{
    return sip_core::AudioPreference::getSupportedAudioManagers();
}

bool
setAudioManager(const std::string& api)
{
    return sip_core::Manager::instance().setAudioManager(api);
}

std::string
getAudioManager()
{
    return sip_core::Manager::instance().getAudioManager();
}

void
setVolume(const std::string& device, double value)
{
    if (auto audiolayer = sip_core::Manager::instance().getAudioDriver()) {
        SIP_CORE_DBG("set volume for %s: %f", device.c_str(), value);

        if (device == "speaker")
            audiolayer->setPlaybackGain(value);
        else if (device == "mic")
            audiolayer->setCaptureGain(value);

        sip_core::emitSignal<ConfigurationSignal::VolumeChanged>(device, value);
    } else {
        SIP_CORE_ERR("Audio layer not valid while updating volume");
    }
}

double
getVolume(const std::string& device)
{
    if (auto audiolayer = sip_core::Manager::instance().getAudioDriver()) {
        if (device == "speaker")
            return audiolayer->getPlaybackGain();
        if (device == "mic")
            return audiolayer->getCaptureGain();
    }

    SIP_CORE_ERR("Audio layer not valid while updating volume");
    return 0.0;
}

// FIXME: we should store "muteDtmf" instead of "playDtmf"
// in config and avoid negating like this
bool
isDtmfMuted()
{
    return not sip_core::Manager::instance().voipPreferences.getPlayDtmf();
}

void
muteDtmf(bool mute)
{
    sip_core::Manager::instance().voipPreferences.setPlayDtmf(not mute);
}

bool
isCaptureMuted()
{
    if (auto audiolayer = sip_core::Manager::instance().getAudioDriver())
        return audiolayer->isCaptureMuted();

    SIP_CORE_ERR("Audio layer not valid");
    return false;
}

void
muteCapture(bool mute)
{
    if (auto audiolayer = sip_core::Manager::instance().getAudioDriver())
        return audiolayer->muteCapture(mute);

    SIP_CORE_ERR("Audio layer not valid");
    return;
}

bool
isPlaybackMuted()
{
    if (auto audiolayer = sip_core::Manager::instance().getAudioDriver())
        return audiolayer->isPlaybackMuted();

    SIP_CORE_ERR("Audio layer not valid");
    return false;
}

void
mutePlayback(bool mute)
{
    if (auto audiolayer = sip_core::Manager::instance().getAudioDriver())
        return audiolayer->mutePlayback(mute);

    SIP_CORE_ERR("Audio layer not valid");
    return;
}

bool
isRingtoneMuted()
{
    if (auto audiolayer = sip_core::Manager::instance().getAudioDriver())
        return audiolayer->isRingtoneMuted();

    SIP_CORE_ERR("Audio layer not valid");
    return false;
}

void
muteRingtone(bool mute)
{
    if (auto audiolayer = sip_core::Manager::instance().getAudioDriver())
        return audiolayer->muteRingtone(mute);

    SIP_CORE_ERR("Audio layer not valid");
    return;
}

void
setAccountsOrder(const std::string& order)
{
    sip_core::Manager::instance().setAccountsOrder(order);
}

std::string
applicationProxy()
{
    return sip_core::Manager::instance().applicationProxy;
}

std::string
getAddrFromInterfaceName(const std::string& interface)
{
    return sip_core::ip_utils::getInterfaceAddr(interface, AF_INET);
}

std::vector<std::string>
getAllIpInterface()
{
    return sip_core::ip_utils::getAllIpInterface();
}

std::vector<std::string>
getAllIpInterfaceByName()
{
    return sip_core::ip_utils::getAllIpInterfaceByName();
}

std::vector<std::map<std::string, std::string>>
getCredentials(const std::string& accountID)
{
    if (auto sipaccount = sip_core::Manager::instance().getAccount<SIPAccount>(accountID))
        return sipaccount->getCredentials();
    return {};
}

void
setCredentials(const std::string& accountID,
               const std::vector<std::map<std::string, std::string>>& details)
{
    if (auto sipaccount = sip_core::Manager::instance().getAccount<SIPAccount>(accountID)) {
        sipaccount->doUnregister([&](bool /* transport_free */) {
            sipaccount->editConfig(
                [&](sip_core::SipAccountConfig& config) { config.setCredentials(details); });
            sipaccount->loadConfig();
            if (sipaccount->isEnabled())
                sipaccount->doRegister();
        });
        sip_core::Manager::instance().saveConfig(sipaccount);
    }
}

void
connectivityChanged()
{
    SIP_CORE_WARN("received connectivity changed - trying to re-connect enabled accounts");

    for (const auto& account : sip_core::Manager::instance().getAllAccounts()) {
        account->connectivityChanged();
    }
}

bool
isAudioMeterActive(const std::string& id)
{
    return sip_core::Manager::instance().getRingBufferPool().isAudioMeterActive(id);
}

void
setAudioMeterState(const std::string& id, bool state)
{
    sip_core::Manager::instance().getRingBufferPool().setAudioMeterState(id, state);
}

void
setDefaultModerator(const std::string& accountID, const std::string& peerURI, bool state)
{
    sip_core::Manager::instance().setDefaultModerator(accountID, peerURI, state);
}

std::vector<std::string>
getDefaultModerators(const std::string& accountID)
{
    return sip_core::Manager::instance().getDefaultModerators(accountID);
}

void
enableLocalModerators(const std::string& accountID, bool isModEnabled)
{
    sip_core::Manager::instance().enableLocalModerators(accountID, isModEnabled);
}

bool
isLocalModeratorsEnabled(const std::string& accountID)
{
    return sip_core::Manager::instance().isLocalModeratorsEnabled(accountID);
}

void
setAllModerators(const std::string& accountID, bool allModerators)
{
    sip_core::Manager::instance().setAllModerators(accountID, allModerators);
}

bool
isAllModerators(const std::string& accountID)
{
    return sip_core::Manager::instance().isAllModerators(accountID);
}

} // namespace libsip_core
