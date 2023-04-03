/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
 *  Author: Patrick Keroulas <patrick.keroulas@savoirfairelinux.com>
 *  Author: Guillaume Roguez <Guillaume.Roguez@savoirfairelinux.com>
 *  Author: Simon Désaulniers <simon.desaulniers@savoirfairelinux.com>
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

#include "presencemanager_interface.h"

#include <cerrno>
#include <sstream>
#include <cstring>

#include "logger.h"
#include "manager.h"
#include "sip/sipaccount.h"
#include "sip/sippresence.h"
#include "sip/pres_sub_client.h"
#include "client/ring_signal.h"
#include "compiler_intrinsics.h"

namespace libsip_core {

using sip_core::SIPAccount;

void
registerPresHandlers(const std::map<std::string, std::shared_ptr<CallbackWrapperBase>>& handlers)
{
    registerSignalHandlers(handlers);
}

/**
 * Un/subscribe to buddySipUri for an accountID
 */
void
subscribeBuddy(const std::string& accountID, const std::string& uri, bool flag)
{
    if (auto sipaccount = sip_core::Manager::instance().getAccount<SIPAccount>(accountID)) {
        auto pres = sipaccount->getPresence();
        if (pres and pres->isEnabled() and pres->isSupported(PRESENCE_FUNCTION_SUBSCRIBE)) {
            SIP_CORE_DBG("%subscribePresence (acc:%s, buddy:%s)",
                     flag ? "S" : "Uns",
                     accountID.c_str(),
                     uri.c_str());
            pres->subscribeClient(uri, flag);
        }
    } else
        SIP_CORE_ERR("Could not find account %s", accountID.c_str());
}

/**
 * push a presence for a account
 * Notify for IP2IP account and publish for PBX account
 */
void
publish(const std::string& accountID, bool status, const std::string& note)
{
    if (auto sipaccount = sip_core::Manager::instance().getAccount<SIPAccount>(accountID)) {
        auto pres = sipaccount->getPresence();
        if (pres and pres->isEnabled() and pres->isSupported(PRESENCE_FUNCTION_PUBLISH)) {
            SIP_CORE_DBG("Send Presence (acc:%s, status %s).",
                     accountID.c_str(),
                     status ? "online" : "offline");
            pres->sendPresence(status, note);
        }
    } else
        SIP_CORE_ERR("Could not find account %s.", accountID.c_str());
}

/**
 * Accept or not a PresSubServer request for IP2IP account
 */
void
answerServerRequest(UNUSED const std::string& uri, UNUSED bool flag)
{
#if 0 // DISABLED: removed IP2IP support, tuleap: #448
    auto account = sip_core::Manager::instance().getIP2IPAccount();
    if (auto sipaccount = static_cast<SIPAccount *>(account.get())) {
        SIP_CORE_DBG("Approve presence (acc:IP2IP, serv:%s, flag:%s)", uri.c_str(),
                 flag ? "true" : "false");

        if (auto pres = sipaccount->getPresence())
            pres->approvePresSubServer(uri, flag);
        else
            SIP_CORE_ERR("Presence not initialized");
    } else
        SIP_CORE_ERR("Could not find account IP2IP");
#else
    SIP_CORE_ERR("answerServerRequest() is deprecated and does nothing");
#endif
}

/**
 * Get all active subscriptions for "accountID"
 */
std::vector<std::map<std::string, std::string>>
getSubscriptions(const std::string& accountID)
{
    std::vector<std::map<std::string, std::string>> ret;

    if (auto sipaccount = sip_core::Manager::instance().getAccount<SIPAccount>(accountID)) {
        if (auto pres = sipaccount->getPresence()) {
            const auto& subs = pres->getClientSubscriptions();
            ret.reserve(subs.size());
            for (const auto& s : subs) {
                ret.push_back(
                    {{libsip_core::Presence::BUDDY_KEY, std::string(s->getURI())},
                     {libsip_core::Presence::STATUS_KEY,
                      s->isPresent() ? libsip_core::Presence::ONLINE_KEY
                                     : libsip_core::Presence::OFFLINE_KEY},
                     {libsip_core::Presence::LINESTATUS_KEY, std::string(s->getLineStatus())}});
            }
        } else
            SIP_CORE_ERR("Presence not initialized");
    } else
        SIP_CORE_ERR("Could not find account %s.", accountID.c_str());

    return ret;
}

/**
 * Batch subscribing of URIs
 */
void
setSubscriptions(const std::string& accountID, const std::vector<std::string>& uris)
{
    if (auto sipaccount = sip_core::Manager::instance().getAccount<SIPAccount>(accountID)) {
        if (auto pres = sipaccount->getPresence()) {
            for (const auto& u : uris)
                pres->subscribeClient(u, true);
        } else
            SIP_CORE_ERR("Presence not initialized");
    } else
        SIP_CORE_ERR("Could not find account %s.", accountID.c_str());
}

} // namespace libsip_core
