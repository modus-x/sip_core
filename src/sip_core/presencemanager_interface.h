/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
 *  Author: Patrick Keroulas <patrick.keroulas@savoirfairelinux.com>
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

#ifndef LIBSIP_CORE_PRESENCEMANAGERI_H
#define LIBSIP_CORE_PRESENCEMANAGERI_H

#include "def.h"

#include <vector>
#include <map>
#include <string>
#include <memory>

#include "sip_core.h"
#include "presence_const.h"

namespace libsip_core {

[[deprecated("Replaced by registerSignalHandlers")]] LIBSIP_CORE_PUBLIC void registerPresHandlers(
    const std::map<std::string, std::shared_ptr<CallbackWrapperBase>>&);

/* Presence subscription/Notification. */
LIBSIP_CORE_PUBLIC void publish(const std::string& accountID, bool status, const std::string& note);
LIBSIP_CORE_PUBLIC void answerServerRequest(const std::string& uri, bool flag);
LIBSIP_CORE_PUBLIC void subscribeBuddy(const std::string& accountID,
                                       const std::string& uri,
                                       bool flag);
LIBSIP_CORE_PUBLIC void subscribeToEvents(const std::string& accountID,
                                          const std::string& uri,
                                          const std::string& eventType,
                                          bool flag);
LIBSIP_CORE_PUBLIC std::vector<std::map<std::string, std::string>> getSubscriptions(
    const std::string& accountID);
LIBSIP_CORE_PUBLIC void setSubscriptions(const std::string& accountID,
                                         const std::vector<std::string>& uris);

// Presence signal type definitions
struct LIBSIP_CORE_PUBLIC PresenceSignal
{
    struct LIBSIP_CORE_PUBLIC NewServerSubscriptionRequest
    {
        constexpr static const char* name = "NewServerSubscriptionRequest";
        using cb_type = void(const std::string& /*remote*/);
    };
    struct LIBSIP_CORE_PUBLIC ServerError
    {
        constexpr static const char* name = "ServerError";
        using cb_type = void(const std::string& /*account_id*/,
                             const std::string& /*error*/,
                             const std::string& /*msg*/);
    };
    struct LIBSIP_CORE_PUBLIC NewBuddyNotification
    {
        constexpr static const char* name = "NewBuddyNotification";
        using cb_type = void(const std::string& /*account_id*/,
                             const std::string& /*buddy_uri*/,
                             int /*status*/,
                             const std::string& /*line_status*/);
    };
    struct LIBSIP_CORE_PUBLIC SubscriptionStateChanged
    {
        constexpr static const char* name = "SubscriptionStateChanged";
        using cb_type = void(const std::string& /*account_id*/,
                             const std::string& /*buddy_uri*/,
                             int /*state*/);
    };
};

} // namespace libsip_core

#endif // PRESENCEMANAGERI_H
