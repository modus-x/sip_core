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
    /* Check if the buddy was already subscribed */
    for (const auto& c : sub_list_) {
        if (c->getURI() == uri && c->getEvent() == event) {
            if (flag)
                c->subscribe();
            else
                c->unsubscribe();
            return;
        }
    }

    if (flag) {
        CustomEventSubClient* c = new CustomEventSubClient(uri, event, this);
        if (!(c->subscribe())) {
            SIP_CORE_WARN("Failed send subscribe.");
            delete c;
        }
        // the uri has to be accepted before being added in the list
    }
}

void
SIPEvents::addSubClient(CustomEventSubClient* c)
{
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
