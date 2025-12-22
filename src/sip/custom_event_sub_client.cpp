/*
 *  Copyright (C) 2012, 2013 LOTES TM LLC
 *  Author : Andrey Loukhnov <aol.nnov@gmail.com>
 *
 *  This file is a part of pult5-voip
 *
 *  pult5-voip is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  pult5-voip is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this programm. If not, see <http://www.gnu.org/licenses/>.
 */

#include <pj/log.h>
#include <pj/rand.h>
#include <pjsip/sip_module.h>
#include <pjsip/sip_types.h>
#include <pjsip/sip_event.h>
#include <pjsip/sip_transaction.h>
#include <pjsip/sip_dialog.h>
#include <pjsip/sip_endpoint.h>
#include <string>
#include <sstream>
#include <thread>
#include <pj/pool.h>
#include <pjsip/sip_ua_layer.h>
#include <pjsip-simple/evsub.h>
#include <unistd.h>

#include "custom_event_sub_client.h"
#include "sip/sipaccount.h"
#include "sip/sipvoiplink.h"
#include "connectivity/sip_utils.h"
#include "manager.h"
#include "client/ring_signal.h"

#include "logger.h"

#define TIMER_MSEC_AFTER_TERMINATE 300

namespace sip_core {

using sip_utils::CONST_PJ_STR;

int CustomEventSubClient::modId_ = 0; // used to extract data structure from event_subscription

void
CustomEventSubClient::client_timer_cb(pj_timer_heap_t* /*th*/, pj_timer_entry* entry)
{
    CustomEventSubClient* c = (CustomEventSubClient*) entry->user_data;
    SIP_CORE_DBG("timeout for %.*s", (int) c->getURI().size(), c->getURI().data());
}

/* Callback called when *client* subscription state has changed. */
void
CustomEventSubClient::client_evsub_on_state(pjsip_evsub* sub, pjsip_event* event)
{
    PJ_UNUSED_ARG(event);

    CustomEventSubClient* event_client = (CustomEventSubClient*) pjsip_evsub_get_mod_data(sub,
                                                                                          modId_);
    /* No need to manager->lock() here since the client has a locked dialog*/

    if (!event_client) {
        SIP_CORE_WARN("event_client not found");
        return;
    }

    SIP_CORE_DBG("Subscription for [%.*s] '%.*s' is '%s'",
                 (int) event_client->getEvent().size(),
                 event_client->getEvent().data(),
                 (int) event_client->getURI().size(),
                 event_client->getURI().data(),
                 pjsip_evsub_get_state_name(sub) ? pjsip_evsub_get_state_name(sub) : "null");

    pjsip_evsub_state state = pjsip_evsub_get_state(sub);

    SIPEvents* manager = event_client->getManager();

    if (state == PJSIP_EVSUB_STATE_ACCEPTED) {
        event_client->enable(true);
        emitSignal<libsip_core::PresenceSignal::SubscriptionStateChanged>(
            manager->getAccount()->getAccountID(),
            std::string(event_client->getURI()),
            std::string(event_client->getEvent()),
            PJ_TRUE);

    } else if (state == PJSIP_EVSUB_STATE_TERMINATED) {
        int resub_delay = -1;
        pj_strdup_with_null(event_client->pool_,
                            &event_client->term_reason_,
                            pjsip_evsub_get_termination_reason(sub));

        emitSignal<libsip_core::PresenceSignal::SubscriptionStateChanged>(
            manager->getAccount()->getAccountID(),
            std::string(event_client->getURI()),
            std::string(event_client->getEvent()),
            PJ_FALSE);

        event_client->term_code_ = 200;

        /* Determine whether to resubscribe automatically */
        if (event && event->type == PJSIP_EVENT_TSX_STATE) {
            const pjsip_transaction* tsx = event->body.tsx_state.tsx;

            if (pjsip_method_cmp(&tsx->method, &pjsip_subscribe_method) == 0) {
                event_client->term_code_ = tsx->status_code;
                std::ostringstream os;
                os << event_client->term_code_;
                const std::string error = os.str() + "/"
                                          + sip_utils::as_view(event_client->term_reason_);

                std::string msg;

                switch (tsx->status_code) {
                case PJSIP_SC_CALL_TSX_DOES_NOT_EXIST:
                    /* 481: we refreshed too late? resubscribe
                     * immediately.
                     */
                    /* But this must only happen when the 481 is received
                     * on subscription refresh request. We MUST NOT try to
                     * resubscribe automatically if the 481 is received
                     * on the initial SUBSCRIBE (if server returns this
                     * response for some reason).
                     */
                    if (event_client->dlg_->remote.contact)
                        resub_delay = 500;
                    msg = "Bad subscribe refresh.";
                    break;

                case PJSIP_SC_NOT_FOUND:
                    msg = "Subscribe context not set for this buddy.";
                    break;

                case PJSIP_SC_FORBIDDEN:
                    msg = "Subscribe not allowed for this buddy.";
                    break;

                case PJSIP_SC_PRECONDITION_FAILURE:
                    msg = "Wrong server.";
                    break;
                }

                //  report error:
                emitSignal<libsip_core::PresenceSignal::ServerError>(
                    event_client->getManager()->getAccount()->getAccountID(), error, msg);

            } else if (pjsip_method_cmp(&tsx->method, &pjsip_notify_method) == 0) {
                if (event_client->isTermReason("deactivated")
                    || event_client->isTermReason("timeout")) {
                    /* deactivated: The subscription has been terminated,
                     * but the subscriber SHOULD retry immediately with
                     * a new subscription.
                     */
                    /* timeout: The subscription has been terminated
                     * because it was not refreshed before it expired.
                     * Clients MAY re-subscribe immediately. The
                     * "retry-after" parameter has no semantics for
                     * "timeout".
                     */
                    resub_delay = 500;
                } else if (event_client->isTermReason("probation")
                           || event_client->isTermReason("giveup")) {
                    /* probation: The subscription has been terminated,
                     * but the client SHOULD retry at some later time.
                     * If a "retry-after" parameter is also present, the
                     * client SHOULD wait at least the number of seconds
                     * specified by that parameter before attempting to re-
                     * subscribe.
                     */
                    /* giveup: The subscription has been terminated because
                     * the notifier could not obtain authorization in a
                     * timely fashion.  If a "retry-after" parameter is
                     * also present, the client SHOULD wait at least the
                     * number of seconds specified by that parameter before
                     * attempting to re-subscribe; otherwise, the client
                     * MAY retry immediately, but will likely get put back
                     * into pending state.
                     */
                    const pjsip_sub_state_hdr* sub_hdr;
                    constexpr pj_str_t sub_state = CONST_PJ_STR("Subscription-State");
                    const pjsip_msg* msg;

                    msg = event->body.tsx_state.src.rdata->msg_info.msg;
                    sub_hdr = (const pjsip_sub_state_hdr*) pjsip_msg_find_hdr_by_name(msg,
                                                                                      &sub_state,
                                                                                      NULL);

                    if (sub_hdr && sub_hdr->retry_after > 0)
                        resub_delay = sub_hdr->retry_after * 1000;
                }
            }
        }

        /* For other cases of subscription termination, if resubscribe
         * timer is not set, schedule with default expiration (plus minus
         * some random value, to avoid sending SUBSCRIBEs all at once)
         */
        if (resub_delay == -1) {
            resub_delay = TIMER_MSEC_AFTER_TERMINATE * 1000;
        }

        event_client->sub_ = sub;
        event_client->rescheduleTimer(PJ_TRUE, resub_delay);

    } else { // state==ACTIVE ......
        // This will clear the last termination code/reason
        event_client->term_code_ = 0;
        event_client->term_reason_.ptr = NULL;
    }

    /* Clear subscription */
    if (pjsip_evsub_get_state(sub) == PJSIP_EVSUB_STATE_TERMINATED) {
        pjsip_evsub_terminate(event_client->sub_, PJ_FALSE); // = NULL;
        event_client->dlg_ = NULL;
        event_client->rescheduleTimer(PJ_FALSE, 0);
        pjsip_evsub_set_mod_data(sub, modId_, NULL);

        event_client->enable(false);
    }
}

/* Callback when transaction state has changed. */
void
CustomEventSubClient::client_evsub_on_tsx_state(pjsip_evsub* sub,
                                                pjsip_transaction* tsx,
                                                pjsip_event* event)
{
    CustomEventSubClient* event_client;
    pjsip_contact_hdr* contact_hdr;

    event_client = (CustomEventSubClient*) pjsip_evsub_get_mod_data(sub, modId_);
    /* No need to manager->lock() here since the client has a locked dialog*/

    if (!event_client) {
        SIP_CORE_WARN("Couldn't find event_client.");
        return;
    }

    /* We only use this to update event_client's Contact, when it's not
     * set.
     */
    if (event_client->contact_.slen != 0) {
        /* Contact already set */
        return;
    }

    /* Only care about 2xx response to outgoing SUBSCRIBE */
    if (tsx->status_code / 100 != 2 || tsx->role != PJSIP_UAC_ROLE
        || event->type != PJSIP_EVENT_RX_MSG
        || pjsip_method_cmp(&tsx->method, pjsip_get_subscribe_method()) != 0) {
        return;
    }

    /* Find contact header. */
    contact_hdr = (pjsip_contact_hdr*) pjsip_msg_find_hdr(event->body.rx_msg.rdata->msg_info.msg,
                                                          PJSIP_H_CONTACT,
                                                          NULL);

    if (!contact_hdr || !contact_hdr->uri) {
        return;
    }

    event_client->contact_.ptr = (char*) pj_pool_alloc(event_client->pool_, PJSIP_MAX_URL_SIZE);
    event_client->contact_.slen = pjsip_uri_print(PJSIP_URI_IN_CONTACT_HDR,
                                                  contact_hdr->uri,
                                                  event_client->contact_.ptr,
                                                  PJSIP_MAX_URL_SIZE);

    if (event_client->contact_.slen < 0)
        event_client->contact_.slen = 0;
}

/* Callback called when we receive NOTIFY */
void
CustomEventSubClient::client_evsub_on_rx_notify(pjsip_evsub* sub,
                                                pjsip_rx_data* rdata,
                                                int* p_st_code,
                                                pj_str_t** p_st_text,
                                                pjsip_hdr* res_hdr,
                                                pjsip_msg_body** p_body)
{
    CustomEventSubClient* event_client = (CustomEventSubClient*) pjsip_evsub_get_mod_data(sub,
                                                                                          modId_);

    if (!event_client) {
        SIP_CORE_WARN("Couldn't find event_client from ev_sub.");
        return;
    }
    /* No need to manager->lock() here since the client has a locked dialog*/
    auto body = rdata->msg_info.msg->body;

    if (body && body->len > 0) {
        void* clonedData = body->clone_data(event_client->pool_, body->data, body->len);
        if (clonedData) {
            // Convert the cloned data to a C++ string
            std::string result(static_cast<char*>(clonedData), body->len);
            emitSignal<libsip_core::PresenceSignal::NotifyReceived>(
                event_client->getManager()->getAccount()->getAccountID(),
                std::string(event_client->getURI()),
                std::string(event_client->getEvent()),
                result);
        }
    } else {
        SIP_CORE_WARN("No notify body.");
    }

    /* The default is to send 200 response to NOTIFY.
     * Just leave it there..
     */
    PJ_UNUSED_ARG(p_st_code);
    PJ_UNUSED_ARG(p_st_text);
    PJ_UNUSED_ARG(res_hdr);
    PJ_UNUSED_ARG(p_body);
}

CustomEventSubClient::CustomEventSubClient(const std::string& uri,
                                           const std::string& event,
                                           SIPEvents* manager)
    : manager_(manager)
    , uri_ {0, 0}
    , contact_ {0, 0}
    , event_ {0, 0}
    , display_()
    , dlg_(NULL)
    , monitored_(false)
    , name_()
    , cp_()
    , pool_(0)
    , sub_(NULL)
    , term_code_(0)
    , term_reason_()
    , timer_()
    , user_data_(NULL)
    , lock_count_(0)
    , lock_flag_(0)
{
    pj_caching_pool_init(&cp_, &pj_pool_factory_default_policy, 0);
    pool_ = pj_pool_create(&cp_.factory, "Events_sub_client", 512, 512, NULL);
    uri_ = pj_strdup3(pool_, uri.c_str());
    event_ = pj_strdup3(pool_, event.c_str());
    contact_ = pj_strdup3(pool_, manager_->getAccount()->getContactHeader().c_str());
}

CustomEventSubClient::~CustomEventSubClient()
{
    SIP_CORE_DBG("Destroying event_client object with uri %.*s", (int) uri_.slen, uri_.ptr);
    rescheduleTimer(PJ_FALSE, 0);
    unsubscribe();
    pj_pool_release(pool_);
}

bool
CustomEventSubClient::isSubscribed()
{
    return monitored_;
}

std::string_view
CustomEventSubClient::getURI()
{
    return {uri_.ptr, (size_t) uri_.slen};
}

SIPEvents*
CustomEventSubClient::getManager()
{
    return manager_;
}

const std::string&
CustomEventSubClient::getLastMessage() const
{
    return last_message_;
}

std::string_view
CustomEventSubClient::getEvent()
{
    return {event_.ptr, (size_t) event_.slen};
}

bool
CustomEventSubClient::isTermReason(const std::string& reason)
{
    const std::string_view myReason(term_reason_.ptr, (size_t) term_reason_.slen);
    return not myReason.compare(reason);
}

void
CustomEventSubClient::rescheduleTimer(bool reschedule, unsigned msec)
{
    if (timer_.id) {
        pjsip_endpt_cancel_timer(Manager::instance().sipVoIPLink().getEndpoint(), &timer_);
        timer_.id = PJ_FALSE;
    }

    if (reschedule) {
        pj_time_val delay;

        SIP_CORE_WARN("event_client  %.*s will resubscribe in %u ms (reason: %.*s)",
                      (int) uri_.slen,
                      uri_.ptr,
                      msec,
                      (int) term_reason_.slen,
                      term_reason_.ptr);
        pj_timer_entry_init(&timer_, 0, this, &client_timer_cb);
        delay.sec = 0;
        delay.msec = msec;
        pj_time_val_normalize(&delay);

        if (pjsip_endpt_schedule_timer(Manager::instance().sipVoIPLink().getEndpoint(),
                                       &timer_,
                                       &delay)
            == PJ_SUCCESS) {
            timer_.id = PJ_TRUE;
        }
    }
}

void
CustomEventSubClient::enable(bool flag)
{
    SIP_CORE_DBG("event_client [%.*s] %.*s is %s monitored.",
                 (int) getEvent().size(),
                 getEvent().data(),
                 (int) getURI().size(),
                 getURI().data(),
                 flag ? "" : "NOT");
    if (flag and not monitored_)
        manager_->addSubClient(this);
    monitored_ = flag;
}

bool
CustomEventSubClient::lock()
{
    unsigned i;

    for (i = 0; i < 50; i++) {
        if (not manager_->tryLock()) {
            // FIXME: i/10 in ms, sure!?
            std::this_thread::sleep_for(std::chrono::milliseconds(i / 10));
            continue;
        }
        lock_flag_ = SIP_EVENTS_LOCK_FLAG;

        if (dlg_ == NULL) {
            return true;
        }

        if (pjsip_dlg_try_inc_lock(dlg_) != PJ_SUCCESS) {
            lock_flag_ = 0;
            manager_->unlock();
            // FIXME: i/10 in ms, sure!?
            std::this_thread::sleep_for(std::chrono::milliseconds(i / 10));
            continue;
        }

        lock_flag_ = SIP_EVENTS_CLIENT_LOCK_FLAG;
        manager_->unlock();
        break;
    }

    if (lock_flag_ == 0) {
        SIP_CORE_DBG("event_client failed to lock : timeout");
        return false;
    }
    return true;
}

void
CustomEventSubClient::unlock()
{
    if (lock_flag_ & SIP_EVENTS_CLIENT_LOCK_FLAG)
        pjsip_dlg_dec_lock(dlg_);

    if (lock_flag_ & SIP_EVENTS_LOCK_FLAG)
        manager_->unlock();
}

bool
CustomEventSubClient::unsubscribe()
{
    if (not lock())
        return false;

    monitored_ = false;

    pjsip_tx_data* tdata;
    pj_status_t retStatus;

    if (sub_ == NULL or dlg_ == NULL) {
        SIP_CORE_WARN("CustomEventSubClient already unsubscribed. Sending result to client.");
        unlock();
        emitSignal<libsip_core::PresenceSignal::SubscriptionStateChanged>(manager_->getAccount()
                                                                              ->getAccountID(),
                                                                          std::string(getURI()),
                                                                          std::string(getEvent()),
                                                                          PJ_FALSE);

        return false;
    }

    if (pjsip_evsub_get_state(sub_) == PJSIP_EVSUB_STATE_TERMINATED) {
        SIP_CORE_WARN(
            "event_client already unsubscribed sub=TERMINATED. Sending result to client.");
        sub_ = NULL;
        unlock();
        emitSignal<libsip_core::PresenceSignal::SubscriptionStateChanged>(manager_->getAccount()
                                                                              ->getAccountID(),
                                                                          std::string(getURI()),
                                                                          std::string(getEvent()),
                                                                          PJ_FALSE);

        return false;
    }

    /* Unsubscribe means send a subscribe with timeout=0s*/
    SIP_CORE_WARN("event_client %.*s: unsubscribing..", (int) uri_.slen, uri_.ptr);
    retStatus = pjsip_evsub_initiate(sub_, pjsip_get_subscribe_method(), 0, &tdata);

    if (retStatus == PJ_SUCCESS) {
        // Add user-agent header
        sip_utils::addUserAgentHeader(manager_->getAccount()->getUserAgentName(), tdata);

        retStatus = pjsip_evsub_send_request(sub_, tdata);
    }

    if (retStatus != PJ_SUCCESS and sub_) {
        pjsip_evsub_terminate(sub_, PJ_FALSE);
        sub_ = NULL;
        SIP_CORE_WARN("Unable to unsubscribe sip events (%d)", retStatus);
        unlock();
        return false;
    }

    unlock();
    return true;
}

bool
CustomEventSubClient::subscribe()
{
    if (sub_ and dlg_) { // do not bother if already subscribed
        pjsip_evsub_terminate(sub_, PJ_FALSE);
        emitSignal<libsip_core::PresenceSignal::SubscriptionStateChanged>(manager_->getAccount()
                                                                              ->getAccountID(),
                                                                          std::string(getURI()),
                                                                          std::string(getEvent()),
                                                                          PJ_TRUE);
        SIP_CORE_DBG("CustomEventSubClient %.*s: already subscribed. Refresh it.",
                     (int) uri_.slen,
                     uri_.ptr);
    }

    // subscribe
    pjsip_evsub_user event_callback;
    pjsip_tx_data* tdata;
    pj_status_t status;

    /* Event subscription callback. */
    pj_bzero(&event_callback, sizeof(event_callback));
    event_callback.on_evsub_state = &client_evsub_on_state;
    event_callback.on_tsx_state = &client_evsub_on_tsx_state;
    event_callback.on_rx_notify = &client_evsub_on_rx_notify;

    SIPAccount* acc = manager_->getAccount();
    SIP_CORE_DBG("CustomEventSubClient [%.*s] %.*s => subscribing ",
                 (int) event_.slen,
                 event_.ptr,
                 (int) uri_.slen,
                 uri_.ptr);

    /* Create UAC dialog */
    pj_str_t from = pj_strdup3(pool_, acc->getFromUri().c_str());
    status = pjsip_dlg_create_uac(pjsip_ua_instance(), &from, &contact_, &uri_, NULL, &dlg_);

    if (status != PJ_SUCCESS) {
        emitSignal<libsip_core::PresenceSignal::SubscriptionStateChanged>(manager_->getAccount()
                                                                              ->getAccountID(),
                                                                          std::string(getURI()),
                                                                          std::string(getEvent()),
                                                                          PJ_FALSE);
        SIP_CORE_ERR("Unable to create dialog \n");
        return false;
    }

    /* Add credential for auth. */
    if (acc->hasCredentials()
        and pjsip_auth_clt_set_credentials(&dlg_->auth_sess,
                                           acc->getCredentialCount(),
                                           acc->getCredInfo())
                != PJ_SUCCESS) {
        emitSignal<libsip_core::PresenceSignal::SubscriptionStateChanged>(manager_->getAccount()
                                                                              ->getAccountID(),
                                                                          std::string(getURI()),
                                                                          std::string(getEvent()),
                                                                          PJ_FALSE);
        SIP_CORE_ERR("Could not initialize credentials for subscribe session authentication");
    }

    /* Increment the dialog's lock otherwise when sip events session creation
     * fails the dialog will be destroyed prematurely.
     */
    pjsip_dlg_inc_lock(dlg_);

    const pjsip_tpselector tp_sel = acc->getTransportSelector();
    if (pjsip_dlg_set_transport(dlg_, &tp_sel) != PJ_SUCCESS) {
        sub_ = NULL;
        SIP_CORE_ERR("Unable to associate transport for invite session dialog");
        emitSignal<libsip_core::PresenceSignal::SubscriptionStateChanged>(acc->getAccountID(),
                                                                          std::string(getURI()),
                                                                          std::string(getEvent()),
                                                                          PJ_FALSE);
        if (dlg_) {
            pjsip_dlg_dec_lock(dlg_);
        }
        return false;
    }

    /* Create event subscription */
    status = pjsip_evsub_create_uac(dlg_, &event_callback, &event_, PJSIP_EVSUB_NO_EVENT_ID, &sub_);

    if (status != PJ_SUCCESS) {
        sub_ = NULL;
        SIP_CORE_WARN("Unable to create sip events client (%d)", status);

        emitSignal<libsip_core::PresenceSignal::SubscriptionStateChanged>(manager_->getAccount()
                                                                              ->getAccountID(),
                                                                          std::string(getURI()),
                                                                          std::string(getEvent()),
                                                                          PJ_FALSE);

        /* This should destroy the dialog since there's no session
         * referencing it
         */
        if (dlg_) {
            pjsip_dlg_dec_lock(dlg_);
        }

        return false;
    }

    /* Add credential for authentication */
    if (acc->hasCredentials()
        and pjsip_auth_clt_set_credentials(&dlg_->auth_sess,
                                           acc->getCredentialCount(),
                                           acc->getCredInfo())
                != PJ_SUCCESS) {
        SIP_CORE_ERR("Could not initialize credentials for invite session authentication");
        emitSignal<libsip_core::PresenceSignal::SubscriptionStateChanged>(manager_->getAccount()
                                                                              ->getAccountID(),
                                                                          std::string(getURI()),
                                                                          std::string(getEvent()),
                                                                          PJ_FALSE);
        if (dlg_) {
            pjsip_dlg_dec_lock(dlg_);
        }
        return false;
    }

    /* Set route-set */
    if (!acc->getActiveServiceRoute().empty())
        pjsip_dlg_set_route_set(dlg_, sip_utils::createRouteSet(acc->getActiveServiceRoute(), pool_));

    // attach the client data to the sub
    pjsip_evsub_set_mod_data(sub_, modId_, this);

    status = pjsip_evsub_initiate(sub_, pjsip_get_subscribe_method(), -1, &tdata);

    if (status != PJ_SUCCESS) {
        if (dlg_)
            pjsip_dlg_dec_lock(dlg_);
        if (sub_)
            pjsip_evsub_terminate(sub_, PJ_FALSE);
        sub_ = NULL;
        emitSignal<libsip_core::PresenceSignal::SubscriptionStateChanged>(manager_->getAccount()
                                                                              ->getAccountID(),
                                                                          std::string(getURI()),
                                                                          std::string(getEvent()),
                                                                          PJ_FALSE);
        SIP_CORE_WARN("Unable to create initial SUBSCRIBE (%d)", status);
        return false;
    }

    sip_utils::addUserAgentHeader(manager_->getAccount()->getUserAgentName(), tdata);

    status = pjsip_evsub_send_request(sub_, tdata);

    if (status != PJ_SUCCESS) {
        if (dlg_)
            pjsip_dlg_dec_lock(dlg_);
        if (sub_)
            pjsip_evsub_terminate(sub_, PJ_FALSE);
        sub_ = NULL;
        emitSignal<libsip_core::PresenceSignal::SubscriptionStateChanged>(manager_->getAccount()
                                                                              ->getAccountID(),
                                                                          std::string(getURI()),
                                                                          std::string(getEvent()),
                                                                          PJ_FALSE);
        SIP_CORE_WARN("Unable to send initial SUBSCRIBE (%d)", status);
        return false;
    }

    pjsip_dlg_dec_lock(dlg_);
    return true;
}

bool
CustomEventSubClient::match(CustomEventSubClient* b)
{
    return (b->getURI() == getURI());
}

} // namespace sip_core