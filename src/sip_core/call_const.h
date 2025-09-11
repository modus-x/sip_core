/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
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
#ifndef LIBSIP_CORE_CALL_H
#define LIBSIP_CORE_CALL_H

#include "def.h"

namespace libsip_core {

namespace Call {

namespace StateEvent {

constexpr static char INCOMING[] = "INCOMING";
constexpr static char CONNECTING[] = "CONNECTING";
constexpr static char RINGING[] = "RINGING";
constexpr static char CURRENT[] = "CURRENT";
constexpr static char HUNGUP[] = "HUNGUP";
constexpr static char BUSY[] = "BUSY";
constexpr static char PEER_BUSY[] = "PEER_BUSY";
constexpr static char FAILURE[] = "FAILURE";
constexpr static char HOLD[] = "HOLD";
constexpr static char UNHOLD[] = "UNHOLD";
constexpr static char INACTIVE[] = "INACTIVE";
constexpr static char OVER[] = "OVER";

} // namespace StateEvent

namespace Details {

constexpr static char CALL_TYPE[] = "CALL_TYPE";
constexpr static char PEER_NUMBER[] = "PEER_NUMBER";
constexpr static char FROM_HEADER[] = "FROM_HEADER";
constexpr static char REGISTERED_NAME[] = "REGISTERED_NAME";
constexpr static char DISPLAY_NAME[] = "DISPLAY_NAME";
constexpr static char CALL_STATE[] = "CALL_STATE";
constexpr static char CONF_ID[] = "CONF_ID";
constexpr static char TIMESTAMP_START[] = "TIMESTAMP_START";
constexpr static char ACCOUNTID[] = "ACCOUNTID";
constexpr static char PEER_HOLDING[] = "PEER_HOLDING";
constexpr static char INVITE_CALL_ID[] = "INVITE_CALL_ID";
constexpr static char AUDIO_MUTED[] = "AUDIO_MUTED";
constexpr static char VIDEO_MUTED[] = "VIDEO_MUTED";
constexpr static char VIDEO_SOURCE[] = "VIDEO_SOURCE";
constexpr static char AUDIO_ONLY[] = "AUDIO_ONLY";
constexpr static char PEER_MUTED[] = "PEER_MUTED";
constexpr static char PEER_VOICE[] = "PEER_VOICE";
constexpr static char AUDIO_CODEC[] = "AUDIO_CODEC";
constexpr static char AUDIO_SAMPLE_RATE[] = "AUDIO_SAMPLE_RATE";
constexpr static char AUDIO_FRACTION_LOST[] = "AUDIO_FRACTION_LOST";
constexpr static char AUDIO_CUM_LOST_PACKET[] = "AUDIO_CUM_LOST_PACKET";
constexpr static char AUDIO_JITTER[] = "AUDIO_JITTER";
constexpr static char AUDIO_EXT_HIGH[] = "AUDIO_EXT_HIGH";
constexpr static char AUDIO_LSR[] = "AUDIO_LSR";
constexpr static char AUDIO_DLSR[] = "AUDIO_DLSR";
constexpr static char AUDIO_SPC[] = "AUDIO_SPC";
constexpr static char AUDIO_SOC[] = "AUDIO_SOC";
constexpr static char AUDIO_TIMESTAMP_MSB[] = "AUDIO_TIMESTAMP_MSB";
constexpr static char AUDIO_TIMESTAMP_LSB[] = "AUDIO_TIMESTAMP_LSB";
constexpr static char AUDIO_TIMESTAMP_RTP[] = "AUDIO_TIMESTAMP_RTP";
constexpr static char AUDIO_BR_EXP[] = "AUDIO_BR_EXP";
constexpr static char AUDIO_BR_MANTIS[] = "AUDIO_BR_MANTIS";
constexpr static char VIDEO_CODEC[] = "VIDEO_CODEC";
constexpr static char SOCKETS[] = "SOCKETS";
constexpr static char VIDEO_MIN_BITRATE[] = "VIDEO_MIN_BITRATE";
constexpr static char VIDEO_BITRATE[] = "VIDEO_BITRATE";
constexpr static char VIDEO_MAX_BITRATE[] = "VIDEO_MAX_BITRATE";
constexpr static char VIDEO_FPS[] = "VIDEO_FPS";
constexpr static char VIDEO_FRACTION_LOST[] = "VIDEO_FRACTION_LOST";
constexpr static char VIDEO_CUM_LOST_PACKET[] = "VIDEO_CUM_LOST_PACKET";
constexpr static char VIDEO_JITTER[] = "VIDEO_JITTER";
constexpr static char VIDEO_EXT_HIGH[] = "VIDEO_EXT_HIGH";
constexpr static char VIDEO_LSR[] = "VIDEO_LSR";
constexpr static char VIDEO_DLSR[] = "VIDEO_DLSR";
constexpr static char VIDEO_SPC[] = "VIDEO_SPC";
constexpr static char VIDEO_SOC[] = "VIDEO_SOC";
constexpr static char VIDEO_TIMESTAMP_MSB[] = "VIDEO_TIMESTAMP_MSB";
constexpr static char VIDEO_TIMESTAMP_LSB[] = "VIDEO_TIMESTAMP_LSB";
constexpr static char VIDEO_TIMESTAMP_RTP[] = "VIDEO_TIMESTAMP_RTP";
constexpr static char VIDEO_BR_EXP[] = "VIDEO_BR_EXP";
constexpr static char VIDEO_BR_MANTIS[] = "VIDEO_BR_MANTIS";

} // namespace Details

} // namespace Call

} // namespace libsip_core

#endif
