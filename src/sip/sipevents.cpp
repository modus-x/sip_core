/*
 * Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
 *  Author: Patrick Keroulas  <patrick.keroulas@savoirfairelinux.com>
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

#include "sip/sipevents.h"

#include "logger.h"
#include "manager.h"
#include "sip/sipaccount.h"
#include "connectivity/sip_utils.h"
#include "sip/sipvoiplink.h"
#include "client/ring_signal.h"
#include "connectivity/sip_utils.h"

#include <fmt/core.h>

#include <thread>
#include <sstream>
#include <algorithm>

#define DEFAULT_EVENT_EXPIRE 600

namespace sip_core {

using sip_utils::CONST_PJ_STR;


SIPEvents::SIPEvents(SIPAccount* acc)
    : enabled_(true)
    , acc_(acc)
{}

SIPEvents::~SIPEvents()
{
    /* Flush the lists */
    // FIXME: Can't destroy/unsubscribe buddies properly.
    // Is the transport usable when the account is being destroyed?
    // for (const auto & c : sub_client_list_)
    //    delete(c);
    sub_list_.clear();
}

SIPAccount*
SIPEvents::getAccount() const
{
    return acc_;
}

int
SIPEvents::getModId() const
{
    return Manager::instance().sipVoIPLink().getModId();
}

void
SIPEvents::enable(bool enabled)
{
    enabled_ = enabled;
}

void
SIPEvents::subscribeClient(const std::string& uri, const std::string& event, bool flag)
{
    auto* account = acc_;
    const bool transportReady = account && !account->isTransportRecoveryActive()
                                && static_cast<bool>(account->getTransport());
    const bool registered = account
                            && account->getRegistrationState() == RegistrationState::REGISTERED;
    const bool canAttemptNow = transportReady && registered;
    /* Check if the buddy was already subscribed */
    for (const auto& c : sub_list_) {
        if (c->getURI() == uri && c->getEvent() == event) {
            c->setDesired(flag);
            if (flag) {
                c->refreshContact(acc_->getContactHeader());
                if (!canAttemptNow) {
                    if (account)
                        account->needsResubscribe_.store(true);
                    SIP_CORE_WARN("Deferring event subscription [%.*s] %.*s until account is "
                                  "REGISTERED (transportReady=%d, registered=%d)",
                                  (int) c->getEvent().size(),
                                  c->getEvent().data(),
                                  (int) c->getURI().size(),
                                  c->getURI().data(),
                                  transportReady ? 1 : 0,
                                  registered ? 1 : 0);
                    return;
                }
                c->subscribe();
            } else {
                c->unsubscribe();
            }
            return;
        }
    }

    if (flag) {
        CustomEventSubClient* c = new CustomEventSubClient(uri, event, this);
        c->setDesired(true);
        c->refreshContact(acc_->getContactHeader());
        addSubClient(c);
        if (!canAttemptNow) {
            if (account)
                account->needsResubscribe_.store(true);
            SIP_CORE_WARN("Deferring new event subscription [%.*s] %.*s until account is "
                          "REGISTERED (transportReady=%d, registered=%d)",
                          (int) c->getEvent().size(),
                          c->getEvent().data(),
                          (int) c->getURI().size(),
                          c->getURI().data(),
                          transportReady ? 1 : 0,
                          registered ? 1 : 0);
            return;
        }
        if (!(c->subscribe())) {
            SIP_CORE_WARN("Failed send subscribe.");
            removeSubClient(c);
            delete c;
        }
    }
}

void
SIPEvents::recoverSubscriptions(const std::string& contactHeader)
{
    std::vector<CustomEventSubClient*> subscriptions;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        subscriptions.assign(sub_list_.begin(), sub_list_.end());
    }

    for (auto* sub : subscriptions) {
        if (!sub || !sub->isDesired())
            continue;
        sub->refreshContact(contactHeader);
        if (!sub->subscribe()) {
            SIP_CORE_WARN("Failed to recover event subscription [%.*s] %.*s",
                          (int) sub->getEvent().size(),
                          sub->getEvent().data(),
                          (int) sub->getURI().size(),
                          sub->getURI().data());
        }
    }
}

void
SIPEvents::invalidateSubscriptions(const char* reason)
{
    std::vector<CustomEventSubClient*> subscriptions;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        subscriptions.assign(sub_list_.begin(), sub_list_.end());
    }

    for (auto* sub : subscriptions) {
        if (!sub)
            continue;
        sub->invalidateDialog(reason, true, false);
    }
}

void
SIPEvents::addSubClient(CustomEventSubClient* c)
{
    if (!c)
        return;
    if (std::find(sub_list_.begin(), sub_list_.end(), c) != sub_list_.end())
        return;
    SIP_CORE_DBG("addSubClient added (list[%zu]).", sub_list_.size());
    sub_list_.push_back(c);
}

void
SIPEvents::removeSubClient(CustomEventSubClient* c)
{
    SIP_CORE_DBG("addSubClient removed from the buddy list.");
    sub_list_.remove(c);
}

void
SIPEvents::lock()
{
    mutex_.lock();
}

bool
SIPEvents::tryLock()
{
    return mutex_.try_lock();
}

void
SIPEvents::unlock()
{
    mutex_.unlock();
}

} // namespace sip_core
