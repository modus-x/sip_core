/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
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

#include "ring_signal.h"

namespace sip_core {

std::unique_ptr<ScheduledExecutor> eventScheduler = nullptr;

SignalHandlerMap&
getSignalHandlers()
{
    static SignalHandlerMap handlers = {
        /* Call */
        exported_callback<libsip_core::CallSignal::StateChange>(),
        exported_callback<libsip_core::CallSignal::TransferStateChange>(),
        exported_callback<libsip_core::CallSignal::StateChange>(),
        exported_callback<libsip_core::CallSignal::TransferFailed>(),
        exported_callback<libsip_core::CallSignal::TransferSucceeded>(),
        exported_callback<libsip_core::CallSignal::RecordPlaybackStopped>(),
        exported_callback<libsip_core::CallSignal::VideoSenderNatResolved>(),
        exported_callback<libsip_core::CallSignal::VoiceMailNotify>(),
        exported_callback<libsip_core::CallSignal::IncomingMessage>(),
        exported_callback<libsip_core::CallSignal::IncomingCall>(),
        exported_callback<libsip_core::CallSignal::IncomingCallWithMedia>(),
        exported_callback<libsip_core::CallSignal::ExtraHeadersUpdated>(),
        exported_callback<libsip_core::CallSignal::MediaChangeRequested>(),
        exported_callback<libsip_core::CallSignal::RecordPlaybackFilepath>(),
        exported_callback<libsip_core::CallSignal::ConferenceCreated>(),
        exported_callback<libsip_core::CallSignal::ConferenceChanged>(),
        exported_callback<libsip_core::CallSignal::UpdatePlaybackScale>(),
        exported_callback<libsip_core::CallSignal::ConferenceRemoved>(),
        exported_callback<libsip_core::CallSignal::RecordingStateChanged>(),
        exported_callback<libsip_core::CallSignal::RtcpReportReceived>(),
        exported_callback<libsip_core::CallSignal::PeerHold>(),
        exported_callback<libsip_core::CallSignal::VideoMuted>(),
        exported_callback<libsip_core::CallSignal::AudioMuted>(),
        exported_callback<libsip_core::CallSignal::PeerMuted>(),
        exported_callback<libsip_core::CallSignal::SmartInfo>(),
        exported_callback<libsip_core::CallSignal::ConnectionUpdate>(),
        exported_callback<libsip_core::CallSignal::OnConferenceInfosUpdated>(),
        exported_callback<libsip_core::CallSignal::RemoteRecordingChanged>(),
        exported_callback<libsip_core::CallSignal::MediaNegotiationStatus>(),

        /* Configuration */
        exported_callback<libsip_core::ConfigurationSignal::VolumeChanged>(),
        exported_callback<libsip_core::ConfigurationSignal::AccountsChanged>(),
        exported_callback<libsip_core::ConfigurationSignal::AccountDetailsChanged>(),
        exported_callback<libsip_core::ConfigurationSignal::StunStatusFailed>(),
        exported_callback<libsip_core::ConfigurationSignal::RegistrationStateChanged>(),
        exported_callback<libsip_core::ConfigurationSignal::VolatileDetailsChanged>(),
        exported_callback<libsip_core::ConfigurationSignal::IncomingAccountMessage>(),
        exported_callback<libsip_core::ConfigurationSignal::AccountMessageStatusChanged>(),
        exported_callback<libsip_core::ConfigurationSignal::ActiveCallsChanged>(),
        exported_callback<libsip_core::ConfigurationSignal::MediaParametersChanged>(),
        exported_callback<libsip_core::ConfigurationSignal::Error>(),
#if defined(__ANDROID__) || (defined(TARGET_OS_IOS) && TARGET_OS_IOS)
        exported_callback<libsip_core::ConfigurationSignal::GetHardwareAudioFormat>(),
#endif
#if defined(__ANDROID__) || (defined(TARGET_OS_IOS) && TARGET_OS_IOS) || defined(RING_UWP)
        exported_callback<libsip_core::ConfigurationSignal::GetAppDataPath>(),
        exported_callback<libsip_core::ConfigurationSignal::GetDeviceName>(),
#endif
        exported_callback<libsip_core::ConfigurationSignal::HardwareDecodingChanged>(),
        exported_callback<libsip_core::ConfigurationSignal::HardwareEncodingChanged>(),
        exported_callback<libsip_core::ConfigurationSignal::MessageSend>(),

        /* Presence */
        exported_callback<libsip_core::PresenceSignal::NewServerSubscriptionRequest>(),
        exported_callback<libsip_core::PresenceSignal::ServerError>(),
        exported_callback<libsip_core::PresenceSignal::NewBuddyNotification>(),
        exported_callback<libsip_core::PresenceSignal::SubscriptionStateChanged>(),
        exported_callback<libsip_core::PresenceSignal::NotifyReceived>(),

        /* Audio */
        exported_callback<libsip_core::AudioSignal::DeviceEvent>(),
        exported_callback<libsip_core::AudioSignal::AudioMeter>(),

#ifdef ENABLE_VIDEO
        /* MediaPlayer */
        exported_callback<libsip_core::MediaPlayerSignal::FileOpened>(),

        /* Video */
        exported_callback<libsip_core::VideoSignal::DeviceEvent>(),
        exported_callback<libsip_core::VideoSignal::DecodingStarted>(),
        exported_callback<libsip_core::VideoSignal::DecodingStopped>(),
#ifdef __ANDROID__
        exported_callback<libsip_core::VideoSignal::GetCameraInfo>(),
        exported_callback<libsip_core::VideoSignal::SetParameters>(),
        exported_callback<libsip_core::VideoSignal::RequestKeyFrame>(),
        exported_callback<libsip_core::VideoSignal::SetBitrate>(),
#endif
        exported_callback<libsip_core::VideoSignal::StartCapture>(),
        exported_callback<libsip_core::VideoSignal::StopCapture>(),
        exported_callback<libsip_core::VideoSignal::DeviceAdded>(),
        exported_callback<libsip_core::VideoSignal::ParametersChanged>(),
#endif

    };

    return handlers;
}

}; // namespace sip_core

namespace libsip_core {

void
registerSignalHandlers(const std::map<std::string, std::shared_ptr<CallbackWrapperBase>>& handlers)
{
    if (sip_core::eventScheduler == nullptr) {
        sip_core::eventScheduler = std::make_unique<sip_core::ScheduledExecutor>("SipEventEmitter");
    }
    auto& handlers_ = sip_core::getSignalHandlers();
    for (auto& item : handlers) {
        auto iter = handlers_.find(item.first);
        if (iter == handlers_.end()) {
            SIP_CORE_DBG("Signal %s not supported", item.first.c_str());
            continue;
        }
        else {
            SIP_CORE_INFO("Signal %s is registered", item.first.c_str());

        }
        iter->second = item.second;
    }
}

void
unregisterSignalHandlers()
{
    SIP_CORE_DBG("Signals are unregistered");
    sip_core::eventScheduler = nullptr;
    auto& handlers_ = sip_core::getSignalHandlers();
    for (auto& item : handlers_) {
        item.second = {};
    }
}

} // namespace libsip_core
