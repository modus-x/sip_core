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

#include "sip/sippresence.h"

#include "logger.h"
#include "manager.h"
#include "sip/sipaccount.h"
#include "connectivity/sip_utils.h"
#include "pres_sub_server.h"
#include "pres_sub_client.h"
#include "sip/sipvoiplink.h"
#include "client/ring_signal.h"
#include "connectivity/sip_utils.h"

#include <fmt/core.h>

#include <thread>
#include <sstream>

#define MAX_N_SUB_SERVER 100
#define MAX_N_SUB_CLIENT 100

namespace sip_core {

using sip_utils::CONST_PJ_STR;

SIPPresence::SIPPresence(SIPAccount* acc)
    : publish_sess_()
    , status_data_()
    , enabled_(true)
    , publish_supported_(true)
    , subscribe_supported_(true)
    , status_(false)
    , note_(" ")
    , acc_(acc)
    , sub_server_list_() // IP2IP context
    , sub_client_list_()
    , cp_()
    , pool_()
{
    /* init pool */
    pj_caching_pool_init(&cp_, &pj_pool_factory_default_policy, 0);
    pool_ = pj_pool_create(&cp_.factory, "pres", 1000, 1000, NULL);
    if (!pool_)
        throw std::runtime_error("Could not allocate pool for presence");

    /* init default status */
    updateStatus(false, " ");
}

SIPPresence::~SIPPresence()
{
    /* Flush the lists */
    // FIXME: Can't destroy/unsubscribe buddies properly.
    // Is the transport usable when the account is being destroyed?
    // for (const auto & c : sub_client_list_)
    //    delete(c);
    sub_client_list_.clear();
    sub_server_list_.clear();

    pj_pool_release(pool_);
    pj_caching_pool_destroy(&cp_);
}

SIPAccount*
SIPPresence::getAccount() const
{
    return acc_;
}

pjsip_pres_status*
SIPPresence::getStatus()
{
    return &status_data_;
}

int
SIPPresence::getModId() const
{
    return Manager::instance().sipVoIPLink().getModId();
}

pj_pool_t*
SIPPresence::getPool() const
{
    return pool_;
}

void
SIPPresence::enable(bool enabled)
{
    enabled_ = enabled;
}

void
SIPPresence::support(int function, bool supported)
{
    if (function == PRESENCE_FUNCTION_PUBLISH)
        publish_supported_ = supported;
    else if (function == PRESENCE_FUNCTION_SUBSCRIBE)
        subscribe_supported_ = supported;
}

bool
SIPPresence::isSupported(int function)
{
    if (function == PRESENCE_FUNCTION_PUBLISH)
        return publish_supported_;
    else if (function == PRESENCE_FUNCTION_SUBSCRIBE)
        return subscribe_supported_;

    return false;
}

void
SIPPresence::updateStatus(bool status, const std::string& note)
{
    // char* pj_note  = (char*) pj_pool_alloc(pool_, "50");

    pjrpid_element rpid = {PJRPID_ELEMENT_TYPE_PERSON,
                           CONST_PJ_STR("0"),
                           PJRPID_ACTIVITY_UNKNOWN,
                           CONST_PJ_STR(note)};

    /* fill activity if user not available. */
    if (note == "away")
        rpid.activity = PJRPID_ACTIVITY_AWAY;
    else if (note == "busy")
        rpid.activity = PJRPID_ACTIVITY_BUSY;
    /*
    else // TODO: is there any other possibilities
        SIP_CORE_DBG("Presence : no activity");
    */

    pj_bzero(&status_data_, sizeof(status_data_));
    status_data_.info_cnt = 1;
    status_data_.info[0].basic_open = status;

    // at most we will have 3 digits + NULL termination
    char buf[4];
    pj_utoa(rand() % 1000, buf);
    status_data_.info[0].id = pj_strdup3(pool_, buf);

    pj_memcpy(&status_data_.info[0].rpid, &rpid, sizeof(pjrpid_element));
    /* "contact" field is optionnal */
}

void
SIPPresence::sendPresence(bool status, const std::string& note)
{
    updateStatus(status, note);

    // if ((not publish_supported_) or (not enabled_))
    //    return;

    publish(this); // to the PBX server
}

void
SIPPresence::reportPresSubClientNotification(std::string_view uri, pjsip_pres_status* status)
{
    /* Update our info. See pjsua_buddy_get_info() for additionnal ideas*/
    const std::string& acc_ID = acc_->getAccountID();
    const std::string note(status->info[0].rpid.note.ptr, status->info[0].rpid.note.slen);
    SIP_CORE_DBG(" Received status of PresSubClient %.*s(acc:%s): status=%s note=%s",
                 (int) uri.size(),
                 uri.data(),
                 acc_ID.c_str(),
                 status->info[0].basic_open ? "open" : "closed",
                 note.c_str());

    if (uri == acc_->getFromUri()) {
        // save the status of our own account
        status_ = status->info[0].basic_open;
        note_ = note;
    }
    // report status to client signal
    emitSignal<libsip_core::PresenceSignal::NewBuddyNotification>(acc_ID,
                                                                  std::string(uri),
                                                                  status->info[0].basic_open,
                                                                  note);
}

void
SIPPresence::subscribeClient(const std::string& uri, bool flag)
{
    auto* account = acc_;
    const bool transportReady = account && !account->isTransportRecoveryActive()
                                && static_cast<bool>(account->getTransport());
    const bool registered = account
                            && account->getRegistrationState() == RegistrationState::REGISTERED;
    const bool canAttemptNow = transportReady && registered;
    /* if an account has a server that doesn't support SUBSCRIBE, it's still possible
     * to subscribe to someone on another server */
    /*
    std::string account_host = std::string(pj_gethostname()->ptr, pj_gethostname()->slen);
    std::string sub_host = sip_utils::getHostFromUri(uri);
    if (((not subscribe_supported_) && (account_host == sub_host))
            or (not enabled_))
        return;
    */

    /* Check if the buddy was already subscribed */
    for (const auto& c : sub_client_list_) {
        if (c->getURI() == uri) {
            // SIP_CORE_DBG("-PresSubClient:%s exists in the list. Replace it.", uri.c_str());
            c->setDesired(flag);
            if (flag) {
                c->refreshContact(acc_->getContactHeader());
                if (!canAttemptNow) {
                    if (account)
                        account->needsResubscribe_.store(true);
                    SIP_CORE_WARN("Deferring presence subscription %.*s until account is "
                                  "REGISTERED (transportReady=%d, registered=%d)",
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

    if (sub_client_list_.size() >= MAX_N_SUB_CLIENT) {
        SIP_CORE_WARN("Can't add PresSubClient, max number reached.");
        return;
    }

    if (flag) {
        PresSubClient* c = new PresSubClient(uri, this);
        c->setDesired(true);
        c->refreshContact(acc_->getContactHeader());
        addPresSubClient(c);
        if (!canAttemptNow) {
            if (account)
                account->needsResubscribe_.store(true);
            SIP_CORE_WARN("Deferring new presence subscription %.*s until account is "
                          "REGISTERED (transportReady=%d, registered=%d)",
                          (int) c->getURI().size(),
                          c->getURI().data(),
                          transportReady ? 1 : 0,
                          registered ? 1 : 0);
            return;
        }
        if (!(c->subscribe())) {
            SIP_CORE_WARN("Failed send subscribe.");
            removePresSubClient(c);
            delete c;
        }
    }
}

void
SIPPresence::recoverSubscriptionsAndPublish(const std::string& contactHeader, bool republish)
{
    std::vector<PresSubClient*> subscriptions;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        subscriptions.assign(sub_client_list_.begin(), sub_client_list_.end());
    }

    for (auto* sub : subscriptions) {
        if (!sub || !sub->isDesired())
            continue;
        sub->refreshContact(contactHeader);
        if (!sub->subscribe()) {
            SIP_CORE_WARN("Failed to recover presence subscription for %.*s",
                          (int) sub->getURI().size(),
                          sub->getURI().data());
            // Re-arm: recovery is otherwise one-shot (runPostRegisterRecoverySync
            // already consumed the flag). Retry on the next register-2xx instead
            // of leaving the subscription permanently stale.
            acc_->needsResubscribe_.store(true);
        }
    }

    if (!republish)
        return;

    if (publish_sess_) {
        pjsip_publishc_destroy(publish_sess_);
        publish_sess_ = NULL;
    }

    if (!enabled_ || !publish_supported_) {
        SIP_CORE_DBG("Skipping presence republish for account %s (enabled=%d, supported=%d)",
                     acc_->getAccountID().c_str(),
                     enabled_,
                     publish_supported_);
        return;
    }

    const auto status = publish(this);
    if (status != PJ_SUCCESS) {
        SIP_CORE_WARN("Failed to recover presence publish session for account %s: %d",
                      acc_->getAccountID().c_str(),
                      status);
        // Re-arm: the republish is otherwise one-shot — the flag was already
        // consumed by runPostRegisterRecoverySync(), so a failure here (e.g. the
        // transport is still settling right after a connectivity change) would
        // leave presence dead until an app restart. Retry on the next
        // register-2xx. (Vologda 112 incident, 2026-07-12)
        acc_->needsRepublish_.store(true);
    }
}

void
SIPPresence::invalidateSubscriptionsAndPublish(const char* reason)
{
    std::vector<PresSubClient*> subscriptions;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        subscriptions.assign(sub_client_list_.begin(), sub_client_list_.end());
    }

    for (auto* sub : subscriptions) {
        if (!sub)
            continue;
        sub->invalidateDialog(reason, true, false);
    }

    if (publish_sess_) {
        SIP_CORE_WARN("Invalidating presence publish session for account %s locally (%s)",
                      acc_->getAccountID().c_str(),
                      reason ? reason : "unspecified");
        pjsip_publishc_destroy(publish_sess_);
        publish_sess_ = NULL;
    }
}

void
SIPPresence::addPresSubClient(PresSubClient* c)
{
    if (!c)
        return;
    for (const auto& existing : sub_client_list_) {
        if (existing == c)
            return;
    }
    if (sub_client_list_.size() < MAX_N_SUB_CLIENT) {
        sub_client_list_.push_back(c);
        SIP_CORE_DBG("New Presence_subscription_client added (list[%zu]).", sub_client_list_.size());
    } else {
        SIP_CORE_WARN("Max Presence_subscription_client is reach.");
        // let the client alive //delete c;
    }
}

void
SIPPresence::removePresSubClient(PresSubClient* c)
{
    SIP_CORE_DBG("Remove Presence_subscription_client from the buddy list.");
    sub_client_list_.remove(c);
}

void
SIPPresence::approvePresSubServer(const std::string& uri, bool flag)
{
    for (const auto& s : sub_server_list_) {
        if (s->matches((char*) uri.c_str())) {
            s->approve(flag);
            // return; // 'return' would prevent multiple-time subscribers from spam
        }
    }
}

void
SIPPresence::addPresSubServer(PresSubServer* s)
{
    if (sub_server_list_.size() < MAX_N_SUB_SERVER) {
        sub_server_list_.push_back(s);
    } else {
        SIP_CORE_WARN("Max Presence_subscription_server is reach.");
        // let de server alive // delete s;
    }
}

void
SIPPresence::removePresSubServer(PresSubServer* s)
{
    sub_server_list_.remove(s);
    SIP_CORE_DBG("Presence_subscription_server removed");
}

void
SIPPresence::notifyPresSubServer()
{
    SIP_CORE_DBG("Iterating through IP2IP Presence_subscription_server:");

    for (const auto& s : sub_server_list_)
        s->notify();
}

void
SIPPresence::lock()
{
    mutex_.lock();
}

bool
SIPPresence::tryLock()
{
    return mutex_.try_lock();
}

void
SIPPresence::unlock()
{
    mutex_.unlock();
}

void
SIPPresence::fillDoc(pjsip_tx_data* tdata, const pres_msg_data* msg_data)
{
    if (tdata->msg->type == PJSIP_REQUEST_MSG) {
        constexpr pj_str_t STR_USER_AGENT = CONST_PJ_STR("User-Agent");
        std::string useragent(acc_->getUserAgentName());
        pj_str_t pJuseragent = pj_str((char*) useragent.c_str());
        pjsip_hdr* h = (pjsip_hdr*) pjsip_generic_string_hdr_create(tdata->pool,
                                                                    &STR_USER_AGENT,
                                                                    &pJuseragent);
        pjsip_msg_add_hdr(tdata->msg, h);
    }

    if (msg_data == NULL)
        return;

    const pjsip_hdr* hdr;
    hdr = msg_data->hdr_list.next;

    while (hdr && hdr != &msg_data->hdr_list) {
        pjsip_hdr* new_hdr;
        new_hdr = (pjsip_hdr*) pjsip_hdr_clone(tdata->pool, hdr);
        SIP_CORE_DBG("adding header %p", new_hdr->name.ptr);
        pjsip_msg_add_hdr(tdata->msg, new_hdr);
        hdr = hdr->next;
    }

    if (msg_data->content_type.slen && msg_data->msg_body.slen) {
        pjsip_msg_body* body;
        constexpr pj_str_t type = CONST_PJ_STR("application");
        constexpr pj_str_t subtype = CONST_PJ_STR("pidf+xml");
        body = pjsip_msg_body_create(tdata->pool, &type, &subtype, &msg_data->msg_body);
        tdata->msg->body = body;
    }
}

static const pjsip_publishc_opt my_publish_opt = {true}; // this is queue_request

/*
 * Client presence publication callback.
 */
void
SIPPresence::publish_cb(struct pjsip_publishc_cbparam* param)
{
    SIPPresence* pres = (SIPPresence*) param->token;

    if (param->code / 100 != 2 || param->status != PJ_SUCCESS) {
        pjsip_publishc_destroy(param->pubc);
        pres->publish_sess_ = NULL;
        std::string error = fmt::format("{} / {}", param->code, sip_utils::as_view(param->reason));
        if (param->status != PJ_SUCCESS) {
            char errmsg[PJ_ERR_MSG_SIZE];
            pj_strerror(param->status, errmsg, sizeof(errmsg));
            SIP_CORE_ERR("Client (PUBLISH) failed, status=%d, msg=%s", param->status, errmsg);
            emitSignal<libsip_core::PresenceSignal::ServerError>(pres->getAccount()->getAccountID(),
                                                                 error,
                                                                 errmsg);

        } else if (param->code == 412) {
            /* 412 (Conditional Request Failed)
             * The PUBLISH refresh has failed, retry with new one.
             */
            SIP_CORE_WARN("Publish retry.");
            publish(pres);
        } else if ((param->code == PJSIP_SC_BAD_EVENT)
                   || (param->code == PJSIP_SC_NOT_IMPLEMENTED)) { // 489 or 501
            SIP_CORE_WARN("Client (PUBLISH) failed (%s)", error.c_str());

            emitSignal<libsip_core::PresenceSignal::ServerError>(pres->getAccount()->getAccountID(),
                                                                 error,
                                                                 "Publish not supported.");

            pres->getAccount()->supportPresence(PRESENCE_FUNCTION_PUBLISH, false);
        }

    } else {
        if (param->expiration < 1) {
            /* Could happen if server "forgot" to include Expires header
             * in the response. We will not renew, so destroy the pubc.
             */
            pjsip_publishc_destroy(param->pubc);
            pres->publish_sess_ = NULL;
        }

        pres->getAccount()->supportPresence(PRESENCE_FUNCTION_PUBLISH, true);
    }
}

/*
 * Send PUBLISH request.
 */
pj_status_t
SIPPresence::send_publish(SIPPresence* pres)
{
    pjsip_tx_data* tdata;
    pj_status_t status;

    SIP_CORE_DBG("Send PUBLISH (%s).", pres->getAccount()->getAccountID().c_str());

    SIPAccount* acc = pres->getAccount();
    std::string contactWithAngles = acc->getFromUri();
    contactWithAngles.erase(contactWithAngles.find('>'));
    int semicolon = contactWithAngles.find_first_of(':');
    std::string contactWithoutAngles = contactWithAngles.substr(semicolon + 1);
    //    pj_str_t contact = pj_str(strdup(contactWithoutAngles.c_str()));
    //    pj_memcpy(&status_data.info[0].contact, &contt, sizeof(pj_str_t));;

    /* Create PUBLISH request */
    char* bpos;
    pj_str_t entity;

    status = pjsip_publishc_publish(pres->publish_sess_, PJ_TRUE, &tdata);

    if (!acc->setUpTransmissionData(tdata)) {
        status = PJSIP_SC_TSX_TRANSPORT_ERROR;
        if (pres->publish_sess_) {
            pjsip_publishc_destroy(pres->publish_sess_);
            pres->publish_sess_ = NULL;
        }

        return status;
    }

    pj_str_t from = pj_strdup3(pres->pool_, acc->getFromUri().c_str());

    if (status != PJ_SUCCESS) {
        SIP_CORE_ERR("Error creating PUBLISH request %d", status);
        goto on_error;
    }

    if ((bpos = pj_strchr(&from, '<')) != NULL) {
        char* epos = pj_strchr(&from, '>');

        if (epos - bpos < 2) {
            SIP_CORE_ERR("Unexpected invalid URI");
            status = PJSIP_EINVALIDURI;
            goto on_error;
        }

        entity.ptr = bpos + 1;
        entity.slen = epos - bpos - 1;
    } else {
        entity = from;
    }

    /* Create and add PIDF message body */
    status = pjsip_pres_create_pidf(tdata->pool, pres->getStatus(), &entity, &tdata->msg->body);

    pres_msg_data msg_data;

    if (status != PJ_SUCCESS) {
        SIP_CORE_ERR("Error creating PIDF for PUBLISH request");
        pjsip_tx_data_dec_ref(tdata);
        goto on_error;
    }

    pj_bzero(&msg_data, sizeof(msg_data));
    pj_list_init(&msg_data.hdr_list);
    pjsip_media_type_init(&msg_data.multipart_ctype, NULL, NULL);
    pj_list_init(&msg_data.multipart_parts);

    pres->fillDoc(tdata, &msg_data);

    // Pin the PUBLISH to the account's live transport, exactly like REGISTER
    // (pjsip_regc_set_transport) and the raw keep-alive do. publishc has no
    // set_transport API, so we seed the UAC transaction's selector via the
    // tx_data — pjsip_endpt_send_request() forwards tdata->tp_sel to
    // pjsip_tsx_set_transport(). Without this, after a connectivity change
    // rebinds the account onto a fresh transport, the PUBLISH resolves through
    // the transport manager to the old (destroyed) transport and fails with
    // PJSIP_EUNSUPTRANSPORT forever — presence never recovers until restart
    // (Vologda 112 incident, 2026-07-12). Set it here, right before the send and
    // past every goto on_error, so a failed early step can't leak the transport
    // ref; use pjsip_tx_data_set_transport (not a raw tp_sel assignment) so the
    // ref-count is held for the life of the transaction.
    // Own scope so the earlier `goto on_error` statements route around this
    // declaration instead of jumping across its initialization.
    {
        const pjsip_tpselector tpSel = acc->getTransportSelector();
        if (tpSel.type != PJSIP_TPSELECTOR_NONE)
            pjsip_tx_data_set_transport(tdata, &tpSel);
    }

    /* Send the PUBLISH request */
    status = pjsip_publishc_send(pres->publish_sess_, tdata);

    if (status == PJ_EPENDING) {
        SIP_CORE_WARN("Previous request is in progress, ");
    } else if (status != PJ_SUCCESS) {
        SIP_CORE_ERR("Error sending PUBLISH request");
        goto on_error;
    }

    return PJ_SUCCESS;

on_error:

    if (pres->publish_sess_) {
        pjsip_publishc_destroy(pres->publish_sess_);
        pres->publish_sess_ = NULL;
    }

    return status;
}

/* Create client publish session */
pj_status_t
SIPPresence::publish(SIPPresence* pres)
{
    pj_status_t status;
    constexpr pj_str_t STR_PRESENCE = CONST_PJ_STR("presence");
    SIPAccount* acc = pres->getAccount();
    pjsip_endpoint* endpt = Manager::instance().sipVoIPLink().getEndpoint();

    /* Create and init client publication session */

    /* Create client publication */
    status = pjsip_publishc_create(endpt, &my_publish_opt, pres, &publish_cb, &pres->publish_sess_);

    if (status != PJ_SUCCESS) {
        pres->publish_sess_ = NULL;
        SIP_CORE_ERR("Failed to create a publish session.");
        return status;
    }

    /* Initialize client publication */
    pj_str_t from = pj_strdup3(pres->pool_, acc->getFromUri().c_str());
    status = pjsip_publishc_init(pres->publish_sess_, &STR_PRESENCE, &from, &from, &from, 0xFFFF);

    if (status != PJ_SUCCESS) {
        SIP_CORE_ERR("Failed to init a publish session");
        pres->publish_sess_ = NULL;
        return status;
    }

    /* Add credential for authentication */
    if (acc->hasCredentials()
        and pjsip_publishc_set_credentials(pres->publish_sess_,
                                           acc->getCredentialCount(),
                                           acc->getCredInfo())
                != PJ_SUCCESS) {
        SIP_CORE_ERR("Could not initialize credentials for invite session authentication");
        return status;
    }

    /* Send initial PUBLISH request */
    status = send_publish(pres);

    if (status != PJ_SUCCESS)
        return status;

    return PJ_SUCCESS;
}

} // namespace sip_core
