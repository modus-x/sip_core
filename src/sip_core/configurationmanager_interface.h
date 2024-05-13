/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
 *  Author: Pierre-Luc Beaudoin <pierre-luc.beaudoin@savoirfairelinux.com>
 *  Author: Alexandre Bourget <alexandre.bourget@savoirfairelinux.com>
 *  Author: Emmanuel Milou <emmanuel.milou@savoirfairelinux.com>
 *  Author: Guillaume Carmel-Archambault <guillaume.carmel-archambault@savoirfairelinux.com>
 *  Author: Guillaume Roguez <Guillaume.Roguez@savoirfairelinux.com>
 *  Author: Adrien Béraud <adrien.beraud@savoirfairelinux.com>
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

#include "def.h"

#include <vector>
#include <map>
#include <memory>
#include <string>
#include <cstdint>
#include "account_const.h"

#include "sip_core.h"

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

namespace libsip_core {

[[deprecated("Replaced by registerSignalHandlers")]] LIBSIP_CORE_PUBLIC void registerConfHandlers(
    const std::map<std::string, std::shared_ptr<CallbackWrapperBase>>&);

struct LIBSIP_CORE_PUBLIC Message
{
    std::string from;
    std::map<std::string, std::string> payloads;
    uint64_t received;
};

LIBSIP_CORE_PUBLIC std::map<std::string, std::string> getAccountDetails(const std::string& accountID);
LIBSIP_CORE_PUBLIC bool switchTransport(const std::string& accountID, Account::Transport type);
LIBSIP_CORE_PUBLIC std::map<std::string, std::string> getVolatileAccountDetails(
    const std::string& accountID);
LIBSIP_CORE_PUBLIC void setAccountDetails(const std::string& accountID,
                                      const std::map<std::string, std::string>& details);
LIBSIP_CORE_PUBLIC void setAccountActive(const std::string& accountID,
                                     bool active,
                                     bool shutdownConnections = false);
LIBSIP_CORE_PUBLIC std::map<std::string, std::string> getAccountTemplate(const std::string& accountType);
LIBSIP_CORE_PUBLIC std::string addAccount(const std::map<std::string, std::string>& details,
                                      const std::string& accountID = {});
LIBSIP_CORE_PUBLIC void monitor(bool continuous);

LIBSIP_CORE_PUBLIC void removeAccount(const std::string& accountID);
LIBSIP_CORE_PUBLIC void playDigitSound(const std::string& digit);
LIBSIP_CORE_PUBLIC std::vector<std::string> getAccountList();
LIBSIP_CORE_PUBLIC void sendRegister(const std::string& accountID, bool enable);
LIBSIP_CORE_PUBLIC void registerAllAccounts(void);
LIBSIP_CORE_PUBLIC uint64_t sendAccountTextMessage(const std::string& accountID,
                                               const std::string& to,
                                               const std::map<std::string, std::string>& payloads);
LIBSIP_CORE_PUBLIC bool cancelMessage(const std::string& accountID, uint64_t message);
LIBSIP_CORE_PUBLIC std::vector<Message> getLastMessages(const std::string& accountID,
                                                    const uint64_t& base_timestamp);
LIBSIP_CORE_PUBLIC int getMessageStatus(uint64_t id);
LIBSIP_CORE_PUBLIC std::string applicationProxy();
LIBSIP_CORE_PUBLIC int getMessageStatus(const std::string& accountID, uint64_t id);
LIBSIP_CORE_PUBLIC void setIsComposing(const std::string& accountID,
                                   const std::string& conversationUri,
                                   bool isWriting);
LIBSIP_CORE_PUBLIC bool setMessageDisplayed(const std::string& accountID,
                                        const std::string& conversationUri,
                                        const std::string& messageId,
                                        int status);

LIBSIP_CORE_PUBLIC std::vector<unsigned> getCodecList();
LIBSIP_CORE_PUBLIC std::map<std::string, std::string> getCodecDetails(const std::string& accountID,
                                                                  const unsigned& codecId);
LIBSIP_CORE_PUBLIC bool setCodecDetails(const std::string& accountID,
                                    const unsigned& codecId,
                                    const std::map<std::string, std::string>& details);
LIBSIP_CORE_PUBLIC std::vector<unsigned> getActiveCodecList(const std::string& accountID);

LIBSIP_CORE_PUBLIC void setActiveCodecList(const std::string& accountID,
                                       const std::vector<unsigned>& list);


LIBSIP_CORE_PUBLIC void setDND(const std::string& accountID, bool isDND);

LIBSIP_CORE_PUBLIC std::vector<std::string> getAudioPluginList();
LIBSIP_CORE_PUBLIC void setAudioPlugin(const std::string& audioPlugin);
LIBSIP_CORE_PUBLIC std::vector<std::string> getAudioOutputDeviceList();
LIBSIP_CORE_PUBLIC void setAudioOutputDevice(int32_t index);
LIBSIP_CORE_PUBLIC void startAudio();
LIBSIP_CORE_PUBLIC void setAudioInputDevice(int32_t index);
LIBSIP_CORE_PUBLIC void setAudioRingtoneDevice(int32_t index);
LIBSIP_CORE_PUBLIC std::vector<std::string> getAudioInputDeviceList();
LIBSIP_CORE_PUBLIC std::vector<int> getCurrentAudioDevicesIndex();
LIBSIP_CORE_PUBLIC int32_t getAudioInputDeviceIndex(const std::string& name);
LIBSIP_CORE_PUBLIC int32_t getAudioOutputDeviceIndex(const std::string& name);
LIBSIP_CORE_PUBLIC std::string getCurrentAudioOutputPlugin();

LIBSIP_CORE_PUBLIC std::string getNoiseSuppressState();
LIBSIP_CORE_PUBLIC void setNoiseSuppressState(const std::string& state);
LIBSIP_CORE_PUBLIC std::string getEchoCancellerState();
LIBSIP_CORE_PUBLIC void setEchoCancellerState(const std::string& state);

LIBSIP_CORE_PUBLIC bool isAgcEnabled();
LIBSIP_CORE_PUBLIC void setAgcState(bool enabled);
LIBSIP_CORE_PUBLIC bool isVADEnabled();
LIBSIP_CORE_PUBLIC void setVADState(bool enabled);

LIBSIP_CORE_PUBLIC void setAutoAnswer(const std::string& accountId, bool enable);

LIBSIP_CORE_PUBLIC void muteDtmf(bool mute);
LIBSIP_CORE_PUBLIC bool isDtmfMuted();

LIBSIP_CORE_PUBLIC bool isCaptureMuted();
LIBSIP_CORE_PUBLIC void muteCapture(bool mute);
LIBSIP_CORE_PUBLIC bool isPlaybackMuted();
LIBSIP_CORE_PUBLIC void mutePlayback(bool mute);
LIBSIP_CORE_PUBLIC bool isRingtoneMuted();
LIBSIP_CORE_PUBLIC void muteRingtone(bool mute);

LIBSIP_CORE_PUBLIC std::vector<std::string> getSupportedAudioManagers();
LIBSIP_CORE_PUBLIC std::string getAudioManager();
LIBSIP_CORE_PUBLIC bool setAudioManager(const std::string& api);
LIBSIP_CORE_PUBLIC bool isInitialized();

LIBSIP_CORE_PUBLIC std::string getRecordPath();
LIBSIP_CORE_PUBLIC std::string getHomePath();
LIBSIP_CORE_PUBLIC void setRecordPath(const std::string& recPath);
LIBSIP_CORE_PUBLIC bool getIsAlwaysRecording();
LIBSIP_CORE_PUBLIC void setIsAlwaysRecording(bool rec);
LIBSIP_CORE_PUBLIC bool getRecordPreview();
LIBSIP_CORE_PUBLIC void setRecordPreview(bool rec);
LIBSIP_CORE_PUBLIC int getRecordQuality();
LIBSIP_CORE_PUBLIC void setRecordQuality(int quality);

LIBSIP_CORE_PUBLIC void setHistoryLimit(int32_t days);
LIBSIP_CORE_PUBLIC int32_t getHistoryLimit();

LIBSIP_CORE_PUBLIC void setRingingTimeout(int32_t timeout);
LIBSIP_CORE_PUBLIC int32_t getRingingTimeout();

LIBSIP_CORE_PUBLIC void setAccountsOrder(const std::string& order);

LIBSIP_CORE_PUBLIC std::vector<std::map<std::string, std::string>> getCredentials(
    const std::string& accountID);
LIBSIP_CORE_PUBLIC void setCredentials(const std::string& accountID,
                                   const std::vector<std::map<std::string, std::string>>& details);

LIBSIP_CORE_PUBLIC std::string getAddrFromInterfaceName(const std::string& iface);

LIBSIP_CORE_PUBLIC std::vector<std::string> getAllIpInterface();
LIBSIP_CORE_PUBLIC std::vector<std::string> getAllIpInterfaceByName();

LIBSIP_CORE_PUBLIC void setVolume(const std::string& device, double value);
LIBSIP_CORE_PUBLIC double getVolume(const std::string& device);

/*
 * Network connectivity
 */
LIBSIP_CORE_PUBLIC void connectivityChanged();

/* Dht proxy */

/**
 * Set the device push notification token (for all accounts).
 * If set, proxy clients will use push notifications.
 * Set to empty to disable push notifications.
 */
LIBSIP_CORE_PUBLIC void setPushNotificationToken(const std::string& pushDeviceToken);

/**
 * Set the topic for ios
 * bundle_id for ios 14.5 and higher
 * bundle_id.voip for ios prior 14.5
 */
LIBSIP_CORE_PUBLIC void setPushNotificationTopic(const std::string& topic);
/**
 * To be called by clients with relevant data when a push notification is received.
 */
LIBSIP_CORE_PUBLIC void pushNotificationReceived(const std::string& from,
                                             const std::map<std::string, std::string>& data);

/**
 * Returns whether or not the audio meter is enabled for ring buffer @id.
 *
 * NOTE If @id is empty, returns true if at least 1 audio meter is enabled.
 */
LIBSIP_CORE_PUBLIC bool isAudioMeterActive(const std::string& id);

/**
 * Enables/disables an audio meter for the specified @id.
 *
 * NOTE If @id is empty, applies to all ring buffers.
 */
LIBSIP_CORE_PUBLIC void setAudioMeterState(const std::string& id, bool state);

/**
 * Add/remove default moderator for conferences
 */
LIBSIP_CORE_PUBLIC void setDefaultModerator(const std::string& accountID,
                                        const std::string& peerURI,
                                        bool state);

/**
 * Get default moderators for an account
 */
LIBSIP_CORE_PUBLIC std::vector<std::string> getDefaultModerators(const std::string& accountID);

/**
 * Enable/disable local moderators for conferences
 */
LIBSIP_CORE_PUBLIC void enableLocalModerators(const std::string& accountID, bool isModEnabled);

/**
 * Get local moderators state
 */
LIBSIP_CORE_PUBLIC bool isLocalModeratorsEnabled(const std::string& accountID);

/**
 * Enable/disable all moderators for conferences
 */
LIBSIP_CORE_PUBLIC void setAllModerators(const std::string& accountID, bool allModerators);

/**
 * Get all moderators state
 */
LIBSIP_CORE_PUBLIC bool isAllModerators(const std::string& accountID);

struct LIBSIP_CORE_PUBLIC AudioSignal
{
    struct LIBSIP_CORE_PUBLIC DeviceEvent
    {
        constexpr static const char* name = "audioDeviceEvent";
        using cb_type = void(void);
    };
    // Linear audio level (between 0 and 1). To get level in dB: dB=20*log10(level)
    struct LIBSIP_CORE_PUBLIC AudioMeter
    {
        constexpr static const char* name = "AudioMeter";
        using cb_type = void(const std::string& id, float level);
    };
};

// Configuration signal type definitions
struct LIBSIP_CORE_PUBLIC ConfigurationSignal
{
    struct LIBSIP_CORE_PUBLIC VolumeChanged
    {
        constexpr static const char* name = "VolumeChanged";
        using cb_type = void(const std::string& /*device*/, double /*value*/);
    };
    struct LIBSIP_CORE_PUBLIC AccountsChanged
    {
        constexpr static const char* name = "AccountsChanged";
        using cb_type = void(void);
    };
    struct LIBSIP_CORE_PUBLIC Error
    {
        constexpr static const char* name = "Error";
        using cb_type = void(int /*alert*/);
    };

    // TODO: move those to AccountSignal in next API breakage
    struct LIBSIP_CORE_PUBLIC AccountDetailsChanged
    {
        constexpr static const char* name = "AccountDetailsChanged";
        using cb_type = void(const std::string& /*account_id*/,
                             const std::map<std::string, std::string>& /* details */);
    };
    struct LIBSIP_CORE_PUBLIC StunStatusFailed
    {
        constexpr static const char* name = "StunStatusFailed";
        using cb_type = void(const std::string& /*account_id*/);
    };
    struct LIBSIP_CORE_PUBLIC RegistrationStateChanged
    {
        constexpr static const char* name = "RegistrationStateChanged";
        using cb_type = void(const std::string& /*account_id*/,
                             const std::string& /*state*/,
                             int /*detailsCode*/,
                             const std::string& /*detailsStr*/);
    };
    struct LIBSIP_CORE_PUBLIC VolatileDetailsChanged
    {
        constexpr static const char* name = "VolatileDetailsChanged";
        using cb_type = void(const std::string& /*account_id*/,
                             const std::map<std::string, std::string>& /* details */);
    };
    struct LIBSIP_CORE_PUBLIC IncomingAccountMessage
    {
        constexpr static const char* name = "IncomingAccountMessage";
        using cb_type = void(const std::string& /*account_id*/,
                             const std::string& /*from*/,
                             const std::string& /*message_id*/,
                             const std::map<std::string, std::string>& /*payloads*/);
    };
    struct LIBSIP_CORE_PUBLIC AccountMessageStatusChanged
    {
        constexpr static const char* name = "AccountMessageStatusChanged";
        using cb_type = void(const std::string& /*account_id*/,
                             const std::string& /*conversation_id*/,
                             const std::string& /*peer*/,
                             const std::string& /*message_id*/,
                             int /*state*/);
    };
    struct LIBSIP_CORE_PUBLIC ActiveCallsChanged
    {
        constexpr static const char* name = "ActiveCallsChanged";
        using cb_type = void(const std::string& /*account_id*/,
                             const std::string& /*conversation_id*/,
                             const std::vector<std::map<std::string, std::string>>& /*activeCalls*/);
    };
    struct LIBSIP_CORE_PUBLIC MediaParametersChanged
    {
        constexpr static const char* name = "MediaParametersChanged";
        using cb_type = void(const std::string& /*accountId*/);
    };
    /**
     * These are special getters for Android and UWP, so the daemon can retrieve
     * information only accessible through their respective platform APIs
     */
#if defined(__ANDROID__) || (defined(TARGET_OS_IOS) && TARGET_OS_IOS)
    struct LIBSIP_CORE_PUBLIC GetHardwareAudioFormat
    {
        constexpr static const char* name = "GetHardwareAudioFormat";
        using cb_type = void(std::vector<int32_t>* /* params_ret */);
    };
#endif
#if defined(__ANDROID__) || defined(RING_UWP) || (defined(TARGET_OS_IOS) && TARGET_OS_IOS)
    struct LIBSIP_CORE_PUBLIC GetAppDataPath
    {
        constexpr static const char* name = "GetAppDataPath";
        using cb_type = void(const std::string& name, std::vector<std::string>* /* path_ret */);
    };
    struct LIBSIP_CORE_PUBLIC GetDeviceName
    {
        constexpr static const char* name = "GetDeviceName";
        using cb_type = void(std::vector<std::string>* /* path_ret */);
    };
#endif
    struct LIBSIP_CORE_PUBLIC HardwareDecodingChanged
    {
        constexpr static const char* name = "HardwareDecodingChanged";
        using cb_type = void(bool /* state */);
    };
    struct LIBSIP_CORE_PUBLIC HardwareEncodingChanged
    {
        constexpr static const char* name = "HardwareEncodingChanged";
        using cb_type = void(bool /* state */);
    };
    struct LIBSIP_CORE_PUBLIC MessageSend
    {
        constexpr static const char* name = "MessageSend";
        using cb_type = void(const std::string&);
    };
};

} // namespace libsip_core
