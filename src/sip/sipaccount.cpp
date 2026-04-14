/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
 *  Author: Emmanuel Milou <emmanuel.milou@savoirfairelinux.com>
 *  Author: Pierre-Luc Bacon <pierre-luc.bacon@savoirfairelinux.com>
 *  Author: Guillaume Roguez <guillaume.roguez@savoirfairelinux.com>
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

#include "sip/sipaccount.h"
#include "sip/pres_sub_client.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "compiler_intrinsics.h"

#include "sdp.h"
#include "sip/sipvoiplink.h"
#include "sip/sipcall.h"
#include "connectivity/sip_utils.h"

#include "call_factory.h"

#include "sip/sippresence.h"
#include "sip/sipevents.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include <yaml-cpp/yaml.h>
#pragma GCC diagnostic pop

#include "account_schema.h"
#include "config/yamlparser.h"
#include "logger.h"
#include "manager.h"
#include "client/ring_signal.h"
#include "sip_core/account_const.h"

#ifdef ENABLE_VIDEO
#include "libav_utils.h"
#endif

#include "system_codec_container.h"

#include "connectivity/ip_utils.h"
#include "string_utils.h"

#include "im/instant_messaging.h"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <memory>
#include <sstream>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <ctime>
#include <charconv>

#include "pj/string.h"
#include <pjsip.h>
#include <pjsip/sip_config.h>

#ifdef _WIN32
#include <lmcons.h>
#else
#include <pwd.h>
#include "sipaccount.h"
#endif

namespace sip_core {

using yaml_utils::parseValue;
using yaml_utils::parseValueOptional;
using sip_utils::CONST_PJ_STR;

static constexpr unsigned REGISTRATION_FIRST_RETRY_INTERVAL = 25; // seconds
static constexpr unsigned REGISTRATION_RETRY_INTERVAL = 50;       // seconds
static constexpr int64_t TRANSPORT_RECOVERY_DEBOUNCE_MS = 1500;
static constexpr size_t OPTIONS_RECOVERY_MAX_ATTEMPTS = 3;
static constexpr int64_t OPTIONS_RECOVERY_WINDOW_MS = 60 * 1000;
static constexpr size_t RAW_KEEPALIVE_RECOVERY_MAX_ATTEMPTS = 3;
static constexpr int64_t RAW_KEEPALIVE_RECOVERY_WINDOW_MS = 60 * 1000;
static constexpr uint32_t MAIN_ROUTE_FAST_PROBE_INTERVAL_SEC = 1;
static constexpr uint32_t ACTIVE_NO_ROUTE_FAST_PROBE_INTERVAL_SEC = 1;

// keep-alive const values
static constexpr pj_str_t KA_DATA = CONST_PJ_STR("");

static pjsip_transport_type_e
transportTypeFromConfig(libsip_core::TransportType transportType)
{
    switch (transportType) {
    case libsip_core::TransportType::TCP:
        return PJSIP_TRANSPORT_TCP;
    case libsip_core::TransportType::UDP:
    default:
        return PJSIP_TRANSPORT_UDP;
    }
}

static char*
randomSvAuthString(int length)
{
    const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    const int charsetLength = strlen(charset);
    char* result = new char[length + 1]; // Add 1 for null terminator
    srand(time(0));
    for (int i = 0; i < length; ++i) {
        result[i] = charset[rand() % charsetLength];
    }
    result[length] = '\0'; // Add null terminator
    return result;
}

/* Keep alive timer callback */
static void
keep_alive_on_complete(void* token, pjsip_event* event)
{
    auto* acc = static_cast<SIPAccount*>(token);
    if (!acc || !event || event->type != PJSIP_EVENT_TSX_STATE || !event->body.tsx_state.tsx)
        return;

    const auto* tsx = event->body.tsx_state.tsx;
    const bool finalState = tsx->state == PJSIP_TSX_STATE_COMPLETED
                            || tsx->state == PJSIP_TSX_STATE_TERMINATED;
    if (!finalState)
        return;

    const int code = tsx->status_code;
    SIP_CORE_INFO("KA_EVT: options-final route=active code=%d", code);

    if (acc->isOptionsSuccess200(code)) {
        acc->handleActiveRouteOptionsSuccess(code);
    } else if (acc->shouldEnableActiveNoRouteFastProbeForStatusCode(code)) {
        acc->handleActiveRouteOptionsFailure(code);
    }

    acc->ka_options_pending_ = false;
}

/* Backup route keep-alive completion callback */
static void
backup_keep_alive_on_complete(void* token, pjsip_event* event)
{
    auto* acc = static_cast<SIPAccount*>(token);
    if (!acc || !event || event->type != PJSIP_EVENT_TSX_STATE || !event->body.tsx_state.tsx)
        return;

    const auto* tsx = event->body.tsx_state.tsx;
    const bool finalState = tsx->state == PJSIP_TSX_STATE_COMPLETED
                            || tsx->state == PJSIP_TSX_STATE_TERMINATED;
    if (!finalState)
        return;

    const int code = tsx->status_code;
    SIP_CORE_INFO("KA_EVT: options-final route=backup-probe code=%d", code);
    if (acc->isTransportFailureFromOptions(code)) {
        SIP_CORE_WARN("Backup route KA: transport failure during probe (code=%d), recovering "
                      "transport",
                      code);
        acc->scheduleTransportRecovery("backup-route-options-transport-failure", code);
    } else if (acc->isRouteFailureFromOptions(code)) {
        SIP_CORE_DBG("Backup route KA: probe failed with code %d", code);
    }

    acc->ka_backup_route_options_pending_ = false;
}

/* Main route keep alive completion callback - checks if main route is available */
static void
main_route_keep_alive_on_complete(void* token, pjsip_event* event)
{
    auto* acc = static_cast<SIPAccount*>(token);
    if (!acc || !event || event->type != PJSIP_EVENT_TSX_STATE || !event->body.tsx_state.tsx)
        return;

    const auto* tsx = event->body.tsx_state.tsx;
    const bool finalState = tsx->state == PJSIP_TSX_STATE_COMPLETED
                            || tsx->state == PJSIP_TSX_STATE_TERMINATED;
    if (!finalState)
        return;

    const int code = tsx->status_code;
    SIP_CORE_INFO("KA_EVT: options-final route=main-probe code=%d", code);
    if (acc->isOptionsSuccess200(code)) {
        acc->mainRouteAvailable_.store(true);
        if (acc->getRegistrationState() == RegistrationState::TRYING) {
            SIP_CORE_DBG("Main route KA: registration already in progress, deferring failback");
            acc->enableMainRouteFastProbe("main-probe-200-register-in-progress");
        } else {
            acc->disableMainRouteFastProbe("main-probe-200");
            SIP_CORE_WARN("Main route keep-alive succeeded with 200, switching back to main route");
            acc->switchRouteAndReregister(false, "main-route-options-success", true);
        }
    } else if (acc->shouldEnableMainRouteFastProbeForStatusCode(code)) {
        acc->mainRouteAvailable_.store(false);
        acc->enableMainRouteFastProbe("main-probe-non-200");
        SIP_CORE_DBG("Main route keep-alive failed with code %d, staying on backup", code);
    }

    acc->ka_main_route_options_pending_ = false;
}

static void
startup_main_route_probe_on_complete(void* token, pjsip_event* event)
{
    auto* acc = static_cast<SIPAccount*>(token);
    if (!acc || !event || event->type != PJSIP_EVENT_TSX_STATE || !event->body.tsx_state.tsx)
        return;

    const auto* tsx = event->body.tsx_state.tsx;
    const bool finalState = tsx->state == PJSIP_TSX_STATE_COMPLETED
                            || tsx->state == PJSIP_TSX_STATE_TERMINATED;
    if (!finalState)
        return;

    acc->startupMainRouteProbePending_.store(false);

    const int code = tsx->status_code;
    SIP_CORE_INFO("KA_EVT: options-final route=startup-main-probe code=%d", code);
    acc->handleStartupMainRouteProbeResult(code);
}

/* Main route keep alive timer callback - sends OPTIONS to main route */
static void
main_route_keep_alive_timer_cb(pj_timer_heap_t* th, pj_timer_entry* te)
{
    SIPAccount* acc;
    pj_time_val delay;
    pj_status_t status;

    PJ_UNUSED_ARG(th);

    te->id = PJ_FALSE;

    acc = (SIPAccount*) te->user_data;

    // Only continue if we're using backup route
    if (!acc->isUsingBackupRoute() || !acc->hasServiceRoute()) {
        SIP_CORE_DBG("Main route KA: Not using backup route, stopping main route keep-alive");
        return;
    }

    auto transport = acc->getTransport();
    if (transport == NULL) {
        SIP_CORE_ERR("Main route KA: no transport available");
        return;
    }

    // Check if target is available
    if (acc->kaMainRoute.length == 0) {
        SIP_CORE_ERR("Main route KA: no target available");
        return;
    }

    const unsigned targetLen = pj_sockaddr_get_len(&acc->kaMainRoute.socket);
    if (targetLen == 0) {
        SIP_CORE_ERR("Main route KA: invalid target length");
        return;
    }
    acc->kaMainRoute.length = targetLen;

    // Send OPTIONS keep-alive to main route
    if (true) {
        // Avoid sending a new OPTIONS if one is already in flight
        if (acc->ka_main_route_options_pending_) {
            SIP_CORE_WARN("Main route KA: OPTIONS already in flight, skipping");
        } else {
            acc->ka_main_route_options_pending_ = true;

            pjsip_tx_data* tdata = nullptr;
            auto server_uri = acc->getMainRouteKeepAliveUri();
            pj_str_t pjServer = sip_utils::CONST_PJ_STR(server_uri);
            auto contact = acc->getContactHeader();
            pj_str_t pjContact = sip_utils::CONST_PJ_STR(contact);

            status = pjsip_endpt_create_request(acc->getVoipLink().getEndpoint(),
                                                &pjsip_options_method,
                                                &pjServer,
                                                &pjContact,
                                                &pjServer,
                                                &pjContact,
                                                nullptr,
                                                -1,
                                                nullptr,
                                                &tdata);

            if (status == PJ_SUCCESS) {
                auto ip = acc->getServiceRouteIp();
                if (!ip || !acc->setUpTransmissionData(tdata, ip)) {
                    SIP_CORE_ERR("Main route KA: Unable to setup OPTIONS destination");
                    pjsip_tx_data_dec_ref(tdata);
                    acc->mainRouteAvailable_.store(false);
                    acc->enableMainRouteFastProbe("main-probe-setup-failure");
                    acc->ka_main_route_options_pending_ = false;
                    goto reschedule_timer;
                }

                status = pjsip_endpt_send_request(acc->getVoipLink().getEndpoint(),
                                                  tdata,
                                                  -1,
                                                  (void*) acc,
                                                  &main_route_keep_alive_on_complete);

                if (status == PJ_SUCCESS) {
                    SIP_CORE_DBG("Main route KA: OPTIONS sent to main route");
                    SIP_CORE_INFO("KA_EVT: options-sent route=main-probe");
                } else {
                    SIP_CORE_ERR("Main route KA: Error sending OPTIONS: %d", status);
                    acc->mainRouteAvailable_.store(false);
                    acc->enableMainRouteFastProbe("main-probe-send-failure");
                    acc->ka_main_route_options_pending_ = false;
                }
            } else {
                SIP_CORE_ERR("Main route KA: Error creating OPTIONS: %d", status);
                acc->mainRouteAvailable_.store(false);
                acc->enableMainRouteFastProbe("main-probe-create-failure");
                acc->ka_main_route_options_pending_ = false;
            }
        }
    }

reschedule_timer:
    uint32_t seconds = acc->getMainRouteProbeIntervalSec();
    if (seconds == 0) {
        SIP_CORE_INFO("Main route KA: 0 seconds is set, skipping");
        return;
    }

    delay.sec = seconds;
    delay.msec = 0;

    /* Reschedule next timer */
    status = pjsip_endpt_schedule_timer(acc->getVoipLink().getEndpoint(), te, &delay);
    if (status == PJ_SUCCESS) {
        te->id = PJ_TRUE;
        SIP_CORE_INFO("KA_EVT: main-probe-rescheduled interval_sec=%u fast=%d",
                      seconds,
                      acc->isMainRouteFastProbeEnabled() ? 1 : 0);
    } else {
        SIP_CORE_ERROR("Main route KA: Error rescheduling timer: %d", status);
    }
}

/* Backup route keep alive timer callback - sends OPTIONS to backup route */
static void
backup_route_keep_alive_timer_cb(pj_timer_heap_t* th, pj_timer_entry* te)
{
    SIPAccount* acc;
    pj_time_val delay;
    pj_status_t status;

    PJ_UNUSED_ARG(th);

    te->id = PJ_FALSE;

    acc = (SIPAccount*) te->user_data;

    if (!acc) {
        return;
    }

    // Do not run while backup route is in use
    if (acc->isUsingBackupRoute()) {
        SIP_CORE_DBG("Backup route KA: currently using backup route, skipping");
        return;
    }

    if (!acc->hasBackServiceRoute()) {
        SIP_CORE_DBG("Backup route KA: no backup route configured");
        return;
    }

    if (!acc->sendBackupRouteKeepAlive()) {
        SIP_CORE_DBG("Backup route KA: send attempt skipped or failed");
    }

    uint32_t seconds = acc->config().keepAliveInterval;
    if (seconds == 0) {
        SIP_CORE_INFO("Backup route KA: 0 seconds is set, skipping");
        return;
    }

    delay.sec = seconds;
    delay.msec = 0;

    status = pjsip_endpt_schedule_timer(acc->getVoipLink().getEndpoint(), te, &delay);
    if (status == PJ_SUCCESS) {
        te->id = PJ_TRUE;
    } else {
        SIP_CORE_ERROR("Backup route KA: Error rescheduling timer: %d", status);
    }
}

/* Keep alive timer callback */
static void
keep_alive_timer_cb(pj_timer_heap_t* th, pj_timer_entry* te)
{
    SIPAccount* acc;
    pj_time_val delay;
    char addrtxt[PJ_INET6_ADDRSTRLEN];
    pj_status_t status = PJ_SUCCESS;

    PJ_UNUSED_ARG(th);

    te->id = PJ_FALSE;

    acc = (SIPAccount*) te->user_data;

    auto contactHeader = acc->getContactHeader();

    auto transport = acc->getTransport();
    const unsigned targetLen = pj_sockaddr_get_len(&acc->kaTarget.socket);
    const unsigned maxUdpAddrLen = sizeof(pj_sockaddr_in);
    const unsigned actualLen = acc->getActualIpAddress().getLength();
    bool skipSend = false;

    /* Check if the account is still active. It might have just been deleted
     * while the keep-alive timer was about to be called (race condition).
     */
    if (transport == NULL) {
        SIP_CORE_ERR() << "KA: no transport is available for contact " << contactHeader;
        acc->setRegistrationState(RegistrationState::ERROR_GENERIC, PJSIP_SC_TSX_TRANSPORT_ERROR);
        return;
    }

    // Skip keep-alive during connectivity recovery — transport may be stale
    if (acc->connectivityRecoveryInProgress_.load()) {
        SIP_CORE_DBG("KA: skipping keep-alive during connectivity recovery for %s",
                     contactHeader.c_str());
        // Reschedule so we resume once recovery is done
        uint32_t seconds = acc->getActiveKeepAliveIntervalSec();
        if (seconds > 0) {
            delay.sec = seconds;
            delay.msec = 0;
            status = pjsip_endpt_schedule_timer(acc->getVoipLink().getEndpoint(), te, &delay);
            if (status == PJ_SUCCESS)
                te->id = PJ_TRUE;
        }
        return;
    }

    auto transportType = acc->getTransportType();
    const bool isUdp = transportType == PJSIP_TRANSPORT_UDP;
    const bool isTcp = transportType == PJSIP_TRANSPORT_TCP;
    if (!isUdp && !isTcp) {
        SIP_CORE_DEBUG("KA: transport type %d not supported for keep-alive", transportType);
        return;
    }

    // check if target is available
    if (acc->kaTarget.length == 0) {
        SIP_CORE_ERR() << "KA: no target is available for contact " << contactHeader;
        return;
    }

    if (targetLen == 0) {
        SIP_CORE_ERR() << "KA: target address has zero length for contact " << contactHeader;
        return;
    }

    // Clamp incorrect length to the real sockaddr size. Passing an oversized
    // length (e.g., IPv6 sockaddr with an IPv4-only pj_ioqueue build) into
    // pj_ioqueue_sendto triggers a PJ_ASSERT on iOS.
    if (targetLen != acc->kaTarget.length) {
        SIP_CORE_WARN("KA: correcting target length from %u to %u for %s",
                      acc->kaTarget.length,
                      targetLen,
                      contactHeader.c_str());
        acc->kaTarget.length = targetLen;
    }

    const pjsip_tpselector tp_sel = acc->getTransportSelector();

    /* Send keep-alive packet options */
    const bool noRouteMode = acc->isNoRouteKeepAliveMode();
    const bool forcedOptions = !isUdp && acc->config().keepAliveType != KeepAliveType::Options;
    const bool useOptions = SIPAccount::shouldUseOptionsForKeepAlive(acc->config().keepAliveType,
                                                                     isUdp);
    bool rawUdpKeepAliveSendAttempted = false;

    // Avoid crashing inside pj_ioqueue_sendto on IPv6 destinations when we're
    // bound to an IPv4 UDP transport.
    if (isUdp && targetLen > maxUdpAddrLen) {
        SIP_CORE_WARN("KA: UDP target is IPv6-sized (%u bytes), skipping keep-alive to avoid "
                      "pj_ioqueue_sendto assert",
                      targetLen);
        skipSend = true;
    }
    if (isUdp && actualLen > maxUdpAddrLen) {
        SIP_CORE_WARN("KA: account destination is IPv6-sized (%u bytes) on UDP transport, skipping "
                      "keep-alive to avoid pj_ioqueue_sendto assert",
                      actualLen);
        skipSend = true;
    }

    if (!skipSend && useOptions) {
        if (forcedOptions) {
            SIP_CORE_DEBUG("KA: TCP transport detected, forcing SIP OPTIONS keep-alive");
        }
        // Avoid sending a new OPTIONS keep-alive if one is already in flight
        if (acc->ka_options_pending_) {
            SIP_CORE_DEBUG("KA: OPTIONS keep-alive skipped because previous is pending");
            status = PJ_EPENDING;
        } else {
            /* Send SIP Options packet */
            pjsip_tx_data* tdata;

            std::string srvUri(acc->getActiveKeepAliveUri());
            pj_str_t pjSrv {(char*) srvUri.data(), (pj_ssize_t) srvUri.size()};

            pj_str_t pjContact {(char*) contactHeader.data(), (pj_ssize_t) contactHeader.size()};

            SIP_CORE_DEBUG("KA: Sending OPTIONS keep-alive message for acc {:s} to {:s}",
                           contactHeader,
                           srvUri);

            status = pjsip_endpt_create_request(acc->getVoipLink().getEndpoint(),
                                                &pjsip_options_method,
                                                &pjSrv,
                                                &pjContact,
                                                &pjContact,
                                                NULL,
                                                NULL,
                                                -1,
                                                NULL,
                                                &tdata);
            SIP_CORE_DEBUG("pjsip_endpt_create_request");
            if (status == PJ_SUCCESS) {
                if (acc->setUpTransmissionData(tdata)) {
                    status = pjsip_endpt_send_request(acc->getVoipLink().getEndpoint(),
                                                      tdata,
                                                      -1,
                                                      acc,
                                                      &keep_alive_on_complete);

                    SIP_CORE_DEBUG("pjsip_endpt_send_request");
                    if (status == PJ_SUCCESS) {
                        acc->ka_options_pending_ = true;
                        SIP_CORE_INFO("KA_EVT: options-sent route=active");
                    }
                } else {
                    status = PJSIP_SC_TSX_TRANSPORT_ERROR;
                    pjsip_tx_data_dec_ref(tdata);
                }
            }
        }

    } else if (!skipSend) {
        /* Send raw empty UDP NAT ping */
        rawUdpKeepAliveSendAttempted = isUdp;
        SIP_CORE_DEBUG("KA: Sending {}-byte keep-alive packet for acc {:s} to {:s}",
                       KA_DATA.slen,
                       contactHeader,
                       pj_sockaddr_print(&acc->kaTarget.socket, addrtxt, sizeof(addrtxt), 3));
        status = pjsip_tpmgr_send_raw(pjsip_endpt_get_tpmgr(acc->getVoipLink().getEndpoint()),
                                      transport->getPjSipTransportType(),
                                      &tp_sel,
                                      NULL,
                                      KA_DATA.ptr,
                                      KA_DATA.slen,
                                      &acc->kaTarget.socket,
                                      targetLen,
                                      NULL,
                                      NULL);

        if (status == PJ_SUCCESS && noRouteMode && acc->isActiveNoRouteFastProbeEnabled()) {
            acc->disableActiveNoRouteFastProbe("active-no-route-raw-success");
        }
    }

    if (status != PJ_SUCCESS && status != PJ_EPENDING) {
        SIP_CORE_ERROR("KA: Error sending keep-alive: {:d}", status);
        if (rawUdpKeepAliveSendAttempted) {
            acc->handleUdpRawKeepAliveSendFailure(status);
        } else {
            acc->handleActiveRouteOptionsFailure(PJSIP_SC_TSX_TRANSPORT_ERROR);
        }
    }

    uint32_t seconds = acc->getActiveKeepAliveIntervalSec();

    if (seconds == 0) {
        SIP_CORE_INFO() << "KA: 0 seconds is set, skipping keep-alive for contact "
                        << contactHeader;
        return;
    }

    delay.sec = seconds;
    delay.msec = 0;

    /* Reschedule next timer */
    status = pjsip_endpt_schedule_timer(acc->getVoipLink().getEndpoint(), te, &delay);
    if (status == PJ_SUCCESS) {
        te->id = PJ_TRUE;
        SIP_CORE_INFO("KA_EVT: active-keepalive-rescheduled interval_sec=%u fast=%d no_route=%d",
                      seconds,
                      acc->isActiveNoRouteFastProbeEnabled() ? 1 : 0,
                      noRouteMode ? 1 : 0);
    } else {
        SIP_CORE_ERROR("KA: Error starting keep-alive timer from callback: {:d}", status);
    }
}

struct ctx
{
    ctx(pjsip_auth_clt_sess* auth)
        : auth_sess(auth, &pjsip_auth_clt_deinit)
    {}
    std::weak_ptr<SIPAccount> acc;
    std::string to;
    uint64_t id;
    std::unique_ptr<pjsip_auth_clt_sess, decltype(&pjsip_auth_clt_deinit)> auth_sess;
};

static void
registration_cb(pjsip_regc_cbparam* param)
{
    if (!param) {
        SIP_CORE_ERR("registration callback parameter is null");
        return;
    }

    auto account = static_cast<SIPAccount*>(param->token);
    if (!account) {
        SIP_CORE_ERR("account doesn't exist in registration callback");
        return;
    }

    // Guard: skip callback if the account is shutting down.
    if (account->isShuttingDown_.load()) {
        SIP_CORE_DBG("Ignoring registration callback for shutting-down account");
        return;
    }

    account->onRegister(param);
}

SIPAccount::SIPAccount(const std::string& accountID, bool presenceEnabled)
    : SIPAccountBase(accountID)
    , ciphers_(100)
    , presence_(presenceEnabled ? new SIPPresence(this) : nullptr)
    , sip_events_(new SIPEvents(this))
{
    via_addr_.host.ptr = 0;
    via_addr_.host.slen = 0;
    via_addr_.port = 0;
}

SIPAccount::~SIPAccount() noexcept
{
    isShuttingDown_.store(true);
    transportRecoveryPending_.store(false);
    connectivityRecoveryRequested_.store(false);
    pendingReinviteAfterRegister_.store(false);
    connectivityRecoveryInProgress_.store(false);

    cancelKeepAliveTimer();
    cancelMainRouteKeepAliveTimer();
    cancelBackupRouteKeepAliveTimer();
    cancelAutoReregistrationTimer();
    resetAutoRegistration();

    // ensure that no registration callbacks or transport callbacks survive past this point
    destroyRegistrationInfo();
    setTransport();

    delete presence_;
}

void
SIPAccount::registerKeepAliveTimer()
{
    /* In all cases, stop keep-alive timer if it's running.
     * Keep fast-mode state: this path is also used for timer refreshes.
     */
    if (kaTarget.timer.id != PJ_FALSE) {
        pjsip_endpt_cancel_timer(link_.getEndpoint(), &kaTarget.timer);
        kaTarget.timer = {};
    }

    auto contactHeader = getContactHeader();

    uint32_t seconds = getActiveKeepAliveIntervalSec();

    if (seconds == 0) {
        disableActiveNoRouteFastProbe("active-no-route-disabled-by-interval");
        SIP_CORE_INFO() << "KA: 0 seconds is set, skipping keep-alive for contact "
                        << contactHeader;
        return;
    }

    // do not set if no transport
    if (!transport_) {
        SIP_CORE_ERR() << "KA: no transport is available for contact " << contactHeader;
        return;
    }

    auto transportType = getTransportType();
    if (transportType != PJSIP_TRANSPORT_UDP && transportType != PJSIP_TRANSPORT_TCP) {
        SIP_CORE_INFO() << "KA: keep-alive disabled for transport type " << transportType;
        return;
    }

    if (kaTarget.length == 0) {
        SIP_CORE_ERR() << "KA: no target is available for contact " << contactHeader;
        return;
    }

    const unsigned targetLen = pj_sockaddr_get_len(&kaTarget.socket);
    if (targetLen == 0) {
        SIP_CORE_ERR() << "KA: invalid target length for contact " << contactHeader;
        return;
    }
    kaTarget.length = targetLen;

    if (transportType == PJSIP_TRANSPORT_UDP && targetLen > sizeof(pj_sockaddr_in)) {
        SIP_CORE_WARN() << "KA: UDP keep-alive target is IPv6-sized (" << targetLen
                        << " bytes); skipping timer registration to avoid pj_ioqueue_sendto assert";
        return;
    }

    pj_time_val delay;
    pj_status_t status;

    /* Setup and start the timer */
    kaTarget.timer.cb = &keep_alive_timer_cb;
    kaTarget.timer.user_data = (void*) this;

    delay.sec = seconds;
    delay.msec = 0;
    status = pjsip_endpt_schedule_timer(link_.getEndpoint(), &kaTarget.timer, &delay);
    if (status == PJ_SUCCESS) {
        kaTarget.timer.id = PJ_TRUE;
        SIP_CORE_INFO() << "KA: Timer is set for " << contactHeader << " with delay " << seconds;
        SIP_CORE_INFO("KA_EVT: active-keepalive-rescheduled interval_sec=%u fast=%d no_route=%d",
                      seconds,
                      isActiveNoRouteFastProbeEnabled() ? 1 : 0,
                      isNoRouteKeepAliveMode() ? 1 : 0);
    } else {
        kaTarget.timer.id = PJ_FALSE;
        SIP_CORE_ERR() << "KA: error starting keep-alive timer for contact " << contactHeader
                       << ", status: " << status;
    }
}

void
SIPAccount::cancelKeepAliveTimer()
{
    disableActiveNoRouteFastProbe("active-keepalive-timer-cancel");
    auto contactHeader = getContactHeader();
    if (kaTarget.timer.id != PJ_FALSE) {
        SIP_CORE_INFO() << "KA: Timer is removed for " << contactHeader;

        pjsip_endpt_cancel_timer(link_.getEndpoint(), &kaTarget.timer);
        kaTarget.timer = {};
    }
    ka_options_pending_ = false;
}

void
SIPAccount::registerMainRouteKeepAliveTimer()
{
    /* In all cases, stop main route keep-alive timer if it's running.
     * Keep fast-mode state: this path is also used for timer refreshes.
     */
    if (kaMainRoute.timer.id != PJ_FALSE) {
        pjsip_endpt_cancel_timer(link_.getEndpoint(), &kaMainRoute.timer);
        kaMainRoute.timer = {};
    }

    if (!isUsingBackupRoute() || !hasServiceRoute()) {
        SIP_CORE_DBG("Main route KA: Not using backup route, not starting main route keep-alive");
        return;
    }
    if (!isOptionsKeepAliveMode()) {
        SIP_CORE_DBG("Main route KA: OPTIONS keep-alive disabled for current topology");
        return;
    }

    uint32_t seconds = getMainRouteProbeIntervalSec();
    if (seconds == 0) {
        disableMainRouteFastProbe("main-probe-disabled-by-interval");
        SIP_CORE_INFO("Main route KA: 0 seconds is set, skipping main route keep-alive");
        return;
    }

    // do not set if no transport
    if (!transport_) {
        SIP_CORE_ERR("Main route KA: no transport available");
        return;
    }

    // Resolve the main route target
    std::string mainRoute = config().serviceRoute;
    if (mainRoute.empty()) {
        SIP_CORE_ERR("Main route KA: no main route available");
        return;
    }

    const auto resolveTransportType = transportTypeFromConfig(config().transport);

    // Parse and resolve the main route address
    link_.resolveSrvName(
        mainRoute, resolveTransportType, [w = weak(), mainRoute](std::vector<IpAddr> host_ips) {
            if (auto acc = w.lock()) {
                if (host_ips.empty()) {
                    SIP_CORE_ERR("Main route KA: Can't resolve main route address");
                    return;
                }

                IpAddr mainRouteIp = host_ips[0];

                // Setup kaMainRoute socket address
                acc->kaMainRoute.socket = {};
                auto family = mainRouteIp.getFamily();
                if (family == AF_INET) {
                    auto* addr4 = reinterpret_cast<pj_sockaddr_in*>(&acc->kaMainRoute.socket);
                    pj_sockaddr_in_init(addr4, nullptr, mainRouteIp.getPort());
                    auto ipStr = mainRouteIp.toString(false);
                    pj_str_t pjIpStr = pj_str(const_cast<char*>(ipStr.c_str()));
                    pj_inet_pton(pj_AF_INET(), &pjIpStr, &addr4->sin_addr);
                    acc->kaMainRoute.length = sizeof(pj_sockaddr_in);
                } else if (family == AF_INET6) {
                    auto* addr6 = reinterpret_cast<pj_sockaddr_in6*>(&acc->kaMainRoute.socket);
                    pj_bzero(addr6, sizeof(pj_sockaddr_in6));
                    addr6->sin6_family = pj_AF_INET6();
                    addr6->sin6_port = pj_htons(mainRouteIp.getPort());
                    auto ipStr = mainRouteIp.toString(false);
                    pj_str_t pjIpStr = pj_str(const_cast<char*>(ipStr.c_str()));
                    pj_inet_pton(pj_AF_INET6(), &pjIpStr, &addr6->sin6_addr);
                    acc->kaMainRoute.length = sizeof(pj_sockaddr_in6);
                }

                // Setup and start the timer
                acc->kaMainRoute.timer.cb = &main_route_keep_alive_timer_cb;
                acc->kaMainRoute.timer.user_data = (void*) acc.get();

                const uint32_t intervalSec = acc->getMainRouteProbeIntervalSec();
                if (intervalSec == 0) {
                    SIP_CORE_INFO(
                        "Main route KA: probe interval resolved to 0, skipping timer start");
                    return;
                }

                pj_time_val delay;
                delay.sec = intervalSec;
                delay.msec = 0;

                pj_status_t status = pjsip_endpt_schedule_timer(acc->link_.getEndpoint(),
                                                                &acc->kaMainRoute.timer,
                                                                &delay);
                if (status == PJ_SUCCESS) {
                    acc->kaMainRoute.timer.id = PJ_TRUE;
                    SIP_CORE_WARN(
                        "Main route KA: Timer started to check main route %s with delay %ld "
                        "(fast=%d)",
                        mainRoute.c_str(),
                        delay.sec,
                        acc->isMainRouteFastProbeEnabled() ? 1 : 0);
                    SIP_CORE_INFO("KA_EVT: main-probe-rescheduled interval_sec=%ld fast=%d",
                                  delay.sec,
                                  acc->isMainRouteFastProbeEnabled() ? 1 : 0);
                } else {
                    acc->kaMainRoute.timer.id = PJ_FALSE;
                    SIP_CORE_ERR("Main route KA: error starting timer, status: %d", status);
                }
            }
        });
}

void
SIPAccount::registerBackupRouteKeepAliveTimer()
{
    cancelBackupRouteKeepAliveTimer();
    kaBackupRoute.timer.cb = &backup_route_keep_alive_timer_cb;
    kaBackupRoute.timer.user_data = (void*) this;
    SIP_CORE_DBG("Backup route KA: standby backup probing is disabled");
}

void
SIPAccount::cancelMainRouteKeepAliveTimer()
{
    disableMainRouteFastProbe("main-probe-timer-cancel");
    if (kaMainRoute.timer.id != PJ_FALSE) {
        SIP_CORE_INFO("Main route KA: Timer is removed");
        pjsip_endpt_cancel_timer(link_.getEndpoint(), &kaMainRoute.timer);
        kaMainRoute.timer = {};
    }
    ka_main_route_options_pending_ = false;
}

void
SIPAccount::cancelBackupRouteKeepAliveTimer()
{
    if (kaBackupRoute.timer.id != PJ_FALSE) {
        SIP_CORE_INFO("Backup route KA: Timer is removed");
        pjsip_endpt_cancel_timer(link_.getEndpoint(), &kaBackupRoute.timer);
        kaBackupRoute.timer = {};
    }
    kaBackupRoute.length = 0;
    pj_bzero(&kaBackupRoute.socket, sizeof(kaBackupRoute.socket));
    ka_backup_route_options_pending_ = false;
    pendingBackupKeepAliveStart_.store(false);
}

void
SIPAccount::cancelAutoReregistrationTimer()
{
    auto_rereg_.active = PJ_FALSE;
    if (auto_rereg_.timer.id != PJ_FALSE) {
        pjsip_endpt_cancel_timer(link_.getEndpoint(), &auto_rereg_.timer);
        auto_rereg_.timer.id = PJ_FALSE;
    }
}

void
SIPAccount::startBackupKeepAliveAfterRegister()
{
    pendingBackupKeepAliveStart_.store(false);
    cancelBackupRouteKeepAliveTimer();
}

bool
SIPAccount::sendBackupRouteKeepAlive()
{
    if (!hasBackServiceRoute()) {
        return false;
    }

    if (isUsingBackupRoute()) {
        return false;
    }

    if (!transport_) {
        SIP_CORE_ERR("Backup route KA: no transport available");
        return false;
    }

    auto backupIp = getBackServiceRouteIp();
    if (!backupIp) {
        SIP_CORE_DBG("Backup route KA: backup route address not resolved yet");
        return false;
    }

    if (ka_backup_route_options_pending_) {
        SIP_CORE_DBG("Backup route KA: OPTIONS already in flight, skipping");
        return true;
    }

    pjsip_tx_data* tdata = nullptr;

    auto server_uri = getBackupRouteKeepAliveUri();
    pj_str_t pjServer = sip_utils::CONST_PJ_STR(server_uri);
    auto contact = getContactHeader();
    pj_str_t pjContact = sip_utils::CONST_PJ_STR(contact);

    pj_status_t status = pjsip_endpt_create_request(link_.getEndpoint(),
                                                    &pjsip_options_method,
                                                    &pjServer,
                                                    &pjContact,
                                                    &pjServer,
                                                    &pjContact,
                                                    nullptr,
                                                    -1,
                                                    nullptr,
                                                    &tdata);

    if (status != PJ_SUCCESS) {
        SIP_CORE_ERR("Backup route KA: Error creating OPTIONS: %d", status);
        return false;
    }

    if (!setUpTransmissionData(tdata, backupIp)) {
        pjsip_tx_data_dec_ref(tdata);
        return false;
    }

    status = pjsip_endpt_send_request(link_.getEndpoint(),
                                      tdata,
                                      -1,
                                      this,
                                      &backup_keep_alive_on_complete);
    if (status == PJ_SUCCESS) {
        ka_backup_route_options_pending_ = true;
        SIP_CORE_DBG("Backup route KA: OPTIONS sent to backup route");
        SIP_CORE_INFO("KA_EVT: options-sent route=backup-probe");
        return true;
    }

    SIP_CORE_ERR("Backup route KA: Error sending OPTIONS: %d", status);
    return false;
}

std::shared_ptr<SIPCall>
SIPAccount::newIncomingCall(const std::string& from UNUSED,
                            const std::vector<libsip_core::MediaMap>& mediaList,
                            const std::shared_ptr<SipTransport>& transport)
{
    auto call = Manager::instance().callFactory.newSipCall(shared(),
                                                           Call::CallType::INCOMING,
                                                           mediaList);
    call->setSipTransport(transport, getContactHeader());
    return call;
}

std::shared_ptr<Call>
SIPAccount::newOutgoingCall(std::string_view toUrl,
                            const std::vector<libsip_core::MediaMap>& mediaList)
{
    std::string to;
    int family;

    SIP_CORE_DBG() << *this << "Calling SIP peer " << toUrl;

    auto& manager = Manager::instance();
    std::shared_ptr<SIPCall> call;

    // SIP allows sending empty invites.
    if (not mediaList.empty() or isEmptyOffersEnabled()) {
        call = manager.callFactory.newSipCall(shared(), Call::CallType::OUTGOING, mediaList);
    } else {
        SIP_CORE_WARN("Media list is empty, setting a default list");
        call = manager.callFactory.newSipCall(shared(),
                                              Call::CallType::OUTGOING,
                                              MediaAttribute::mediaAttributesToMediaMaps(
                                                  createDefaultMediaList(false)));
    }

    if (not call)
        throw std::runtime_error("Failed to create the call");

    to = toUrl;
    call->setSipTransport(transport_, getContactHeader());
    // Use the same address family as the SIP transport
    family = pjsip_transport_type_get_af(getTransportType());

    SIP_CORE_DBG("UserAgent: New registered account call to %.*s", (int) toUrl.size(), toUrl.data());

    auto toUri = getToUri(to);

    call->setPeerNumber(toUri);
    call->setPeerUri(toUri);

    const auto localAddress = ip_utils::getInterfaceAddr(getLocalInterface(), family);

    IpAddr addrSdp;

    addrSdp = isStunEnabled() or (not getPublishedSameasLocal()) ? getPublishedIpAddress()
                                                                 : localAddress;

    /* fallback on local address */
    if (not addrSdp)
        addrSdp = localAddress;

    // Building the local SDP offer
    auto& sdp = call->getSDP();

    if (getPublishedSameasLocal())
        sdp.setPublishedIP(addrSdp);
    else
        sdp.setPublishedIP(getPublishedAddress());

    auto mediaAttrList = MediaAttribute::buildMediaAttributesList(mediaList, isSrtpEnabled());
    if (!call->prepareLocalMediaReservations(mediaAttrList)) {
        call->onFailure();
        return call;
    }

    // TODO. We should not do this here. Move it to SIPCall.
    const bool created = sdp.createOffer(mediaAttrList);
    if (!created) {
        call->clearPendingLocalReservations();
    }

    if (created) {
        std::weak_ptr<SIPCall> weak_call = call;
        manager.scheduler().run([this, weak_call] {
            if (auto call = weak_call.lock()) {
                if (not SIPStartCall(call)) {
                    call->clearPendingLocalReservations();
                    SIP_CORE_ERR("Could not send outgoing INVITE request for new call");
                    call->onFailure();
                }
            }
            return false;
        });
    } else {
        throw VoipLinkException("Could not send outgoing INVITE request for new call");
    }

    return call;
}

bool
SIPAccount::isBenignTransportShutdown(pjsip_transport_state state, pj_status_t status) const
{
    if (status != PJ_SUCCESS && status != PJ_ECANCELLED)
        return false;

    if (state == PJSIP_TP_STATE_SHUTDOWN || state == PJSIP_TP_STATE_DESTROY)
        return true;

    return state == PJSIP_TP_STATE_DISCONNECTED && isShuttingDown_.load();
}

void
SIPAccount::markTransportRebindRequired(const char* reason)
{
    pendingTransportRebind_.store(true);
    needsResubscribe_.store(true);
    needsRepublish_.store(true);

    SIP_CORE_DBG("Marking transport rebind required for account %s (%s)",
                 accountID_.c_str(),
                 reason ? reason : "unknown");
}

void
SIPAccount::prepareTransportReset(const char* reason, bool resetNetworkRuntimeState)
{
    SIP_CORE_WARN("Preparing transport reset for account %s (%s)",
                  accountID_.c_str(),
                  reason ? reason : "unspecified");

    if (resetNetworkRuntimeState)
        resetNetworkRuntimeStateForConnectivityChange();

    markTransportRebindRequired(reason);

    cancelKeepAliveTimer();
    cancelMainRouteKeepAliveTimer();
    cancelBackupRouteKeepAliveTimer();
    cancelAutoReregistrationTimer();
    resetAutoRegistration();
    pendingBackupKeepAliveStart_.store(false);

    if (sip_events_)
        sip_events_->invalidateSubscriptions(reason);
    if (presence_)
        presence_->invalidateSubscriptionsAndPublish(reason);

    destroyRegistrationInfo();
    setTransport(nullptr);
}

bool
SIPAccount::isOptionsSuccess200(int statusCode) const
{
    return statusCode == PJSIP_SC_OK;
}

bool
SIPAccount::isTransportFailureFromOptions(int statusCode) const
{
    return statusCode == PJSIP_SC_TSX_TRANSPORT_ERROR;
}

bool
SIPAccount::isRouteFailureFromOptions(int statusCode) const
{
    // OPTIONS decisions are based on final SIP response codes only.
    if (statusCode < 200)
        return false;
    if (isOptionsSuccess200(statusCode))
        return false;
    if (isTransportFailureFromOptions(statusCode))
        return false;
    return true;
}

uint32_t
SIPAccount::resolveMainRouteProbeIntervalSec(uint32_t keepAliveIntervalSec, bool fastProbeEnabled)
{
    if (keepAliveIntervalSec == 0)
        return 0;
    return fastProbeEnabled ? MAIN_ROUTE_FAST_PROBE_INTERVAL_SEC : keepAliveIntervalSec;
}

uint32_t
SIPAccount::resolveActiveKeepAliveIntervalSec(uint32_t keepAliveIntervalSec,
                                              bool fastProbeEnabled,
                                              bool)
{
    if (keepAliveIntervalSec == 0)
        return 0;
    return fastProbeEnabled ? ACTIVE_NO_ROUTE_FAST_PROBE_INTERVAL_SEC : keepAliveIntervalSec;
}

bool
SIPAccount::shouldEnableMainRouteFastProbeForStatusCode(int statusCode)
{
    if (statusCode == PJSIP_SC_TSX_TRANSPORT_ERROR)
        return true;
    if (statusCode < 200)
        return false;
    return statusCode != PJSIP_SC_OK;
}

bool
SIPAccount::shouldDisableMainRouteFastProbeForStatusCode(int statusCode)
{
    return statusCode == PJSIP_SC_OK;
}

bool
SIPAccount::shouldEnableActiveNoRouteFastProbeForStatusCode(int statusCode)
{
    if (statusCode == PJSIP_SC_TSX_TRANSPORT_ERROR)
        return true;
    if (statusCode < 200)
        return false;
    return statusCode != PJSIP_SC_OK;
}

bool
SIPAccount::shouldDisableActiveNoRouteFastProbeForStatusCode(int statusCode)
{
    return statusCode == PJSIP_SC_OK;
}

bool
SIPAccount::isMainRouteFastProbeEnabled() const
{
    return mainRouteFastProbeEnabled_.load();
}

uint32_t
SIPAccount::getMainRouteProbeIntervalSec() const
{
    return resolveMainRouteProbeIntervalSec(config().keepAliveInterval,
                                            mainRouteFastProbeEnabled_.load());
}

bool
SIPAccount::isNoRouteKeepAliveMode() const
{
    return !hasServiceRoute() && !hasBackServiceRoute();
}

bool
SIPAccount::isActiveNoRouteFastProbeEnabled() const
{
    return activeNoRouteFastProbeEnabled_.load();
}

uint32_t
SIPAccount::getActiveKeepAliveIntervalSec() const
{
    return resolveActiveKeepAliveIntervalSec(config().keepAliveInterval,
                                             activeNoRouteFastProbeEnabled_.load(),
                                             isNoRouteKeepAliveMode());
}

void
SIPAccount::rescheduleMainRouteProbeNow(uint32_t seconds)
{
    if (!isUsingBackupRoute() || !hasServiceRoute())
        return;

    if (seconds == 0) {
        SIP_CORE_INFO("KA_EVT: main-probe-reschedule-skipped interval_sec=0");
        return;
    }

    if (!transport_) {
        SIP_CORE_WARN("KA_EVT: main-probe-reschedule-skipped reason=no-transport");
        return;
    }

    if (kaMainRoute.length == 0) {
        SIP_CORE_WARN("KA_EVT: main-probe-reschedule-skipped reason=no-target");
        return;
    }

    if (kaMainRoute.timer.cb == nullptr)
        kaMainRoute.timer.cb = &main_route_keep_alive_timer_cb;
    kaMainRoute.timer.user_data = (void*) this;

    if (kaMainRoute.timer.id != PJ_FALSE) {
        pjsip_endpt_cancel_timer(link_.getEndpoint(), &kaMainRoute.timer);
        kaMainRoute.timer.id = PJ_FALSE;
    }

    pj_time_val delay;
    delay.sec = seconds;
    delay.msec = 0;

    const pj_status_t status = pjsip_endpt_schedule_timer(link_.getEndpoint(),
                                                          &kaMainRoute.timer,
                                                          &delay);
    if (status == PJ_SUCCESS) {
        kaMainRoute.timer.id = PJ_TRUE;
        SIP_CORE_INFO("KA_EVT: main-probe-rescheduled-now interval_sec=%u fast=%d",
                      seconds,
                      mainRouteFastProbeEnabled_.load() ? 1 : 0);
    } else {
        kaMainRoute.timer.id = PJ_FALSE;
        SIP_CORE_WARN("KA_EVT: main-probe-reschedule-failed status=%d", status);
    }
}

void
SIPAccount::rescheduleActiveKeepAliveNow(uint32_t seconds)
{

    if (seconds == 0) {
        SIP_CORE_INFO("KA_EVT: active-no-route-reschedule-skipped interval_sec=0");
        return;
    }

    if (!transport_) {
        SIP_CORE_WARN("KA_EVT: active-no-route-reschedule-skipped reason=no-transport");
        return;
    }

    const unsigned targetLen = pj_sockaddr_get_len(&kaTarget.socket);
    if (targetLen == 0 || kaTarget.length == 0) {
        SIP_CORE_WARN("KA_EVT: active-no-route-reschedule-skipped reason=no-target");
        return;
    }
    kaTarget.length = targetLen;

    if (kaTarget.timer.cb == nullptr)
        kaTarget.timer.cb = &keep_alive_timer_cb;
    kaTarget.timer.user_data = (void*) this;

    if (kaTarget.timer.id != PJ_FALSE) {
        pjsip_endpt_cancel_timer(link_.getEndpoint(), &kaTarget.timer);
        kaTarget.timer.id = PJ_FALSE;
    }

    pj_time_val delay;
    delay.sec = seconds;
    delay.msec = 0;

    const pj_status_t status = pjsip_endpt_schedule_timer(link_.getEndpoint(),
                                                          &kaTarget.timer,
                                                          &delay);
    if (status == PJ_SUCCESS) {
        kaTarget.timer.id = PJ_TRUE;
        SIP_CORE_INFO("KA_EVT: active-no-route-rescheduled-now interval_sec=%u fast=%d",
                      seconds,
                      activeNoRouteFastProbeEnabled_.load() ? 1 : 0);
    } else {
        kaTarget.timer.id = PJ_FALSE;
        SIP_CORE_WARN("KA_EVT: active-no-route-reschedule-failed status=%d", status);
    }
}

void
SIPAccount::enableMainRouteFastProbe(const char* reason)
{
    if (!isUsingBackupRoute())
        return;

    bool expected = false;
    if (!mainRouteFastProbeEnabled_.compare_exchange_strong(expected, true)) {
        const uint32_t interval = getMainRouteProbeIntervalSec();
        SIP_CORE_DBG("KA_EVT: main-probe-fast-unchanged state=enabled reason=%s interval_sec=%u",
                     reason ? reason : "unknown",
                     interval);
        rescheduleMainRouteProbeNow(interval);
        return;
    }

    const uint32_t interval = getMainRouteProbeIntervalSec();
    SIP_CORE_WARN("KA_EVT: main-probe-fast-enabled reason=%s interval_sec=%u",
                  reason ? reason : "unknown",
                  interval);
    rescheduleMainRouteProbeNow(interval);
}

void
SIPAccount::enableActiveNoRouteFastProbe(const char* reason, bool rescheduleNow)
{

    bool expected = false;
    if (!activeNoRouteFastProbeEnabled_.compare_exchange_strong(expected, true)) {
        const uint32_t interval = getActiveKeepAliveIntervalSec();
        SIP_CORE_DBG(
            "KA_EVT: active-no-route-fast-unchanged state=enabled reason=%s interval_sec=%u",
            reason ? reason : "unknown",
            interval);
        if (rescheduleNow)
            rescheduleActiveKeepAliveNow(interval);
        return;
    }

    const uint32_t interval = getActiveKeepAliveIntervalSec();
    SIP_CORE_WARN("KA_EVT: active-no-route-fast-enabled reason=%s interval_sec=%u",
                  reason ? reason : "unknown",
                  interval);
    if (rescheduleNow)
        rescheduleActiveKeepAliveNow(interval);
}

void
SIPAccount::disableMainRouteFastProbe(const char* reason)
{
    bool expected = true;
    if (!mainRouteFastProbeEnabled_.compare_exchange_strong(expected, false)) {
        SIP_CORE_DBG("KA_EVT: main-probe-fast-unchanged state=disabled reason=%s",
                     reason ? reason : "unknown");
        return;
    }

    const uint32_t interval = getMainRouteProbeIntervalSec();
    SIP_CORE_INFO("KA_EVT: main-probe-fast-disabled reason=%s interval_sec=%u",
                  reason ? reason : "unknown",
                  interval);
}

void
SIPAccount::disableActiveNoRouteFastProbe(const char* reason)
{
    bool expected = true;
    if (!activeNoRouteFastProbeEnabled_.compare_exchange_strong(expected, false)) {
        SIP_CORE_DBG("KA_EVT: active-no-route-fast-unchanged state=disabled reason=%s",
                     reason ? reason : "unknown");
        return;
    }

    const uint32_t interval = getActiveKeepAliveIntervalSec();
    SIP_CORE_INFO("KA_EVT: active-no-route-fast-disabled reason=%s interval_sec=%u",
                  reason ? reason : "unknown",
                  interval);
}

bool
SIPAccount::isTransientOptionsFailureCode(int statusCode)
{
    if (statusCode == PJSIP_SC_TSX_TRANSPORT_ERROR)
        return true;

    switch (statusCode) {
    case PJSIP_SC_REQUEST_TIMEOUT:
    case PJSIP_SC_INTERNAL_SERVER_ERROR:
    case PJSIP_SC_BAD_GATEWAY:
    case PJSIP_SC_SERVICE_UNAVAILABLE:
    case PJSIP_SC_SERVER_TIMEOUT:
        return true;
    default:
        return false;
    }
}

bool
SIPAccount::isHardOptionsFailureCode(int statusCode)
{
    if (statusCode < 200)
        return false;
    if (statusCode == PJSIP_SC_OK)
        return false;
    if (statusCode == PJSIP_SC_TSX_TRANSPORT_ERROR)
        return false;
    return !isTransientOptionsFailureCode(statusCode);
}

bool
SIPAccount::shouldSuppressOptionsRecoveryAttempt(std::deque<int64_t>& attemptMs,
                                                 int64_t nowMs,
                                                 size_t maxAttempts,
                                                 int64_t windowMs)
{
    const int64_t minAllowedTs = nowMs - windowMs;
    while (!attemptMs.empty() && attemptMs.front() < minAllowedTs) {
        attemptMs.pop_front();
    }
    if (attemptMs.size() >= maxAttempts) {
        return true;
    }
    attemptMs.emplace_back(nowMs);
    return false;
}

bool
SIPAccount::isTransientOptionsFailure(int statusCode) const
{
    return isTransientOptionsFailureCode(statusCode);
}

bool
SIPAccount::isHardOptionsFailure(int statusCode) const
{
    return isHardOptionsFailureCode(statusCode);
}

void
SIPAccount::resetOptionsRecoveryWindow()
{
    std::lock_guard<std::mutex> lock(optionsRecoveryMutex_);
    optionsRecoveryAttemptMs_.clear();
}

bool
SIPAccount::shouldSuppressOptionsRecovery()
{
    const auto now = std::chrono::steady_clock::now();
    const int64_t nowMs
        = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    std::lock_guard<std::mutex> lock(optionsRecoveryMutex_);
    return shouldSuppressOptionsRecoveryAttempt(optionsRecoveryAttemptMs_,
                                                nowMs,
                                                OPTIONS_RECOVERY_MAX_ATTEMPTS,
                                                OPTIONS_RECOVERY_WINDOW_MS);
}

void
SIPAccount::resetRawKeepAliveRecoveryWindow()
{
    std::lock_guard<std::mutex> lock(rawKeepAliveRecoveryMutex_);
    rawKeepAliveRecoveryAttemptMs_.clear();
}

bool
SIPAccount::shouldSuppressRawKeepAliveRecovery()
{
    const auto now = std::chrono::steady_clock::now();
    const int64_t nowMs
        = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    std::lock_guard<std::mutex> lock(rawKeepAliveRecoveryMutex_);
    return shouldSuppressOptionsRecoveryAttempt(rawKeepAliveRecoveryAttemptMs_,
                                                nowMs,
                                                RAW_KEEPALIVE_RECOVERY_MAX_ATTEMPTS,
                                                RAW_KEEPALIVE_RECOVERY_WINDOW_MS);
}

void
SIPAccount::handleNoBackupOptionsRouteFailure(int statusCode)
{
    handleActiveRouteOptionsFailure(statusCode);
}

void
SIPAccount::handleUdpRawKeepAliveSendFailure(pj_status_t status)
{
    SIP_CORE_WARN("KA_EVT: udp-raw-send-failure account=%s status=%d", accountID_.c_str(), status);

    if (shouldSuppressRawKeepAliveRecovery()) {
        SIP_CORE_WARN(
            "KA_EVT: udp-raw-recovery-suppressed account=%s status=%d window_ms=%lld max=%zu",
            accountID_.c_str(),
            status,
            static_cast<long long>(RAW_KEEPALIVE_RECOVERY_WINDOW_MS),
            RAW_KEEPALIVE_RECOVERY_MAX_ATTEMPTS);
        setRegistrationState(RegistrationState::ERROR_GENERIC, PJSIP_SC_TSX_TRANSPORT_ERROR);
        scheduleReregistration();
        return;
    }

    SIP_CORE_WARN("KA_EVT: udp-raw-recovery-trigger account=%s status=%d",
                  accountID_.c_str(),
                  status);
    scheduleTransportRecovery("active-route-udp-raw-keepalive-send-failure", status);
}

void
SIPAccount::switchRouteAndReregister(bool useBackup,
                                     const char* reason,
                                     bool skipMainRouteStartupProbe)
{
    if (isShuttingDown_.load() || !isUsable()) {
        SIP_CORE_DBG("Skipping route switch for account %s: shutting down or unusable",
                     accountID_.c_str());
        return;
    }

    bool expected = false;
    if (!routeSwitchPending_.compare_exchange_strong(expected, true)) {
        SIP_CORE_DBG("Route switch already pending for account %s", accountID_.c_str());
        return;
    }

    SIP_CORE_WARN("Route switch requested for account %s: target=%s (%s)",
                  accountID_.c_str(),
                  useBackup ? "backup" : "main",
                  reason ? reason : "unspecified");

    bool switched = false;
    if (useBackup) {
        if (!isUsingBackupRoute()) {
            switchToBackupRoute();
            switched = isUsingBackupRoute();
        } else {
            SIP_CORE_DBG("Route switch skipped for account %s: already on backup",
                         accountID_.c_str());
        }
    } else {
        if (isUsingBackupRoute()) {
            switchToMainRoute();
            switched = !isUsingBackupRoute();
        } else {
            SIP_CORE_DBG("Route switch skipped for account %s: already on main", accountID_.c_str());
        }
    }

    if (switched) {
        if (skipMainRouteStartupProbe)
            skipMainRouteStartupProbeOnce_.store(true);
        destroyRegistrationInfo();
        doRegister();
    }

    routeSwitchPending_.store(false);
}

std::pair<std::string, pj_uint16_t>
SIPAccount::currentLocalBinding() const
{
    if (!transport_)
        return {};

    std::string address;
    pj_uint16_t port = 0;
    link_.findLocalAddressFromTransport(transport_, config().hostname, address, port);
    if (address.empty() || port == 0)
        return {};

    return {address, port};
}

void
SIPAccount::runPostRegisterRecoverySync()
{
    const bool pendingRebind = pendingTransportRebind_.exchange(false);
    const bool resubscribe = needsResubscribe_.exchange(false);
    const bool republish = needsRepublish_.exchange(false);

    if (!pendingRebind && !resubscribe && !republish)
        return;

    if (!transport_) {
        SIP_CORE_WARN("Skipping recovery sync for account %s: no transport", accountID_.c_str());
        if (pendingRebind)
            pendingTransportRebind_.store(true);
        if (resubscribe)
            needsResubscribe_.store(true);
        if (republish)
            needsRepublish_.store(true);
        return;
    }

    if (!initContactAddress()) {
        SIP_CORE_WARN("Skipping recovery sync for account %s: invalid contact address",
                      accountID_.c_str());
        if (pendingRebind)
            pendingTransportRebind_.store(true);
        if (resubscribe)
            needsResubscribe_.store(true);
        if (republish)
            needsRepublish_.store(true);
        return;
    }
    updateContactHeader();

    const std::string contactHeader = getContactHeader();
    const bool recoverSubscriptions = pendingRebind || resubscribe;

    SIP_CORE_WARN("Running post-register recovery sync for account %s (resubscribe=%d, "
                  "republish=%d)",
                  accountID_.c_str(),
                  recoverSubscriptions ? 1 : 0,
                  republish ? 1 : 0);
    SIP_CORE_INFO("KA_EVT: post-register-recovery-sync account=%s resubscribe=%d republish=%d",
                  accountID_.c_str(),
                  recoverSubscriptions ? 1 : 0,
                  republish ? 1 : 0);

    if (recoverSubscriptions && sip_events_)
        sip_events_->recoverSubscriptions(contactHeader);

    if (presence_ && (recoverSubscriptions || republish))
        presence_->recoverSubscriptionsAndPublish(contactHeader, republish);
}

bool
SIPAccount::shouldRecoverTransport(pjsip_transport_state state, pj_status_t status) const
{
    if (config().transport != libsip_core::TransportType::TCP
        && config().transport != libsip_core::TransportType::UDP)
        return false;
    if (isShuttingDown_.load())
        return false;
    if (isBenignTransportShutdown(state, status))
        return false;
    return !SipTransport::isAlive(state);
}

bool
SIPAccount::scheduleTransportRecovery(const char* reason, pj_status_t status)
{
    SIP_CORE_WARN("KA_EVT: recovery-scheduled account=%s reason=%s status=%d",
                  accountID_.c_str(),
                  reason ? reason : "transport",
                  status);
    return scheduleRecoveryInternal(reason, status, true);
}

void
SIPAccount::scheduleConnectivityRecovery(const char* reason)
{
    handleConnectivityChangedForced(reason);
}

void
SIPAccount::prepareConnectivityRecovery(const char* reason)
{
    if (isShuttingDown_.load() || !isUsable()) {
        SIP_CORE_DBG("Skipping connectivity recovery preparation for account %s",
                     accountID_.c_str());
        return;
    }

    for (const auto& id : getCallList()) {
        auto call = getCall(id);
        auto sipCall = std::dynamic_pointer_cast<SIPCall>(call);
        if (!sipCall)
            continue;
        sipCall->prepareConnectivityRecoverySnapshot();
    }

    connectivityRecoveryRequested_.store(true);
    connectivityRecoveryInProgress_.store(true);

    SIP_CORE_WARN("Forced connectivity recovery requested for account %s (%s)",
                  accountID_.c_str(),
                  reason ? reason : "unspecified");

    prepareTransportReset(reason ? reason : "connectivity-changed", true);
}

void
SIPAccount::dispatchPreparedConnectivityRecovery(const char* reason)
{
    scheduleRecoveryInternal(reason ? reason : "connectivity-changed",
                             PJSIP_SC_TSX_TRANSPORT_ERROR,
                             false);
}

bool
SIPAccount::scheduleRecoveryInternal(const char* reason, pj_status_t status, bool debounced)
{
    if (isShuttingDown_.load() || !isUsable()) {
        SIP_CORE_DBG("Skipping transport recovery scheduling for account %s: shutting down or "
                     "unusable",
                     accountID_.c_str());
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch())
                           .count();
    if (debounced) {
        const auto lastMs = lastTransportRecoveryMs_.load();
        if (lastMs > 0 && (nowMs - lastMs) < TRANSPORT_RECOVERY_DEBOUNCE_MS) {
            SIP_CORE_DBG("Skipping transport recovery for account %s (debounced)",
                         accountID_.c_str());
            return false;
        }
    }

    bool expected = false;
    if (!transportRecoveryPending_.compare_exchange_strong(expected, true)) {
        if (!debounced && connectivityRecoveryRequested_.load()) {
            SIP_CORE_WARN("Connectivity recovery already pending for account %s, coalescing "
                          "additional request",
                          accountID_.c_str());
        } else {
            SIP_CORE_DBG("Transport recovery already pending for account %s", accountID_.c_str());
        }
        return false;
    }

    lastTransportRecoveryMs_.store(nowMs);

    const auto reasonStr = std::string(reason ? reason : "transport");
    runOnMainThread([w = weak(), reasonStr, status] {
        if (auto acc = w.lock())
            acc->recoverTransport(reasonStr, status);
    });
    return true;
}

void
SIPAccount::recoverTransport(const std::string& reason, pj_status_t status)
{
    if (!transportRecoveryPending_.exchange(false))
        return;
    connectivityRecoveryInProgress_.store(true);
    const bool connectivityRequested = connectivityRecoveryRequested_.exchange(false);
    const bool connectivityReason = reason.rfind("connectivity-changed", 0) == 0;

    auto schedulePendingConnectivityRecovery = [&] {
        if (!isUsable() || isShuttingDown_.load())
            return;
        if (connectivityRecoveryRequested_.exchange(false)) {
            SIP_CORE_WARN("Connectivity recovery was re-requested while recovery was in progress "
                          "for account %s, scheduling follow-up recovery",
                          accountID_.c_str());
            handleConnectivityChangedForced("connectivity-changed-coalesced");
        }
    };

    if (isShuttingDown_.load() || !isUsable()) {
        connectivityRecoveryInProgress_.store(false);
        return;
    }
    if (!link_.sipTransportBroker || !link_.getEndpoint()) {
        connectivityRecoveryInProgress_.store(false);
        return;
    }

    SIP_CORE_WARN("Recovering %s transport for account %s after '%s' (%d: %s)",
                  config().transport == libsip_core::TransportType::TCP ? "TCP" : "SIP",
                  accountID_.c_str(),
                  reason.c_str(),
                  status,
                  sip_utils::sip_strerror(status).c_str());
    prepareTransportReset(reason.c_str(), connectivityRequested || connectivityReason);

    if (!switchTransportInternal(config().transport, false, false, nullptr)) {
        SIP_CORE_ERR("Transport recreation failed during recovery for account %s",
                     accountID_.c_str());
        connectivityRecoveryInProgress_.store(false);
        pendingReinviteAfterRegister_.store(false);
        setRegistrationState(RegistrationState::ERROR_GENERIC, PJSIP_SC_TSX_TRANSPORT_ERROR);
        schedulePendingConnectivityRecovery();
        scheduleReregistration();
        return;
    }

    SIP_CORE_WARN("Transport recreation succeeded during recovery for account %s, triggering "
                  "re-registration",
                  accountID_.c_str());

    // Defer reinvite until registration completes (onRegister 200 OK)
    pendingReinviteAfterRegister_.store(true);
    doRegister();
    connectivityRecoveryInProgress_.store(false);
    schedulePendingConnectivityRecovery();
}

void
SIPAccount::resetViaTransport()
{
    via_tp_ = nullptr;
}

void
SIPAccount::resetNetworkRuntimeStateForConnectivityChange()
{
    SIP_CORE_WARN("Resetting network runtime state for account %s before connectivity recovery",
                  accountID_.c_str());

    receivedParameter_.clear();
    rPort_ = -1;
    transportError_.clear();
    transportStatus_ = PJSIP_SC_TRYING;

    via_addr_.host.ptr = nullptr;
    via_addr_.host.slen = 0;
    via_addr_.port = 0;
    resetViaTransport();

    std::lock_guard<std::mutex> lock(localBindingMutex_);
    hasLocalBindingSnapshot_ = false;
    lastLocalBindingAddress_.clear();
    lastLocalBindingPort_ = 0;
}

void
SIPAccount::onTransportStateChanged(pjsip_transport_state state,
                                    const pjsip_transport_state_info* info)
{
    const auto status = info ? info->status : PJ_SUCCESS;
    const auto currentStatus = transportStatus_;

    SIP_CORE_DBG("Transport state changed to %s for account %s",
                 SipTransport::stateToStr(state),
                 accountID_.c_str());

    const bool alive = SipTransport::isAlive(state);
    if (!alive) {
        resetViaTransport();

        if (isBenignTransportShutdown(state, status)) {
            transportStatus_ = status == PJ_SUCCESS ? PJSIP_SC_OK : status;
            transportError_.clear();
            SIP_CORE_DBG("Ignoring benign transport shutdown state=%s status=%d for account %s",
                         SipTransport::stateToStr(state),
                         status,
                         accountID_.c_str());
        } else {
            transportStatus_ = status ? status : PJSIP_SC_SERVICE_UNAVAILABLE;
            transportError_ = sip_utils::sip_strerror(transportStatus_);
            SIP_CORE_ERR("Transport disconnected: %s", transportError_.c_str());
        }

        if (shouldRecoverTransport(state, status)) {
            if (!scheduleTransportRecovery("transport-state-change", transportStatus_)) {
                // Recovery was debounced — set error state as fallback so
                // connectivityChanged can still pick up this account.
                setRegistrationState(RegistrationState::ERROR_GENERIC,
                                     PJSIP_SC_TSX_TRANSPORT_ERROR);
            }
        } else if (!isBenignTransportShutdown(state, status)) {
            setRegistrationState(RegistrationState::ERROR_GENERIC, PJSIP_SC_TSX_TRANSPORT_ERROR);
        }
    } else {
        transportStatus_ = status ? status : PJSIP_SC_OK;
        transportError_.clear();
    }

    if (currentStatus != transportStatus_) {
        emitSignal<libsip_core::ConfigurationSignal::VolatileDetailsChanged>(
            accountID_, getVolatileAccountDetails());
    }
}

bool
SIPAccount::setTransport(const std::shared_ptr<SipTransport>& t)
{
    if (t == transport_)
        return true;

    const auto listenerId = reinterpret_cast<uintptr_t>(this);

    const auto updateLocalBindingSnapshot = [&] {
        const auto binding = currentLocalBinding();
        if (binding.first.empty() || binding.second == 0)
            return;

        std::string prevAddress;
        pj_uint16_t prevPort = 0;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(localBindingMutex_);
            if (hasLocalBindingSnapshot_) {
                changed = lastLocalBindingAddress_ != binding.first
                          || lastLocalBindingPort_ != binding.second;
                prevAddress = lastLocalBindingAddress_;
                prevPort = lastLocalBindingPort_;
            }
            lastLocalBindingAddress_ = binding.first;
            lastLocalBindingPort_ = binding.second;
            hasLocalBindingSnapshot_ = true;
        }

        if (changed) {
            SIP_CORE_WARN("SIP local binding changed for account %s: %s:%u -> %s:%u",
                          accountID_.c_str(),
                          prevAddress.c_str(),
                          prevPort,
                          binding.first.c_str(),
                          binding.second);
            markTransportRebindRequired("local-binding-changed");
        }
    };

    // attachTransport only runs when transport is non-null, so weak() / shared_from_this()
    // is safe here — we are guaranteed to be owned by a live shared_ptr.
    const auto attachTransport = [&](const std::shared_ptr<SipTransport>& transport) {
        if (!transport)
            return true;

        const auto stateListener = [w = weak()](pjsip_transport_state state,
                                                const pjsip_transport_state_info* info) {
            const bool hasInfo = info != nullptr;
            const auto status = hasInfo ? info->status : PJ_SUCCESS;
            runOnMainThread([w, state, status, hasInfo] {
                if (auto account = w.lock()) {
                    pjsip_transport_state_info infoCopy {};
                    infoCopy.status = status;
                    account->onTransportStateChanged(state, hasInfo ? &infoCopy : nullptr);
                }
            });
        };

        transport->addStateListener(listenerId, stateListener);
        if (!initContactAddress()) {
            SIP_CORE_ERR("Can not register account %s: invalid contact address after transport "
                         "switch",
                         accountID_.c_str());
            transport->removeStateListener(listenerId);
            return false;
        }
        updateContactHeader();

        if (regc_) {
            const auto tpSel = getTransportSelector();
            if (pjsip_regc_set_transport(regc_, &tpSel) != PJ_SUCCESS) {
                SIP_CORE_WARN("Failed to rebind registration client to updated transport for "
                              "account %s",
                              accountID_.c_str());
            }
        }

        updateLocalBindingSnapshot();
        return true;
    };

    const auto previousTransport = transport_;

    if (previousTransport) {
        SIP_CORE_DBG("Removing old transport [%p] from account", previousTransport.get());
        if (regc_)
            pjsip_regc_release_transport(regc_);
        previousTransport->removeStateListener(listenerId);
    }

    transport_ = t;
    resetViaTransport();
    SIP_CORE_DBG("Set new transport [%p]", transport_.get());

    if (attachTransport(transport_))
        return true;

    // Roll back to the previous transport to avoid leaving account in a half-switched state.
    transport_ = previousTransport;
    resetViaTransport();
    SIP_CORE_WARN("Restoring previous transport [%p] for account %s",
                  transport_.get(),
                  accountID_.c_str());

    if (attachTransport(transport_))
        return false;

    SIP_CORE_ERR("Failed to restore previous transport for account %s", accountID_.c_str());
    transport_.reset();
    resetViaTransport();
    return false;
}

bool
SIPAccount::switchTransportInternal(libsip_core::TransportType transportType,
                                    bool persistConfig,
                                    bool markRebind,
                                    bool* changed)
{
    if (changed)
        *changed = false;

    if (!link_.sipTransportBroker) {
        SIP_CORE_ERR("Can't switch transport for account %s: SipTransportBroker is unavailable",
                     accountID_.c_str());
        return false;
    }

    if (transportType != libsip_core::TransportType::UDP
        && transportType != libsip_core::TransportType::TCP) {
        SIP_CORE_ERR("Unsupported SIP transport switch request for account %s (type=%d)",
                     accountID_.c_str(),
                     static_cast<int>(transportType));
        return false;
    }

    IpAddr bindAddress = createBindingAddress();
    if (not bindAddress) {
        SIP_CORE_ERR("Can't compute bind address for account %s transport switch",
                     accountID_.c_str());
        return false;
    }

    auto previousTransport = transport_;
    std::shared_ptr<SipTransport> candidateTransport;
    if (transportType == libsip_core::TransportType::UDP) {
        candidateTransport = link_.sipTransportBroker->getUdpTransport(bindAddress);
    } else {
        candidateTransport = link_.sipTransportBroker->getTcpTransport(bindAddress);
    }

    if (!candidateTransport) {
        SIP_CORE_ERR("Failed to acquire %s transport for account %s",
                     transportType == libsip_core::TransportType::TCP ? "TCP" : "UDP",
                     accountID_.c_str());
        return false;
    }

    if (!setTransport(candidateTransport)) {
        SIP_CORE_ERR("Transport switch failed for account %s (type=%s)",
                     accountID_.c_str(),
                     transportType == libsip_core::TransportType::TCP ? "TCP" : "UDP");
        return false;
    }

    const bool transportChanged = previousTransport != transport_;
    if (changed)
        *changed = transportChanged;

    if (persistConfig && config().transport != transportType)
        editConfig([&](SipAccountConfig& config) { config.transport = transportType; });

    if (markRebind && transportChanged)
        markTransportRebindRequired("transport-type-switch");

    if (transportChanged) {
        SIP_CORE_WARN("Transport switched for account %s to %s",
                      accountID_.c_str(),
                      transportType == libsip_core::TransportType::TCP ? "TCP" : "UDP");
    } else {
        SIP_CORE_DBG("Transport switch is a no-op for account %s", accountID_.c_str());
    }

    return true;
}

bool
SIPAccount::switchTransport(libsip_core::TransportType transportType)
{
    bool expected = false;
    if (!transportSwitchPending_.compare_exchange_strong(expected, true)) {
        SIP_CORE_WARN("Transport switch already pending for account %s", accountID_.c_str());
        return false;
    }

    struct SwitchGuard
    {
        std::atomic<bool>& pending;
        ~SwitchGuard() { pending.store(false); }
    } guard {transportSwitchPending_};

    SIP_CORE_WARN("Switching transport for account %s (requested type=%d)",
                  accountID_.c_str(),
                  static_cast<int>(transportType));

    bool changed = false;
    if (!switchTransportInternal(transportType, true, true, &changed))
        return false;

    if (!changed)
        return true;

    if (!isUsable()) {
        SIP_CORE_DBG("Transport switched for account %s but account is not usable; skipping "
                     "re-registration",
                     accountID_.c_str());
        return true;
    }

    markTransportRebindRequired("manual-transport-switch");
    cancelKeepAliveTimer();
    cancelMainRouteKeepAliveTimer();
    cancelBackupRouteKeepAliveTimer();
    pendingBackupKeepAliveStart_.store(false);
    cancelAutoReregistrationTimer();
    resetAutoRegistration();

    destroyRegistrationInfo();
    doRegister();

    return true;
}

void
SIPAccount::setAccountDetails(const std::map<std::string, std::string>& details)
{
    SipAccountConfig defaults;
    KeepAliveType oldKeepAliveType = defaults.keepAliveType;
    uint32_t oldKeepAliveInterval = defaults.keepAliveInterval;
    std::string oldServiceRoute;
    std::string oldBackServiceRoute;
    bool wasEnabled = defaults.enabled;

    {
        std::lock_guard<std::recursive_mutex> lock(configurationMutex_);
        if (config_) {
            const auto& cfg = config();
            oldKeepAliveType = cfg.keepAliveType;
            oldKeepAliveInterval = cfg.keepAliveInterval;
            oldServiceRoute = cfg.serviceRoute;
            oldBackServiceRoute = cfg.backServiceRoute;
            wasEnabled = cfg.enabled;
        }
    }

    Account::setAccountDetails(details);

    KeepAliveType newKeepAliveType;
    uint32_t newKeepAliveInterval;
    std::string newServiceRoute;
    std::string newBackServiceRoute;
    bool isEnabled;
    {
        std::lock_guard<std::recursive_mutex> lock(configurationMutex_);
        const auto& cfg = config();
        newKeepAliveType = cfg.keepAliveType;
        newKeepAliveInterval = cfg.keepAliveInterval;
        newServiceRoute = cfg.serviceRoute;
        newBackServiceRoute = cfg.backServiceRoute;
        isEnabled = cfg.enabled;
    }

    const bool keepAliveChanged = (oldKeepAliveType != newKeepAliveType)
                                  || (oldKeepAliveInterval != newKeepAliveInterval);
    const bool routeConfigChanged = oldServiceRoute != newServiceRoute
                                    || oldBackServiceRoute != newBackServiceRoute;
    if (!keepAliveChanged && wasEnabled == isEnabled && !routeConfigChanged)
        return;

    const bool restoreMainRouteFastProbe = isUsingBackupRoute() && mainRouteFastProbeEnabled_.load()
                                           && newKeepAliveInterval > 0;
    const bool restoreActiveNoRouteFastProbe
        = shouldUseOptionsForKeepAlive(newKeepAliveType, getTransportType() == PJSIP_TRANSPORT_UDP)
          && activeNoRouteFastProbeEnabled_.load()
                                               && newKeepAliveInterval > 0;

    // Clear pending state and restart timers based on the new configuration.
    ka_options_pending_ = false;
    ka_main_route_options_pending_ = false;
    ka_backup_route_options_pending_ = false;

    cancelKeepAliveTimer();
    cancelMainRouteKeepAliveTimer();
    cancelBackupRouteKeepAliveTimer();

    if (isUsable() && transport_) {
        registerKeepAliveTimer();
        if (restoreActiveNoRouteFastProbe)
            enableActiveNoRouteFastProbe("set-account-details-keepalive-refresh");
        if (hasBackServiceRoute()) {
            if (isUsingBackupRoute()) {
                registerMainRouteKeepAliveTimer();
                if (restoreMainRouteFastProbe)
                    enableMainRouteFastProbe("set-account-details-keepalive-refresh");
            } else {
                registerBackupRouteKeepAliveTimer();
            }
        }
    }
}

pjsip_tpselector
SIPAccount::getTransportSelector()
{
    if (!transport_)
        return SIPVoIPLink::getTransportSelector(nullptr);
    return SIPVoIPLink::getTransportSelector(transport_);
}

bool
SIPAccount::SIPStartCall(std::shared_ptr<SIPCall>& call)
{
    const std::string& toUri(call->getPeerNumber()); // expecting a fully well formed sip uri
    pj_str_t pjTo = sip_utils::CONST_PJ_STR(toUri);

    // Create the from header
    std::string from(getFromUri());
    pj_str_t pjFrom = sip_utils::CONST_PJ_STR(from);

    auto transport = call->getTransport();
    if (!transport) {
        SIP_CORE_ERR("Unable to start call without transport");
        return false;
    }

    std::string contact = getContactHeader();
    SIP_CORE_DBG("contact header: %s / %s -> %s", contact.c_str(), from.c_str(), toUri.c_str());

    pj_str_t pjContact = sip_utils::CONST_PJ_STR(contact);
    auto local_sdp = isEmptyOffersEnabled() ? nullptr : call->getSDP().getLocalSdpSession();

    pjsip_dialog* dialog {nullptr};
    pjsip_inv_session* inv {nullptr};
    if (!CreateClientDialogAndInvite(&pjFrom, &pjContact, &pjTo, nullptr, local_sdp, &dialog, &inv))
        return false;

    inv->mod_data[link_.getModId()] = call.get();
    call->setInviteSession(inv);

    updateDialogViaSentBy(dialog);

    std::string activeRoute = getActiveServiceRoute();
    if (!activeRoute.empty()) {
        call->setInitialServiceRoute(activeRoute);
        pjsip_dlg_set_route_set(dialog,
                                sip_utils::createRouteSet(activeRoute, call->inviteSession_->pool));
    }

    if (hasCredentials()
        and pjsip_auth_clt_set_credentials(&dialog->auth_sess, getCredentialCount(), getCredInfo())
                != PJ_SUCCESS) {
        SIP_CORE_ERR("Could not initialize credentials for invite session authentication");
        return false;
    }

    pjsip_tx_data* tdata;

    if (pjsip_inv_invite(call->inviteSession_.get(), &tdata) != PJ_SUCCESS) {
        SIP_CORE_ERR("Could not initialize invite messager for this call");
        return false;
    }

    const pjsip_tpselector tp_sel = link_.getTransportSelector(transport);
    if (pjsip_dlg_set_transport(dialog, &tp_sel) != PJ_SUCCESS) {
        SIP_CORE_ERR("Unable to associate transport for invite session dialog");
        return false;
    }

    // Add user-agent header
    sip_utils::addUserAgentHeader(getUserAgentName(), tdata);

    if (pjsip_inv_send_msg(call->inviteSession_.get(), tdata) != PJ_SUCCESS) {
        SIP_CORE_ERR("Unable to send invite message for this call");
        return false;
    }

    call->setState(Call::CallState::ACTIVE, Call::ConnectionState::PROGRESSING);

    return true;
}

void
SIPAccount::usePublishedAddressPortInVIA()
{
    publishedIpStr_ = getPublishedIpAddress().toString();
    via_addr_.host.ptr = (char*) publishedIpStr_.c_str();
    via_addr_.host.slen = publishedIpStr_.size();
    via_addr_.port = publishedPortUsed_;
}

template<typename T>
static void
validate(std::string& member, const std::string& param, const T& valid)
{
    const auto begin = std::begin(valid);
    const auto end = std::end(valid);
    if (find(begin, end, param) != end)
        member = param;
    else
        SIP_CORE_ERR("Invalid parameter \"%s\"", param.c_str());
}

std::map<std::string, std::string>
SIPAccount::getVolatileAccountDetails() const
{
    auto a = SIPAccountBase::getVolatileAccountDetails();
    a.emplace(Conf::CONFIG_ACCOUNT_REGISTRATION_STATE_CODE,
              std::to_string(registrationStateDetailed_.first));
    a.emplace(Conf::CONFIG_ACCOUNT_REGISTRATION_STATE_DESC, registrationStateDetailed_.second);

    if (presence_) {
        a.emplace(Conf::CONFIG_PRESENCE_STATUS, presence_->isOnline() ? TRUE_STR : FALSE_STR);
        a.emplace(Conf::CONFIG_PRESENCE_NOTE, presence_->getNote());
    }

    return a;
}

void
SIPAccount::setPushNotificationToken(const std::string& pushDeviceToken)
{
    SIP_CORE_WARN("[SIP Account %s] setPushNotificationToken: %s",
                  getAccountID().c_str(),
                  pushDeviceToken.c_str());

    if (config().deviceKey == pushDeviceToken)
        return;
    SIPAccountBase::setPushNotificationToken(pushDeviceToken);

    if (config().enabled)
        doUnregister([&](bool /* transport_free */) { doRegister(); });
}

void
SIPAccount::pushNotificationReceived(const std::string& from,
                                     const std::map<std::string, std::string>&)
{
    SIP_CORE_WARN("[SIP Account %s] pushNotificationReceived: %s",
                  getAccountID().c_str(),
                  from.c_str());

    if (config().enabled)
        doUnregister([&](bool /* transport_free */) { doRegister(); });
}

void
SIPAccount::doRegister()
{
    if (not isUsable()) {
        SIP_CORE_WARN("Account must be enabled and active to register, ignoring");
        return;
    }

    SIP_CORE_DEBUG("doRegister {:s}", config_->hostname);

    setRegistrationState(RegistrationState::TRYING);
    doRegister1_();
}

void
SIPAccount::doRegister1_()
{
    const auto resolveTransportType = transportTypeFromConfig(config().transport);

    // pre-resolve of proxy OR backup proxy
    if (hasServiceRoute() || hasBackServiceRoute()) {
        if (hasServiceRoute()) {
            link_.resolveSrvName(config().serviceRoute,
                                 resolveTransportType,
                                 [w = weak()](std::vector<IpAddr> host_ips) {
                                     if (auto acc = w.lock()) {
                                         std::lock_guard<std::recursive_mutex> lock(
                                             acc->configurationMutex_);
                                         if (host_ips.empty()) {
                                             SIP_CORE_ERR("Can't resolve serviceRoute for registration.");
                                             acc->setRegistrationState(RegistrationState::ERROR_GENERIC,
                                                                       PJSIP_SC_NOT_FOUND);
                                             return;
                                         }
                                         acc->serviceRouteIp_ = host_ips[0];
                                         acc->doRegister2_();
                                     }
                                 });
        }
        if (hasBackServiceRoute()) {
            link_.resolveSrvName(config().backServiceRoute,
                                 resolveTransportType,
                                 [w = weak()](std::vector<IpAddr> host_ips) {
                                     if (auto acc = w.lock()) {
                                         std::lock_guard<std::recursive_mutex> lock(
                                             acc->configurationMutex_);
                                         if (host_ips.empty()) {
                                             return;
                                         }
                                         acc->backServiceRouteIp_ = host_ips[0];
                                         if (acc->pendingBackupKeepAliveStart_.exchange(false)) {
                                             acc->startBackupKeepAliveAfterRegister();
                                         }
                                     }
                                 });
        }

    } else {
        // pre-resolve of hostname
        link_.resolveSrvName(config().hostname,
                             resolveTransportType,
                             [w = weak()](std::vector<IpAddr> host_ips) {
                                 if (auto acc = w.lock()) {
                                     std::lock_guard<std::recursive_mutex> lock(
                                         acc->configurationMutex_);
                                     if (host_ips.empty()) {
                                         SIP_CORE_ERR("Can't resolve hostname for registration.");

                                         acc->setRegistrationState(RegistrationState::ERROR_GENERIC,
                                                                   PJSIP_SC_NOT_FOUND);
                                         return;
                                     }
                                     acc->hostIp_ = host_ips[0];
                                     acc->doRegister2_();
                                 }
                             });
    }
}

const IpAddr&
SIPAccount::getActualIpAddress() const
{
    if (usingBackupRoute_ && hasBackServiceRoute()) {
        return backServiceRouteIp_;
    }

    if (hasServiceRoute()) {
        return serviceRouteIp_;
    }

    return hostIp_;
}

SIPAccount::KeepAliveTopology
SIPAccount::resolveKeepAliveTopology(bool hasServiceRoute, bool hasBackServiceRoute)
{
    if (hasServiceRoute && hasBackServiceRoute)
        return KeepAliveTopology::ServiceRouteWithBackup;
    if (hasServiceRoute)
        return KeepAliveTopology::ServiceRoute;
    return KeepAliveTopology::NoRoute;
}

bool
SIPAccount::shouldUseOptionsForKeepAlive(KeepAliveType keepAliveType, bool isUdpTransport)
{
    return !isUdpTransport || keepAliveType == KeepAliveType::Options;
}

bool
SIPAccount::shouldUseStartupMainRouteProbe(bool optionsKeepAliveMode,
                                           KeepAliveTopology topology,
                                           bool usingBackupRoute,
                                           bool skipStartupProbe)
{
    return optionsKeepAliveMode && topology == KeepAliveTopology::ServiceRouteWithBackup
           && !usingBackupRoute && !skipStartupProbe;
}

bool
SIPAccount::isOptionsKeepAliveMode() const
{
    return shouldUseOptionsForKeepAlive(config().keepAliveType,
                                        getTransportType() == PJSIP_TRANSPORT_UDP);
}

SIPAccount::KeepAliveTopology
SIPAccount::getKeepAliveTopology() const
{
    return resolveKeepAliveTopology(hasServiceRoute(), hasBackServiceRoute());
}

bool
SIPAccount::shouldRunStartupMainRouteProbe() const
{
    return shouldUseStartupMainRouteProbe(isOptionsKeepAliveMode(),
                                          getKeepAliveTopology(),
                                          isUsingBackupRoute(),
                                          skipMainRouteStartupProbeOnce_.load());
}

bool
SIPAccount::consumeShouldRunStartupMainRouteProbe()
{
    const bool skipStartupProbe = skipMainRouteStartupProbeOnce_.exchange(false);
    return shouldUseStartupMainRouteProbe(isOptionsKeepAliveMode(),
                                          getKeepAliveTopology(),
                                          isUsingBackupRoute(),
                                          skipStartupProbe);
}

std::string
SIPAccount::getServerUriForTarget(const std::string& target) const
{
    if (target.empty())
        return getServerUri();
    if (!target.empty() && target.front() == '<')
        return target;
    if (target.rfind("sip:", 0) == 0 || target.rfind("sips:", 0) == 0)
        return "<" + target + ">";
    return "<sip:" + target + ">";
}

std::string
SIPAccount::getActiveKeepAliveUri() const
{
    if (isUsingBackupRoute() && hasBackServiceRoute())
        return getBackupRouteKeepAliveUri();
    if (hasServiceRoute())
        return getMainRouteKeepAliveUri();
    return getServerUri();
}

std::string
SIPAccount::getMainRouteKeepAliveUri() const
{
    return hasServiceRoute() ? getServerUriForTarget(config().serviceRoute) : getServerUri();
}

std::string
SIPAccount::getBackupRouteKeepAliveUri() const
{
    return hasBackServiceRoute() ? getServerUriForTarget(config().backServiceRoute)
                                 : getServerUri();
}

bool
SIPAccount::updateActiveKeepAliveTargetFromActualIpAddress()
{
    const auto& ip = getActualIpAddress();
    if (!ip)
        return false;

    kaTarget.socket = {};
    pj_memcpy(&kaTarget.socket, ip.pjPtr(), ip.getLength());
    kaTarget.length = ip.getLength();
    return true;
}

void
SIPAccount::emitRegistrationStateSignal(RegistrationState state, unsigned details_code)
{
    std::string details_str;
    const pj_str_t* description = pjsip_get_status_text(details_code);
    if (description)
        details_str = sip_utils::as_view(*description);

    setRegistrationStateDetailed({static_cast<int>(details_code), details_str});

    runOnMainThread([accountId = accountID_,
                     state = mapStateNumberToString(state),
                     details_code,
                     details_str,
                     details = getVolatileAccountDetails()] {
        emitSignal<libsip_core::ConfigurationSignal::RegistrationStateChanged>(accountId,
                                                                               state,
                                                                               details_code,
                                                                               details_str);
        emitSignal<libsip_core::ConfigurationSignal::VolatileDetailsChanged>(accountId, details);
    });
}

void
SIPAccount::setRegistrationStateWithSignal(RegistrationState state,
                                           unsigned details_code,
                                           bool forceEmit)
{
    if (!forceEmit || state != getRegistrationState()) {
        setRegistrationState(state, details_code);
        return;
    }

    emitRegistrationStateSignal(state, details_code);
}

void
SIPAccount::reregisterCurrentRoute(const char* reason)
{
    if (isShuttingDown_.load() || !isUsable()) {
        SIP_CORE_DBG("Skipping route refresh for account %s: shutting down or unusable",
                     accountID_.c_str());
        return;
    }
    if (getRegistrationState() == RegistrationState::TRYING) {
        SIP_CORE_DBG("Skipping route refresh for account %s: registration already in progress",
                     accountID_.c_str());
        return;
    }

    SIP_CORE_WARN("Refreshing registration for account %s on current route (%s)",
                  accountID_.c_str(),
                  reason ? reason : "unspecified");
    needsResubscribe_.store(true);
    needsRepublish_.store(true);
    destroyRegistrationInfo();
    doRegister();
}

void
SIPAccount::handleStartupMainRouteProbeResult(int statusCode)
{
    if (isOptionsSuccess200(statusCode)) {
        mainRouteAvailable_.store(true);
        skipMainRouteStartupProbeOnce_.store(true);
        doRegister2_();
        return;
    }

    mainRouteAvailable_.store(false);
    switchRouteAndReregister(true, "startup-main-route-options-failure");
    enableMainRouteFastProbe("startup-main-route-options-failure");
}

void
SIPAccount::handleActiveRouteOptionsSuccess(int statusCode)
{
    if (!isOptionsSuccess200(statusCode))
        return;

    if (!isUsingBackupRoute() && hasServiceRoute())
        mainRouteAvailable_.store(true);

    if (!isActiveNoRouteFastProbeEnabled())
        return;
    if (getRegistrationState() == RegistrationState::TRYING) {
        SIP_CORE_DBG("Active route OPTIONS recovered while registration is already in progress");
        enableActiveNoRouteFastProbe("active-route-options-200-register-in-progress");
        return;
    }

    disableActiveNoRouteFastProbe("active-route-options-200");
    reregisterCurrentRoute("active-route-options-200");
}

void
SIPAccount::handleActiveRouteOptionsFailure(int statusCode)
{
    if (!shouldEnableActiveNoRouteFastProbeForStatusCode(statusCode))
        return;
    const bool noResponse = statusCode == PJSIP_SC_TSX_TRANSPORT_ERROR
                            || statusCode == PJSIP_SC_REQUEST_TIMEOUT;
    if (getRegistrationState() == RegistrationState::TRYING) {
        if (getKeepAliveTopology() == KeepAliveTopology::ServiceRouteWithBackup
            && isUsingBackupRoute()) {
            enableActiveNoRouteFastProbe("backup-route-options-failure-register-in-progress");
            enableMainRouteFastProbe("backup-route-options-failure-register-in-progress");
        } else if (noResponse || isActiveNoRouteFastProbeEnabled()) {
            enableActiveNoRouteFastProbe("active-route-options-failure-register-in-progress");
        }
        return;
    }

    if (getKeepAliveTopology() == KeepAliveTopology::ServiceRouteWithBackup) {
        if (!isUsingBackupRoute()) {
            mainRouteAvailable_.store(false);
            disableActiveNoRouteFastProbe("main-route-options-failure");
            switchRouteAndReregister(true, "main-route-options-failure");
            enableMainRouteFastProbe("main-route-options-failure");
            return;
        }

        enableActiveNoRouteFastProbe("backup-route-options-failure");
        enableMainRouteFastProbe("backup-route-options-failure");
        setRegistrationStateWithSignal(RegistrationState::ERROR_GENERIC, statusCode, true);
        return;
    }
    if (noResponse || isActiveNoRouteFastProbeEnabled())
        enableActiveNoRouteFastProbe("active-route-options-failure");
    setRegistrationStateWithSignal(RegistrationState::ERROR_GENERIC, statusCode, true);
}

bool
SIPAccount::sendStartupMainRouteProbe()
{
    if (!hasServiceRoute())
        return false;

    bool expected = false;
    if (!startupMainRouteProbePending_.compare_exchange_strong(expected, true))
        return true;

    auto mainRouteIp = getServiceRouteIp();
    if (!mainRouteIp) {
        startupMainRouteProbePending_.store(false);
        return false;
    }

    pjsip_tx_data* tdata = nullptr;
    auto server_uri = getMainRouteKeepAliveUri();
    pj_str_t pjServer = sip_utils::CONST_PJ_STR(server_uri);
    auto contact = getContactHeader();
    pj_str_t pjContact = sip_utils::CONST_PJ_STR(contact);

    pj_status_t status = pjsip_endpt_create_request(link_.getEndpoint(),
                                                    &pjsip_options_method,
                                                    &pjServer,
                                                    &pjContact,
                                                    &pjServer,
                                                    &pjContact,
                                                    nullptr,
                                                    -1,
                                                    nullptr,
                                                    &tdata);

    if (status == PJ_SUCCESS && setUpTransmissionData(tdata, mainRouteIp)) {
        status = pjsip_endpt_send_request(link_.getEndpoint(),
                                          tdata,
                                          -1,
                                          this,
                                          &startup_main_route_probe_on_complete);
    } else if (status == PJ_SUCCESS) {
        status = PJSIP_SC_TSX_TRANSPORT_ERROR;
        pjsip_tx_data_dec_ref(tdata);
    }

    if (status == PJ_SUCCESS) {
        SIP_CORE_INFO("KA_EVT: options-sent route=startup-main-probe");
        return true;
    }

    startupMainRouteProbePending_.store(false);
    mainRouteAvailable_.store(false);
    switchRouteAndReregister(true, "startup-main-route-options-failure");
    enableMainRouteFastProbe("startup-main-route-options-failure");
    return false;
}

void
SIPAccount::doRegister2_()
{
    try {
        bool result = switchTransportInternal(config().transport, false, false, nullptr);
        if (!result) {
            setRegistrationState(RegistrationState::ERROR_GENERIC);
            return;
        }

        if (consumeShouldRunStartupMainRouteProbe()) {
            sendStartupMainRouteProbe();
            return;
        }

        sendRegister();
    } catch (const VoipLinkException& e) {
        SIP_CORE_ERR("%s", e.what());
        setRegistrationState(RegistrationState::ERROR_GENERIC);
        return;
    }
}

void
SIPAccount::doUnregister(std::function<void(bool)> released_cb)
{
    std::unique_lock<std::recursive_mutex> lock(configurationMutex_);

    cancelKeepAliveTimer();
    cancelMainRouteKeepAliveTimer();
    cancelBackupRouteKeepAliveTimer();

    try {
        sendUnregister();
    } catch (const VoipLinkException& e) {
        SIP_CORE_ERR("doUnregister %s", e.what());
        // inform that error occured
        setRegistrationState(RegistrationState::ERROR_GENERIC);
    }

    lock.unlock();
    if (released_cb)
        released_cb(true);
}

void
SIPAccount::doUnregisterFireAndForget()
{
    std::lock_guard<std::recursive_mutex> lock(configurationMutex_);

    cancelKeepAliveTimer();
    cancelMainRouteKeepAliveTimer();
    cancelBackupRouteKeepAliveTimer();
    cancelAutoReregistrationTimer();
    resetAutoRegistration();

    // If not registered, just clean up regc and mark unregistered.
    if (!isRegistered()) {
        destroyRegistrationInfo();
        setRegistrationState(RegistrationState::UNREGISTERED);
        return;
    }

    bRegister_ = false;
    pjsip_regc* regc = getRegistrationInfo();
    if (!regc) {
        setRegistrationState(RegistrationState::UNREGISTERED);
        return;
    }

    // Send UNREGISTER, then immediately destroy regc.
    // pjsip_regc_destroy2(regc, PJ_TRUE) defers internal cleanup until the
    // final response but suppresses the callback — the packet IS still sent.
    try {
        const pjsip_tpselector tp_sel = getTransportSelector();
        if (!isTransportRecoveryActive() && tp_sel.type != PJSIP_TPSELECTOR_NONE) {
            pjsip_tx_data* tdata = nullptr;
            if (pjsip_regc_unregister(regc, &tdata) == PJ_SUCCESS) {
                pjsip_regc_set_transport(regc, &tp_sel);
                pjsip_regc_send(regc, tdata);
            }
        } else {
            SIP_CORE_WARN("Skipping fire-and-forget unregister send for account %s: transport is "
                          "being recovered",
                          accountID_.c_str());
        }
    } catch (...) {
        SIP_CORE_WARN("Fire-and-forget unregister send failed for account %s",
                      accountID_.c_str());
    }

    // Always destroy regc — suppresses any future callback.
    destroyRegistrationInfo();
    setRegistrationState(RegistrationState::UNREGISTERED);
}

void
SIPAccount::connectivityChanged()
{
    handleConnectivityChangedForced("connectivity-changed");
}

bool
SIPAccount::hasRunningTransportForConnectivityChange() const
{
    if (isShuttingDown_.load() || !isUsable())
        return false;

    if (!transport_ || transportRecoveryPending_.load())
        return false;

    switch (transport_->getTransportType()) {
    case libsip_core::TransportType::TCP:
        return transport_->isConnected() || transportStatus_ == PJSIP_SC_OK;
    case libsip_core::TransportType::UDP:
        // UDP is connectionless; transport presence is sufficient for "running".
        return true;
    default:
        return false;
    }
}

bool
SIPAccount::shouldHandleConnectivityChange() const
{
    const bool shuttingDown = isShuttingDown_.load();
    const bool usable = isUsable();
    const bool hasTransport = static_cast<bool>(transport_);
    const auto regState = getRegistrationState();
    const bool hasRegistrationIntent = bRegister_ || regState != RegistrationState::UNREGISTERED
                                       || hasTransport;
    const bool hasRunningTransport = hasRunningTransportForConnectivityChange();
    const bool pendingRecovery = transportRecoveryPending_.load();
    const bool inRecoverableErrorState =
        regState == RegistrationState::ERROR_GENERIC
        || regState == RegistrationState::ERROR_HOST
        || regState == RegistrationState::ERROR_SERVICE_UNAVAILABLE;
    // Defense-in-depth: if the account is stuck in TRYING with a dead transport
    // and no recovery pending, treat it as eligible so connectivityChanged can
    // rescue it (e.g. when a transport-error recovery was debounced and the
    // error-state fallback was not reached).
    const bool stuckInTrying = (regState == RegistrationState::TRYING)
                               && !hasRunningTransport
                               && !pendingRecovery;
    const bool eligible = !shuttingDown && usable && hasRegistrationIntent
                          && (hasRunningTransport || inRecoverableErrorState || stuckInTrying);

    SIP_CORE_DBG("Connectivity eligibility for account %s: eligible=%d, state=%s, bRegister=%d, "
                 "transportPresent=%d, runningTransport=%d, recoverableError=%d, "
                 "recoveryPending=%d, stuckTrying=%d, transportState=%d",
                 accountID_.c_str(),
                 eligible ? 1 : 0,
                 Account::mapStateNumberToString(regState).c_str(),
                 bRegister_ ? 1 : 0,
                 hasTransport ? 1 : 0,
                 hasRunningTransport ? 1 : 0,
                 inRecoverableErrorState ? 1 : 0,
                 pendingRecovery ? 1 : 0,
                 stuckInTrying ? 1 : 0,
                 static_cast<int>(transportStatus_));

    return eligible;
}

void
SIPAccount::handleConnectivityChangedForced(const char* reason)
{
    if (!shouldHandleConnectivityChange()) {
        SIP_CORE_DBG("Skipping forced connectivity recovery for account %s: registrationIntent=%d, "
                     "usable=%d, shuttingDown=%d",
                     accountID_.c_str(),
                     (bRegister_ || getRegistrationState() != RegistrationState::UNREGISTERED
                      || transport_)
                         ? 1
                         : 0,
                     isUsable() ? 1 : 0,
                     isShuttingDown_.load() ? 1 : 0);
        return;
    }
    prepareConnectivityRecovery(reason ? reason : "connectivity-changed");
    dispatchPreparedConnectivityRecovery(reason ? reason : "connectivity-changed");
}

void
SIPAccount::reinviteActiveCalls()
{
    auto callIds = getCallList();
    if (callIds.empty())
        return;

    SIP_CORE_WARN("Sending re-INVITE for %zu active call(s) on account %s after connectivity change",
                  callIds.size(),
                  accountID_.c_str());

    auto contactHdr = getContactHeader();

    for (const auto& id : callIds) {
        auto call = getCall(id);
        if (!call)
            continue;

        auto sipCall = std::dynamic_pointer_cast<SIPCall>(call);
        if (!sipCall)
            continue;

        // Reset retry state for a fresh connectivity change cycle
        sipCall->resetConnectivityReinviteState();

        const auto callState = sipCall->getState();
        const auto connState = sipCall->getConnectionState();
        std::string recoveryPeerNumber;
        std::vector<libsip_core::MediaMap> recoveryMediaList;
        const bool snapshotRequiresRedial = sipCall->consumeConnectivityRecoveryRedialSnapshot(
            recoveryPeerNumber, recoveryMediaList);

        if (SIPCall::shouldRedialAfterConnectivityRecovery(snapshotRequiresRedial,
                                                           sipCall->getCallType(),
                                                           connState)) {
            SIP_CORE_WARN("[call:%s] Outgoing call in setup phase (%s) during connectivity "
                          "change — cancelling and scheduling re-dial",
                          id.c_str(),
                          sipCall->getStateStr().c_str());

            if (recoveryPeerNumber.empty())
                recoveryPeerNumber = sipCall->getPeerNumber();
            if (recoveryMediaList.empty())
                recoveryMediaList = sipCall->currentMediaList();

            sipCall->clearConnectivityTransportResetExpectation();

            // Best-effort CANCEL on old transport + cleanup
            sipCall->hangup(0);

            // Re-dial on the new transport after a short delay
            std::weak_ptr<SIPAccount> wAcc
                = std::dynamic_pointer_cast<SIPAccount>(shared_from_this());
            Manager::instance().scheduleTaskIn(
                [wAcc,
                 peerNumber = std::move(recoveryPeerNumber),
                 mediaList = std::move(recoveryMediaList)] {
                    auto acc = wAcc.lock();
                    if (!acc || !acc->isUsable() || acc->isShuttingDown_.load())
                        return;

                    SIP_CORE_WARN("Re-dialing %s after connectivity change",
                                  peerNumber.c_str());
                    try {
                        acc->newOutgoingCall(peerNumber, mediaList);
                    } catch (const std::exception& e) {
                        SIP_CORE_ERR("Failed to re-dial after connectivity change: %s",
                                     e.what());
                    }
                },
                std::chrono::milliseconds(500));
            continue;
        }

        if (connState != Call::ConnectionState::CONNECTED
            || (callState != Call::CallState::ACTIVE && callState != Call::CallState::HOLD)) {

            // Fix 5: Non-outgoing calls (e.g. incoming ringing) — defer re-INVITE
            SIP_CORE_WARN("[call:%s] Not in re-invitable state (%s), deferring connectivity "
                          "re-INVITE",
                          id.c_str(),
                          sipCall->getStateStr().c_str());
            sipCall->pendingConnectivityReinvite_.store(true);
            sipCall->setSipTransport(transport_, contactHdr);
            sipCall->clearConnectivityTransportResetExpectation();

            // Register a state listener to trigger reinvite when call becomes eligible
            std::weak_ptr<SIPCall> wCall = sipCall;
            call->addStateListener(
                [wCall](Call::CallState cs, Call::ConnectionState cn, int) -> bool {
                    if (cn == Call::ConnectionState::CONNECTED
                        && (cs == Call::CallState::ACTIVE || cs == Call::CallState::HOLD)) {
                        if (auto c = wCall.lock()) {
                            c->tryDeferredConnectivityReinvite();
                        }
                        return false; // remove listener after triggering
                    }
                    // Remove listener if call is over
                    if (cs == Call::CallState::OVER || cn == Call::ConnectionState::DISCONNECTED)
                        return false;
                    return true; // keep listening
                });
            continue;
        }

        SIP_CORE_WARN("[call:%s] Updating transport and sending re-INVITE after connectivity change",
                      id.c_str());

        sipCall->setSipTransport(transport_, contactHdr);
        sipCall->clearConnectivityTransportResetExpectation();

        auto result = sipCall->reinviteOnConnectivityChange();
        if (result == PJ_EPENDING) {
            // Transaction was pending, deferred re-INVITE is already scheduled
            SIP_CORE_DBG("[call:%s] Re-INVITE deferred (pending transaction)", id.c_str());
        } else if (result != PJ_SUCCESS) {
            // Fix 4: Retry with backoff instead of immediate hangup
            scheduleConnectivityReinviteRetry(sipCall);
        }
    }
}

void
SIPAccount::scheduleConnectivityReinviteRetry(const std::shared_ptr<SIPCall>& sipCall)
{
    auto& retryCount = sipCall->connectivityReinviteRetryCount_;
    if (retryCount >= SIPCall::MAX_CONNECTIVITY_REINVITE_RETRIES) {
        SIP_CORE_ERR("[call:%s] Re-INVITE failed after %u retries, hanging up",
                     sipCall->getCallId().c_str(),
                     retryCount);
        sipCall->hangup(0);
        return;
    }

    retryCount++;
    const auto delayMs = std::chrono::milliseconds(1000 * (1 << (retryCount - 1))); // 1s, 2s, 4s

    SIP_CORE_WARN("[call:%s] Re-INVITE failed, scheduling retry %u/%u in %lld ms",
                  sipCall->getCallId().c_str(),
                  retryCount,
                  SIPCall::MAX_CONNECTIVITY_REINVITE_RETRIES,
                  static_cast<long long>(delayMs.count()));

    std::weak_ptr<SIPCall> wCall = sipCall;
    std::weak_ptr<SIPAccount> wAcc = std::dynamic_pointer_cast<SIPAccount>(shared_from_this());
    Manager::instance().scheduleTaskIn(
        [wCall, wAcc] {
            auto acc = wAcc.lock();
            auto call = wCall.lock();
            if (!acc || !call)
                return;
            if (call->getConnectionState() == Call::ConnectionState::DISCONNECTED
                || call->getState() == Call::CallState::OVER)
                return;

            SIP_CORE_WARN("[call:%s] Retrying connectivity re-INVITE (attempt %u)",
                          call->getCallId().c_str(),
                          call->connectivityReinviteRetryCount_);

            auto result = call->reinviteOnConnectivityChange();
            if (result == PJ_EPENDING) {
                // Will be retried when transaction completes
            } else if (result != PJ_SUCCESS) {
                acc->scheduleConnectivityReinviteRetry(call);
            }
        },
        delayMs);
}

void
SIPAccount::sendRegister()
{
    if (not isUsable()) {
        SIP_CORE_WARN("Account must be enabled and active to register, ignoring");
        return;
    }
    bRegister_ = true;
    setRegistrationState(RegistrationState::TRYING);

    pjsip_regc* regc = nullptr;
    if (pjsip_regc_create(link_.getEndpoint(), (void*) this, &registration_cb, &regc) != PJ_SUCCESS)
        throw VoipLinkException("UserAgent: Unable to create regc structure.");

    /* Set authentication preference */
    pjsip_auth_clt_pref conf {};
    pjsip_regc_set_prefs(regc, &conf);

    std::string srvUri(getServerUri());
    pj_str_t pjSrv {(char*) srvUri.data(), (pj_ssize_t) srvUri.size()};

    // Generate the FROM header
    std::string from(getFromUri());
    pj_str_t pjFrom(sip_utils::CONST_PJ_STR(from));

    std::string contact = getContactHeader();

    SIP_CORE_DBG("Using contact header %s in registration", contact.c_str());

    pj_status_t status = PJ_SUCCESS;
    pj_str_t pjContact = sip_utils::CONST_PJ_STR(contact);

    if ((status
         = pjsip_regc_init(regc, &pjSrv, &pjFrom, &pjFrom, 1, &pjContact, getRegistrationExpire()))
        != PJ_SUCCESS) {
        SIP_CORE_ERR("pjsip_regc_init failed with error %d: %s",
                     status,
                     sip_utils::sip_strerror(status).c_str());
        throw VoipLinkException("Unable to initialize account registration structure");
    }

    const pjsip_tpselector tp_sel = getTransportSelector();
    if (pjsip_regc_set_transport(regc, &tp_sel) != PJ_SUCCESS)
        throw VoipLinkException("Unable to set transport");

    std::string activeRoute = getActiveServiceRoute();
    if (!activeRoute.empty())
        pjsip_regc_set_route_set(regc, sip_utils::createRouteSet(activeRoute, link_.getPool()));

    pjsip_regc_set_credentials(regc, getCredentialCount(), getCredInfo());

    pjsip_hdr hdr_list;
    pj_list_init(&hdr_list);
    auto pjUserAgent = CONST_PJ_STR(getUserAgentName());
    constexpr pj_str_t STR_USER_AGENT = CONST_PJ_STR("User-Agent");

    pjsip_generic_string_hdr* h = pjsip_generic_string_hdr_create(link_.getPool(),
                                                                  &STR_USER_AGENT,
                                                                  &pjUserAgent);
    pj_list_push_back(&hdr_list, (pjsip_hdr*) h);

    constexpr pj_str_t STR_SV_AUTH = CONST_PJ_STR("X-Sv-Auth");
    pj_str_t STR_SV_HASH = pj_str(randomSvAuthString(32));
    pjsip_generic_string_hdr* sv_h = pjsip_generic_string_hdr_create(link_.getPool(),
                                                                     &STR_SV_AUTH,
                                                                     &STR_SV_HASH);
    pj_list_push_back(&hdr_list, (pjsip_hdr*) sv_h);

    pjsip_regc_add_headers(regc, &hdr_list);

    pjsip_tx_data* tdata;

    if (pjsip_regc_register(regc, isRegistrationRefreshEnabled(), &tdata) != PJ_SUCCESS)
        throw VoipLinkException("Unable to initialize transaction data for account registration");

    // pjsip_regc_send increment the transport ref count by one,
    if ((status = pjsip_regc_send(regc, tdata)) != PJ_SUCCESS) {
        SIP_CORE_ERR("pjsip_regc_send failed with error %d: %s",
                     status,
                     sip_utils::sip_strerror(status).c_str());
        throw VoipLinkException("Unable to send account registration request");
    }

    setRegistrationInfo(regc);
}

bool
SIPAccount::setUpTransmissionData(pjsip_tx_data* tdata)
{
    return setUpTransmissionData(tdata, getActualIpAddress());
}

bool
SIPAccount::setUpTransmissionData(pjsip_tx_data* tdata, const IpAddr& ip)
{
    if (!ip || !transport_) {
        return false;
    }

    auto length = ip.getLength();
    if (length == 0) {
        SIP_CORE_DBG("setUpTransmissionData: target IP has no length");
        return false;
    }

    if (transport_->getTransportType() == TransportType::UDP && length > sizeof(pj_sockaddr_in)) {
        SIP_CORE_WARN("setUpTransmissionData: skipping IPv6 target (%u bytes) on UDP transport "
                      "to avoid pj_ioqueue_sendto assert",
                      length);
        return false;
    }

    auto ai = &tdata->dest_info;
    ai->name = pj_strdup3(tdata->pool, config().hostname.c_str());
    ai->addr.count = 1;
    ai->addr.entry[0].type = transport_->getPjSipTransportType();
    pj_memcpy(&ai->addr.entry[0].addr, ip.pjPtr(), length);
    ai->addr.entry[0].addr_len = length;
    ai->cur_addr = 0;

    return true;
}

void
SIPAccount::onRegister(pjsip_regc_cbparam* param)
{
    if (isShuttingDown_.load()) {
        SIP_CORE_DBG("Ignoring onRegister for shutting-down account %s", accountID_.c_str());
        return;
    }

    if (param->regc != getRegistrationInfo())
        return;
    const bool optionsKeepAliveMode = isOptionsKeepAliveMode();
    const bool dualRouteOptionsMode = optionsKeepAliveMode
                                      && getKeepAliveTopology()
                                             == KeepAliveTopology::ServiceRouteWithBackup;

    if (param->status != PJ_SUCCESS) {
        // cancel ka timer if everything is BAD
        SIP_CORE_ERR("SIP registration error %d", param->status);
        cancelBackupRouteKeepAliveTimer();
        pendingBackupKeepAliveStart_.store(false);
        const unsigned failureCode = param->code > 0 ? param->code : PJSIP_SC_TSX_TRANSPORT_ERROR;

        if (dualRouteOptionsMode && isUsingBackupRoute() && !mainRouteAvailable_.load()) {
            destroyRegistrationInfo();
            if (updateActiveKeepAliveTargetFromActualIpAddress()) {
                enableActiveNoRouteFastProbe("backup-route-registration-transport-failure");
                registerKeepAliveTimer();
            }
            enableMainRouteFastProbe("backup-route-registration-transport-failure");
            setRegistrationStateWithSignal(RegistrationState::ERROR_GENERIC, failureCode, true);
            return;
        }

        if (shouldRecoverTransport(PJSIP_TP_STATE_DISCONNECTED, param->status)) {
            destroyRegistrationInfo();
            if (scheduleTransportRecovery("registration-error", param->status)) {
                return;
            }
            // Recovery was debounced — fall through to set error state
        }

        // Try backup route if not already using it
        if (dualRouteOptionsMode && !isUsingBackupRoute() && hasBackServiceRoute()) {
            SIP_CORE_WARN("Registration failed, trying backup route");
            mainRouteAvailable_.store(false);
            switchRouteAndReregister(true, "registration-failed-main-route");
            enableMainRouteFastProbe("registration-failed-main-route");
            return;
        }

        destroyRegistrationInfo();
        if (optionsKeepAliveMode && updateActiveKeepAliveTargetFromActualIpAddress()) {
            enableActiveNoRouteFastProbe("registration-transport-failure");
            registerKeepAliveTimer();
        }
        setRegistrationStateWithSignal(RegistrationState::ERROR_GENERIC, failureCode, true);
    } else if (param->code < 0 || param->code >= 300) {
        SIP_CORE_ERR("SIP registration failed, status=%d (%.*s)",
                     param->code,
                     (int) param->reason.slen,
                     param->reason.ptr);

        cancelBackupRouteKeepAliveTimer();
        pendingBackupKeepAliveStart_.store(false);

        // Try backup route if not already using it (for certain error codes)
        bool shouldRetryBackup = false;
        switch (param->code) {
        case PJSIP_SC_REQUEST_TIMEOUT:
        case PJSIP_SC_SERVICE_UNAVAILABLE:
        case PJSIP_SC_NOT_FOUND:
            shouldRetryBackup = dualRouteOptionsMode && !isUsingBackupRoute()
                                && hasBackServiceRoute();
            break;
        }

        if (shouldRetryBackup) {
            SIP_CORE_WARN("Registration failed with code %d, trying backup route", param->code);
            mainRouteAvailable_.store(false);
            destroyRegistrationInfo();
            switchRouteAndReregister(true, "registration-code-failure-main-route");
            enableMainRouteFastProbe("registration-code-failure-main-route");
            return;
        }

        if (isTransportFailureFromOptions(param->code)) {
            destroyRegistrationInfo();
            if (scheduleTransportRecovery("registration-transport-error", param->code)) {
                return;
            }
            // Recovery was debounced — fall through to set error state + schedule reregistration
        }

        destroyRegistrationInfo();
        if (dualRouteOptionsMode && isUsingBackupRoute() && !mainRouteAvailable_.load()) {
            if (updateActiveKeepAliveTargetFromActualIpAddress()) {
                enableActiveNoRouteFastProbe("backup-route-registration-code-failure");
                registerKeepAliveTimer();
            }
            enableMainRouteFastProbe("backup-route-registration-code-failure");
        } else if (optionsKeepAliveMode && updateActiveKeepAliveTargetFromActualIpAddress()) {
            if (param->code == PJSIP_SC_REQUEST_TIMEOUT || isActiveNoRouteFastProbeEnabled()) {
                enableActiveNoRouteFastProbe("registration-code-failure");
                registerKeepAliveTimer();
            }
        }
        switch (param->code) {
        case PJSIP_SC_FORBIDDEN:
            setRegistrationStateWithSignal(RegistrationState::ERROR_AUTH, param->code, true);
            break;
        case PJSIP_SC_NOT_FOUND:
            setRegistrationStateWithSignal(RegistrationState::ERROR_HOST, param->code, true);
            break;
        case PJSIP_SC_REQUEST_TIMEOUT:
            setRegistrationStateWithSignal(RegistrationState::ERROR_HOST, param->code, true);
            break;
        case PJSIP_SC_SERVICE_UNAVAILABLE:
            setRegistrationStateWithSignal(RegistrationState::ERROR_SERVICE_UNAVAILABLE,
                                           param->code,
                                           true);
            break;
        default:
            setRegistrationStateWithSignal(RegistrationState::ERROR_GENERIC, param->code, true);
        }
    } else if (PJSIP_IS_STATUS_IN_CLASS(param->code, 200)) {
        // Update auto registration flag
        resetAutoRegistration();

        auto fakeSubstr = sip_utils::CONST_PJ_STR("fake");

        if (pj_stristr(&param->reason, &fakeSubstr) != NULL) {
            // if fake cancel ka timer
            cancelKeepAliveTimer();
            setRegistrationState(RegistrationState::ERROR_FAKE, 500);
        } else {
            if (param->expiration < 1) {
                // if unregister check that ka timer is already destroyed
                cancelKeepAliveTimer();
                cancelMainRouteKeepAliveTimer();
                cancelBackupRouteKeepAliveTimer();
                destroyRegistrationInfo();
                SIP_CORE_DBG("Unregistration success");
                setRegistrationState(RegistrationState::UNREGISTERED, param->code);
            } else {
                SIP_CORE_INFO("KA_EVT: register-2xx account=%s code=%d",
                              accountID_.c_str(),
                              param->code);
                const pj_str_t applicationProxy = CONST_PJ_STR("Application-Proxy");
                auto* applicationProxyHdr = (pjsip_generic_string_hdr*)
                    pjsip_msg_find_hdr_by_name(param->rdata->msg_info.msg,
                                               &applicationProxy,
                                               nullptr);

                if (applicationProxyHdr) {
                    auto header = sip_utils::as_view(applicationProxyHdr->hvalue);
                    SIP_CORE_DBG() << "Found application proxy header: " << header;
                    Manager::instance().applicationProxy = header;
                } else {
                    Manager::instance().applicationProxy = "";
                }
                /* TODO Check and update SIP outbound status first, since the result
                 * will determine if we should update re-registration
                 */
                // update_rfc5626_status(acc, param->rdata);

                /* TODO Check and update Service-Route header */
                std::string activeRoute = getActiveServiceRoute();
                if (!activeRoute.empty())
                    pjsip_regc_set_route_set(param->regc,
                                             sip_utils::createRouteSet(activeRoute,
                                                                       link_.getPool()));
                if (!isUsingBackupRoute() && hasServiceRoute())
                    mainRouteAvailable_.store(true);

                setRegistrationState(RegistrationState::REGISTERED, param->code);
                disableActiveNoRouteFastProbe("register-2xx");
                resetOptionsRecoveryWindow();
                resetRawKeepAliveRecoveryWindow();

                runPostRegisterRecoverySync();

                /* https://github.com/pjsip/pjproject/issues/1607:
                 * Calculate the destination address from the original request. Some
                 * (broken) servers send the response using different source address
                 * than the one that receives the request, which is forbidden by RFC
                 * 3581.
                 */
                {
                    pjsip_transaction* tsx;
                    pjsip_tx_data* req;

                    tsx = pjsip_rdata_get_tsx(param->rdata);
                    PJ_ASSERT_ON_FAIL(tsx, return);

                    req = tsx->last_tx;

                    pj_memcpy(&kaTarget.socket, &req->tp_info.dst_addr, req->tp_info.dst_addr_len);
                    kaTarget.length = pj_sockaddr_get_len(&kaTarget.socket);
                }

                // only now set timer
                registerKeepAliveTimer();
                startBackupKeepAliveAfterRegister();

                // If a connectivity recovery deferred the reinvite, execute it now
                if (pendingReinviteAfterRegister_.exchange(false)) {
                    SIP_CORE_WARN("Registration succeeded after connectivity recovery for "
                                  "account %s, now re-inviting active calls",
                                  accountID_.c_str());
                    reinviteActiveCalls();
                }
            }
        }
    }
    if (config().allowIPAutoRewrite and checkNATAddress(param, link_.getPool()))
        SIP_CORE_WARN("New contact: %s", getContactHeader().c_str());

    /* Check if we need to auto retry registration. Basically, registration
     * failure codes triggering auto-retry are those of temporal failures
     * considered to be recoverable in relatively short term.
     */
    switch (param->code) {
    case PJSIP_SC_REQUEST_TIMEOUT:
    case PJSIP_SC_INTERNAL_SERVER_ERROR:
    case PJSIP_SC_BAD_GATEWAY:
    case PJSIP_SC_SERVICE_UNAVAILABLE:
    case PJSIP_SC_SERVER_TIMEOUT:
        scheduleReregistration();
        break;

    default:
        /* Global failure */
        if (PJSIP_IS_STATUS_IN_CLASS(param->code, 600))
            scheduleReregistration();
    }

    if (param->expiration != config().registrationExpire) {
        SIP_CORE_DBG("Registrar returned EXPIRE value [%u s] different from the requested [%u s]",
                     param->expiration,
                     config().registrationExpire);
        // NOTE: We don't alter the EXPIRE set by the user even if the registrar
        // returned a different value. PJSIP lib will set the proper timer for
        // the refresh, if the auto-regisration is enabled.
    }
}

static void
tsx_cb(struct pjsip_regc_tsx_cb_param* param)
{
    SIP_CORE_DBG() << "regc_tsx_cb -> " << param->cbparam.code << " " << param->cbparam.status;
    // auto account = static_cast<SIPAccount*>(param->cbparam.token);
}

void
SIPAccount::sendUnregister()
{
    // This may occurs if account failed to register and is in state INVALID
    if (!isRegistered()) {
        setRegistrationState(RegistrationState::UNREGISTERED);
        return;
    }

    bRegister_ = false;
    pjsip_regc* regc = getRegistrationInfo();

    if (!regc)
        throw VoipLinkException("Registration structure is NULL");

    const pjsip_tpselector tp_sel = getTransportSelector();
    if (isTransportRecoveryActive() || tp_sel.type == PJSIP_TPSELECTOR_NONE) {
        SIP_CORE_WARN("Skipping network unregister for account %s: transport is being recovered",
                      accountID_.c_str());
        destroyRegistrationInfo();
        setRegistrationState(RegistrationState::UNREGISTERED);
        return;
    }

    pjsip_tx_data* tdata = nullptr;
    if (pjsip_regc_unregister(regc, &tdata) != PJ_SUCCESS)
        throw VoipLinkException("Unable to unregister sip account");
    if (pjsip_regc_set_transport(regc, &tp_sel) != PJ_SUCCESS)
        throw VoipLinkException("Unable to set transport");

    pjsip_regc_set_reg_tsx_cb(regc, tsx_cb);

    pj_status_t status;

    if ((status = pjsip_regc_send(regc, tdata)) != PJ_SUCCESS) {
        SIP_CORE_ERR("pjsip_regc_send failed with error %d: %s",
                     status,
                     sip_utils::sip_strerror(status).c_str());
        throw VoipLinkException("Unable to send request to unregister sip account");
    }

    SIP_CORE_DBG() << "Unregister was guaranteed to be already sent";
}

void
SIPAccount::loadConfig()
{
    SIPAccountBase::loadConfig();
    setCredentials(config().credentials);
    enablePresence(config().presenceEnabled);
}

bool
SIPAccount::fullMatch(std::string_view username, std::string_view hostname) const
{
    return userMatch(username) and (proxyMatch(hostname) || hostnameMatch(hostname));
}

bool
SIPAccount::userMatch(std::string_view username) const
{
    return !username.empty() and username == config().username;
}

bool
SIPAccount::hostnameMatch(std::string_view hostname) const
{
    if (hostname == config().hostname)
        return true;
    const auto a = ip_utils::getAddrList(hostname);
    const auto b = ip_utils::getAddrList(config().hostname);
    return ip_utils::haveCommonAddr(a, b);
}

bool
SIPAccount::proxyMatch(std::string_view hostname) const
{
    if (hostname == config().serviceRoute || hostname == config().backServiceRoute)
        return true;
    const auto a = ip_utils::getAddrList(hostname);
    const auto b = ip_utils::getAddrList(config().hostname);
    return ip_utils::haveCommonAddr(a, b);
}

std::string
SIPAccount::getLoginName()
{
#ifndef _WIN32
    struct passwd* user_info = getpwuid(getuid());
    return user_info ? user_info->pw_name : "";
#elif defined(RING_UWP)
    return "Unknown";
#else
    DWORD size = UNLEN + 1;
    TCHAR username[UNLEN + 1];
    std::string uname;
    if (GetUserName((TCHAR*) username, &size)) {
        uname = sip_core::to_string(username);
    }
    return uname;
#endif
}

std::string
SIPAccount::getFromUri() const
{
    std::string scheme = "sip:";
    std::string transport;

    // Get login name if username is not specified
    const auto& conf = config();
    std::string username(conf.username.empty() ? getLoginName() : conf.username);
    std::string hostname(conf.hostname);

    // Get machine hostname if not provided
    if (hostname.empty()) {
        hostname = sip_utils::as_view(*pj_gethostname());
    }

    if (IpAddr::isIpv6(hostname))
        hostname = IpAddr(hostname).toString(false, true);

    std::string uri = "<" + scheme + username + "@" + hostname + transport + ">";
    if (not conf.displayName.empty())
        return "\"" + conf.displayName + "\" " + uri;
    return uri;
}

std::string
SIPAccount::getToUri(const std::string& username) const
{
    if (username.size() >= 5 && username.compare(0, 5, "<sip:") == 0) {
        return username;
    }

    std::string scheme;
    std::string transport;
    std::string hostname;

    scheme = "sip:";

    // Check if scheme is already specified
    if (username.size() >= 3 && (username.compare(0, 4, scheme) == 0))
        scheme = "";

    // Check if hostname is already specified
    if (username.find('@') == std::string::npos)
        hostname = config().hostname;

    if (not hostname.empty() and IpAddr::isIpv6(hostname))
        hostname = IpAddr(hostname).toString(false, true);

    auto ltSymbol = username.find('<') == std::string::npos ? "<" : "";
    auto gtSymbol = username.find('>') == std::string::npos ? ">" : "";

    return ltSymbol + scheme + username + (hostname.empty() ? "" : "@") + hostname + transport
           + gtSymbol;
}

std::string
SIPAccount::getServerUri() const
{
    std::string scheme = "sip:";
    std::string transport;

    std::string host;
    if (IpAddr::isIpv6(config().hostname))
        host = IpAddr(config().hostname).toString(false, true);
    else
        host = config().hostname;

    return "<" + scheme + host + transport + ">";
}

std::string
SIPAccount::getActiveServiceRoute() const
{
    if (usingBackupRoute_ && hasBackServiceRoute()) {
        return config().backServiceRoute;
    }
    return config().serviceRoute;
}

void
SIPAccount::switchToBackupRoute()
{
    if (!hasBackServiceRoute()) {
        SIP_CORE_WARN("No backup service route available");
        return;
    }

    if (usingBackupRoute_) {
        SIP_CORE_DBG("Already using backup service route");
        return;
    }

    markTransportRebindRequired("switch-to-backup-route");

    SIP_CORE_WARN("Switching to backup service route: %s", config().backServiceRoute.c_str());
    usingBackupRoute_ = true;
    mainRouteAvailable_.store(false);

    // Stop backup route keep-alive since backup becomes active route
    cancelBackupRouteKeepAliveTimer();
    disableActiveNoRouteFastProbe("switch-to-backup-route");

    disableMainRouteFastProbe("switch-to-backup-route");
    updateActiveKeepAliveTargetFromActualIpAddress();

    // Start the separate keep-alive to monitor main route availability
    registerMainRouteKeepAliveTimer();
}

void
SIPAccount::switchToMainRoute()
{
    if (!usingBackupRoute_) {
        SIP_CORE_DBG("Already using main service route");
        return;
    }

    markTransportRebindRequired("switch-to-main-route");

    SIP_CORE_WARN("Switching back to main service route: %s", config().serviceRoute.c_str());
    usingBackupRoute_ = false;
    disableActiveNoRouteFastProbe("switch-to-main-route");

    disableMainRouteFastProbe("switch-to-main-route");
    updateActiveKeepAliveTargetFromActualIpAddress();

    // Stop the main route keep-alive timer
    cancelMainRouteKeepAliveTimer();
}

IpAddr
SIPAccount::getContactAddress() const
{
    std::lock_guard<std::mutex> lock(contactMutex_);
    return contactAddress_;
}

std::string
SIPAccount::getContactHeader() const
{
    std::lock_guard<std::mutex> lock(contactMutex_);
    return contactHeader_;
}

void
SIPAccount::updateContactHeader()
{
    std::lock_guard<std::mutex> lock(contactMutex_);

    if (not transport_) {
        SIP_CORE_ERR("Transport not created yet");
        return;
    }

    if (not contactAddress_) {
        SIP_CORE_ERR("Invalid contact address: %s", contactAddress_.toString(true).c_str());
        return;
    }
    auto contactHdr = printContactHeader(config().username,
                                         config().displayName,
                                         contactAddress_.toString(false, true),
                                         contactAddress_.getPort(),
                                         config().deviceKey);

    contactHeader_ = std::move(contactHdr);
}

bool
SIPAccount::initContactAddress()
{
    // This method tries to determine the address to be used in the
    // contact header using the available information (current transport,
    // UPNP, STUN, ...). The contact address may be updated after the
    // registration using information sent by the registrar in the SIP
    // messages (see checkNATAddress).

    if (not transport_) {
        SIP_CORE_ERR("Transport not created yet");
        return {};
    }

    std::string address;
    pj_uint16_t port;

    // Init the address to the local address.
    link_.findLocalAddressFromTransport(transport_, config().hostname, address, port);

    if (not config().publishedSameasLocal) {
        address = getPublishedIpAddress().toString();
        port = config().publishedPort;
        SIP_CORE_DBG("Using published address %s and port %d", address.c_str(), port);
    } else {
        if (!receivedParameter_.empty()) {
            address = receivedParameter_;
            SIP_CORE_DBG("Using received address %s", address.c_str());
        }

        if (rPort_ > 0) {
            port = rPort_;
            SIP_CORE_DBG("Using received port %d", port);
        }
    }

    std::lock_guard<std::mutex> lock(contactMutex_);
    contactAddress_ = IpAddr(address);
    contactAddress_.setPort(port);

    return contactAddress_;
}

std::string
SIPAccount::printContactHeader(const std::string& username,
                               const std::string& displayName,
                               const std::string& address,
                               pj_uint16_t port,
                               const std::string& deviceKey)
{
    // This method generates SIP contact header field, with push
    // notification parameters if any.
    // Example without push notification:
    // John Doe<sips:jdoe@10.10.10.10:5060;transport=tls>
    // Example with push notification:
    // John Doe<sips:jdoe@10.10.10.10:5060;transport=tls;pn-provider=XXX;pn-param=YYY;pn-prid=ZZZ>

    std::string quotedDisplayName = displayName.empty() ? "" : "\"" + displayName + "\" ";

    std::ostringstream contact;
    auto scheme = "sip";
    auto transport = transport_->getTransportType() == libsip_core::TransportType::TCP
                         ? ";transport=TCP"
                         : "";

    contact << quotedDisplayName << "<" << scheme << ":" << username
            << (username.empty() ? "" : "@") << address << ":" << port << transport;

    if (not deviceKey.empty()) {
        contact
#if defined(__ANDROID__)
#elif defined(__Apple__)
            << ";pn-provider=" << PN_APNS
#endif
            << ";pn-param="
            << ";pn-prid=" << deviceKey;
    }
    contact << ">";
    return contact.str();
}

void
SIPAccount::setCredentials(const std::vector<SipAccountConfig::Credentials>& creds)
{
    cred_.clear();
    cred_.reserve(creds.size());

    for (auto& c : creds) {
        cred_.emplace_back(
            pjsip_cred_info {/*.realm     = */ CONST_PJ_STR(c.realm),
                             /*.scheme    = */ CONST_PJ_STR("digest"),
                             /*.username  = */ CONST_PJ_STR(c.username),
                             /*.data_type = */
                             (c.password_h != "" ? PJSIP_CRED_DATA_DIGEST
                                                 : PJSIP_CRED_DATA_PLAIN_PASSWD),
                             /*.data      = */
                             CONST_PJ_STR(c.password_h != "" ? c.password_h : c.password),
                             /*.ext       = */ {}});
    }
}

void
SIPAccount::setRegistrationState(RegistrationState state,
                                 unsigned details_code,
                                 const std::string& /*detail_str*/)
{
    std::string details_str;
    const pj_str_t* description = pjsip_get_status_text(details_code);
    if (description)
        details_str = sip_utils::as_view(*description);
    setRegistrationStateDetailed({details_code, details_str});
    SIPAccountBase::setRegistrationState(state, details_code, details_str);
}

SIPPresence*
SIPAccount::getPresence() const
{
    return presence_;
}

SIPEvents*
SIPAccount::getSIPEvents() const
{
    return sip_events_;
}

/**
 *  Enable the presence module
 */
void
SIPAccount::enablePresence(const bool& enabled)
{
    if (!presence_) {
        SIP_CORE_ERR("Presence not initialized");
        return;
    }

    SIP_CORE_DBG("Presence enabled for %s : %s.",
                 accountID_.c_str(),
                 enabled ? TRUE_STR : FALSE_STR);

    presence_->enable(enabled);
}

/**
 *  Set the presence (PUBLISH/SUBSCRIBE) support flags
 *  and process the change.
 */
void
SIPAccount::supportPresence(int function, bool enabled)
{
    if (!presence_) {
        SIP_CORE_ERR("Presence not initialized");
        return;
    }

    if (presence_->isSupported(function) == enabled)
        return;

    SIP_CORE_DBG("Presence support for %s (%s: %s).",
                 accountID_.c_str(),
                 function == PRESENCE_FUNCTION_PUBLISH ? "publish" : "subscribe",
                 enabled ? TRUE_STR : FALSE_STR);
    presence_->support(function, enabled);

    // force presence to disable when nothing is supported
    if (not presence_->isSupported(PRESENCE_FUNCTION_PUBLISH)
        and not presence_->isSupported(PRESENCE_FUNCTION_SUBSCRIBE))
        enablePresence(false);

    Manager::instance().saveConfig();
    // FIXME: bad signal used here, we need a global config changed signal.
    emitSignal<libsip_core::ConfigurationSignal::AccountsChanged>();
}

MatchRank
SIPAccount::matches(std::string_view userName, std::string_view server) const
{
    SIP_CORE_DBG("calling matches: current serviceRoute is %s, current backServiceRoute is %s, "
                 "username is %s",
                 config().serviceRoute.c_str(),
                 config().backServiceRoute.c_str(),
                 config().username.c_str());
    if (fullMatch(userName, server)) {
        SIP_CORE_DBG("Matching account id in request is a fullmatch %.*s@%.*s",
                     (int) userName.size(),
                     userName.data(),
                     (int) server.size(),
                     server.data());
        return MatchRank::FULL;
    } else if (hostnameMatch(server)) {
        SIP_CORE_DBG("Matching account id in request with hostname %.*s",
                     (int) server.size(),
                     server.data());
        return MatchRank::PARTIAL;
    } else if (userMatch(userName)) {
        SIP_CORE_DBG("Matching account id in request with username %.*s",
                     (int) userName.size(),
                     userName.data());
        return MatchRank::PARTIAL;
    } else if (proxyMatch(server)) {
        SIP_CORE_DBG("Matching account id in request with proxy %.*s",
                     (int) server.size(),
                     server.data());
        return MatchRank::PARTIAL;
    } else {
        return MatchRank::NONE;
    }
}

void
SIPAccount::destroyRegistrationInfo()
{
    if (!regc_)
        return;

    auto* regc = regc_;
    regc_ = nullptr;

    const auto status = pjsip_regc_destroy2(regc, PJ_TRUE);
    if (status != PJ_SUCCESS) {
        SIP_CORE_WARN("Failed to destroy registration client for account %s: %s",
                      accountID_.c_str(),
                      sip_utils::sip_strerror(status).c_str());
    }

    resetViaTransport();
}

void
SIPAccount::resetAutoRegistration()
{
    auto_rereg_.active = PJ_FALSE;
    auto_rereg_.attempt_cnt = 0;
    if (auto_rereg_.timer.user_data) {
        delete ((std::weak_ptr<SIPAccount>*) auto_rereg_.timer.user_data);
        auto_rereg_.timer.user_data = nullptr;
    }
}

bool
SIPAccount::checkNATAddress(pjsip_regc_cbparam* param, pj_pool_t* pool)
{
    SIP_CORE_DBG("[Account %s] Checking IP route after the registration", accountID_.c_str());

    pjsip_transport* tp = param->rdata->tp_info.transport;

    /* Get the received and rport info */
    pjsip_via_hdr* via = param->rdata->msg_info.via;
    int rport = 0;
    if (via->rport_param < 1) {
        /* Remote doesn't support rport */
        rport = via->sent_by.port;
        if (rport == 0) {
            pjsip_transport_type_e tp_type;
            tp_type = (pjsip_transport_type_e) tp->key.type;
            rport = pjsip_transport_get_default_port_for_type(tp_type);
        }
    } else {
        rport = via->rport_param;
    }

    const pj_str_t* via_addr = via->recvd_param.slen != 0 ? &via->recvd_param : &via->sent_by.host;
    std::string via_addrstr(sip_utils::as_view(*via_addr));
    /* Enclose IPv6 address in square brackets */
    if (IpAddr::isIpv6(via_addrstr))
        via_addrstr = IpAddr(via_addrstr).toString(false, true);

    SIP_CORE_DBG("Checking received VIA address: %s", via_addrstr.c_str());

    if (via_addr_.host.slen == 0 or via_tp_ != tp) {
        if (pj_strcmp(&via_addr_.host, via_addr))
            pj_strdup(pool, &via_addr_.host, via_addr);

        // Update Via header
        via_addr_.port = rport;
        via_tp_ = tp;
        pjsip_regc_set_via_sent_by(regc_, &via_addr_, via_tp_);
    }

    // Set published Ip address
    setPublishedAddress(IpAddr(via_addrstr));

    /* Compare received and rport with the URI in our registration */
    IpAddr contact_addr = getContactAddress();

    // TODO. Why note save the port in contact uri/header?
    if (contact_addr.getPort() == 0) {
        pjsip_transport_type_e tp_type;
        tp_type = (pjsip_transport_type_e) tp->key.type;
        contact_addr.setPort(pjsip_transport_get_default_port_for_type(tp_type));
    }

    /* Convert IP address strings into sockaddr for comparison.
     * (http://trac.pjsip.org/repos/ticket/863)
     */
    bool matched = false;
    IpAddr recv_addr {};
    auto status = pj_sockaddr_parse(pj_AF_UNSPEC(), 0, via_addr, recv_addr.pjPtr());
    recv_addr.setPort(rport);
    if (status == PJ_SUCCESS) {
        // Compare the addresses as sockaddr according to the ticket above
        matched = contact_addr == recv_addr;
    } else {
        // Compare the addresses as string, as before
        auto pjContactAddr = sip_utils::CONST_PJ_STR(contact_addr.toString());
        matched = (contact_addr.getPort() == rport and pj_stricmp(&pjContactAddr, via_addr) == 0);
    }

    if (matched) {
        // Address doesn't change
        return false;
    }

    /* Get server IP */
    IpAddr srv_ip = {std::string_view(param->rdata->pkt_info.src_name)};

    /* At this point we've detected that the address as seen by registrar.
     * has changed.
     */

    /* Do not switch if both Contact and server's IP address are
     * public but response contains private IP. A NAT in the middle
     * might have messed up with the SIP packets. See:
     * http://trac.pjsip.org/repos/ticket/643
     *
     * This exception can be disabled by setting allow_contact_rewrite
     * to 2. In this case, the switch will always be done whenever there
     * is difference in the IP address in the response.
     */
    if (not contact_addr.isPrivate() and not srv_ip.isPrivate() and recv_addr.isPrivate()) {
        /* Don't switch */
        // return false;
    }

    /* Also don't switch if only the port number part is different, and
     * the Via received address is private.
     * See http://trac.pjsip.org/repos/ticket/864
     */
    if (contact_addr == recv_addr and recv_addr.isPrivate()) {
        // return false;
    }

    SIP_CORE_WARN("[account %s] Contact address changed: "
                  "(%s --> %s:%d). Updating registration.",
                  accountID_.c_str(),
                  contact_addr.toString(true).c_str(),
                  via_addrstr.data(),
                  rport);

    /*
     * Build new Contact header
     */
    {
        auto tempContact = printContactHeader(config().username,
                                              config().displayName,
                                              via_addrstr,
                                              rport,
                                              config().deviceKey);

        if (tempContact.empty()) {
            SIP_CORE_ERR("Invalid contact header");
            return false;
        }

        // Update
        std::lock_guard<std::mutex> lock(contactMutex_);
        contactHeader_ = std::move(tempContact);
    }

    if (regc_ != nullptr) {
        auto contactHdr = getContactHeader();
        auto pjContact = sip_utils::CONST_PJ_STR(contactHdr);
        pjsip_regc_update_contact(regc_, 2, &pjContact);

        /*  Perform new registration at the next registration cycle */
    }

    return true;
}

/* Auto re-registration timeout callback */
void
SIPAccount::autoReregTimerCb()
{
    /* Check if the re-registration timer is still valid, e.g: while waiting
     * timeout timer application might have deleted the account or disabled
     * the auto-reregistration.
     */
    if (not auto_rereg_.active)
        return;

    /* Start re-registration */
    ++auto_rereg_.attempt_cnt;
    try {
        doRegister();
    } catch (const VoipLinkException& e) {
        SIP_CORE_ERR("Exception during SIP registration: %s", e.what());
        scheduleReregistration();
    }
}

/* Schedule reregistration for specified account. Note that the first
 * re-registration after a registration failure will be done immediately.
 * Also note that this function should be called within PJSUA mutex.
 */
void
SIPAccount::scheduleReregistration()
{
    if (!isUsable())
        return;

    /* Cancel any re-registration timer */
    if (auto_rereg_.timer.id) {
        auto_rereg_.timer.id = PJ_FALSE;
        pjsip_endpt_cancel_timer(link_.getEndpoint(), &auto_rereg_.timer);
    }

    /* Update re-registration flag */
    auto_rereg_.active = PJ_TRUE;

    /* Set up timer for reregistration */
    auto_rereg_.timer.cb = [](pj_timer_heap_t* /*th*/, pj_timer_entry* te) {
        if (auto sipAccount = static_cast<std::weak_ptr<SIPAccount>*>(te->user_data)->lock())
            sipAccount->autoReregTimerCb();
    };
    if (not auto_rereg_.timer.user_data)
        auto_rereg_.timer.user_data = new std::weak_ptr<SIPAccount>(weak());

    /* Reregistration attempt. The first attempt will be done sooner */
    pj_time_val delay;
    delay.sec = auto_rereg_.attempt_cnt ? REGISTRATION_RETRY_INTERVAL
                                        : REGISTRATION_FIRST_RETRY_INTERVAL;
    delay.msec = 0;

    /* Randomize interval by +/- 10 secs */
    if (delay.sec >= 10) {
        delay.msec = delay10ZeroDist_(rand);
    } else {
        delay.sec = 0;
        delay.msec = delay10PosDist_(rand);
    }

    pj_time_val_normalize(&delay);

    SIP_CORE_WARNING("Scheduling re-registration retry in {:d} seconds..", delay.sec);
    auto_rereg_.timer.id = PJ_TRUE;
    if (pjsip_endpt_schedule_timer(link_.getEndpoint(), &auto_rereg_.timer, &delay) != PJ_SUCCESS)
        auto_rereg_.timer.id = PJ_FALSE;
}

void
SIPAccount::updateDialogViaSentBy(pjsip_dialog* dlg)
{
    if (config().allowIPAutoRewrite && via_addr_.host.slen > 0)
        pjsip_dlg_set_via_sent_by(dlg, &via_addr_, via_tp_);
}

#if 0
/**
 * Create Accept header for MESSAGE.
 */
static pjsip_accept_hdr* im_create_accept(pj_pool_t *pool)
{
    /* Create Accept header. */
    pjsip_accept_hdr *accept;

    accept = pjsip_accept_hdr_create(pool);
    accept->values[0] = CONST_PJ_STR("text/plain");
    accept->values[1] = CONST_PJ_STR("application/im-iscomposing+xml");
    accept->count = 2;

    return accept;
}
#endif

void
SIPAccount::sendMessage(const std::string& to,
                        const std::map<std::string, std::string>& payloads,
                        uint64_t id,
                        bool,
                        bool)
{
    if (to.empty() or payloads.empty()) {
        SIP_CORE_WARN("No sender or payload");
        messageEngine_.onMessageSent(to, id, false);
        return;
    }

    auto toUri = getToUri(to);

    constexpr pjsip_method msg_method = {PJSIP_OTHER_METHOD,
                                         CONST_PJ_STR(sip_utils::SIP_METHODS::MESSAGE)};
    std::string from(getFromUri());
    pj_str_t pjFrom = sip_utils::CONST_PJ_STR(from);
    pj_str_t pjTo = sip_utils::CONST_PJ_STR(toUri);

    /* Create request. */
    pjsip_tx_data* tdata;
    pj_status_t status = pjsip_endpt_create_request(link_.getEndpoint(),
                                                    &msg_method,
                                                    &pjTo,
                                                    &pjFrom,
                                                    &pjTo,
                                                    nullptr,
                                                    nullptr,
                                                    -1,
                                                    nullptr,
                                                    &tdata);
    if (status != PJ_SUCCESS) {
        SIP_CORE_ERR("Unable to create request: %s", sip_utils::sip_strerror(status).c_str());
        messageEngine_.onMessageSent(to, id, false);
        return;
    }

    /* Add Date Header. */
    pj_str_t date_str;
    constexpr auto key = CONST_PJ_STR("Date");
    pjsip_hdr* hdr;
    auto time = std::time(nullptr);
    auto date = std::ctime(&time);
    // the erase-remove idiom for a cstring, removes _all_ new lines with in date
    *std::remove(date, date + strlen(date), '\n') = '\0';

    // Add Header
    hdr = reinterpret_cast<pjsip_hdr*>(
        pjsip_date_hdr_create(tdata->pool, &key, pj_cstr(&date_str, date)));
    pjsip_msg_add_hdr(tdata->msg, hdr);

    // Add user-agent header
    sip_utils::addUserAgentHeader(getUserAgentName(), tdata);

    // Set input token into callback
    std::unique_ptr<ctx> t {new ctx(new pjsip_auth_clt_sess)};
    t->acc = shared();
    t->to = to;
    t->id = id;

    /* Initialize Auth header. */
    status = pjsip_auth_clt_init(t->auth_sess.get(), link_.getEndpoint(), tdata->pool, 0);

    if (status != PJ_SUCCESS) {
        SIP_CORE_ERR("Unable to initialize auth session: %s",
                     sip_utils::sip_strerror(status).c_str());
        messageEngine_.onMessageSent(to, id, false);
        return;
    }

    status = pjsip_auth_clt_set_credentials(t->auth_sess.get(), getCredentialCount(), getCredInfo());

    if (status != PJ_SUCCESS) {
        SIP_CORE_ERR("Unable to set auth session data: %s", sip_utils::sip_strerror(status).c_str());
        messageEngine_.onMessageSent(to, id, false);
        return;
    }

    if (!setUpTransmissionData(tdata)) {
        SIP_CORE_ERR("Unable to set transport: destination not usable with current transport");
        messageEngine_.onMessageSent(to, id, false);
        return;
    }

    im::fillPJSIPMessageBody(*tdata, payloads);

    // Send message request with callback SendMessageOnComplete
    status = pjsip_endpt_send_request(link_.getEndpoint(), tdata, -1, t.release(), &onComplete);

    if (status != PJ_SUCCESS) {
        SIP_CORE_ERR("Unable to send request: %s", sip_utils::sip_strerror(status).c_str());
        messageEngine_.onMessageSent(to, id, false);
        return;
    }
}

void
SIPAccount::onComplete(void* token, pjsip_event* event)
{
    std::unique_ptr<ctx> c {(ctx*) token};
    int code;
    pj_status_t status;
    pj_assert(event->type == PJSIP_EVENT_TSX_STATE);
    code = event->body.tsx_state.tsx->status_code;

    auto acc = c->acc.lock();
    if (not acc)
        return;

    // Check if Authorization Header if needed (request rejected by server)
    if (code == PJSIP_SC_UNAUTHORIZED || code == PJSIP_SC_PROXY_AUTHENTICATION_REQUIRED) {
        SIP_CORE_INFO("Authorization needed for SMS message - Resending");
        pjsip_tx_data* new_request;

        // Add Authorization Header into msg
        status = pjsip_auth_clt_reinit_req(c->auth_sess.get(),
                                           event->body.tsx_state.src.rdata,
                                           event->body.tsx_state.tsx->last_tx,
                                           &new_request);

        if (status == PJ_SUCCESS) {
            // Increment Cseq number by one manually
            pjsip_cseq_hdr* cseq_hdr;
            cseq_hdr = (pjsip_cseq_hdr*) pjsip_msg_find_hdr(new_request->msg, PJSIP_H_CSEQ, NULL);
            cseq_hdr->cseq += 1;

            // Resend request
            auto to = c->to;
            auto id = c->id;
            status = pjsip_endpt_send_request(acc->link_.getEndpoint(),
                                              new_request,
                                              -1,
                                              c.release(),
                                              &onComplete);

            if (status != PJ_SUCCESS) {
                SIP_CORE_ERR("Unable to send request: %s", sip_utils::sip_strerror(status).c_str());
                acc->messageEngine_.onMessageSent(to, id, false);
            }
            return;
        } else {
            SIP_CORE_ERR("Unable to add Authorization Header into msg");
            acc->messageEngine_.onMessageSent(c->to, c->id, false);
            return;
        }
    }
    acc->messageEngine_.onMessageSent(c->to,
                                      c->id,
                                      event && event->body.tsx_state.tsx
                                          && (event->body.tsx_state.tsx->status_code == PJSIP_SC_OK
                                              || event->body.tsx_state.tsx->status_code
                                                     == PJSIP_SC_ACCEPTED));
}

std::string
SIPAccount::getUserUri() const
{
    return getFromUri();
}

IpAddr
SIPAccount::createBindingAddress()
{
    auto family = PJ_AF_INET;
    const auto& conf = config();

    IpAddr ret = conf.bindAddress.empty()
                     ? (conf.interface == ip_utils::DEFAULT_INTERFACE
                            ? ip_utils::getAnyHostAddr(family)
                            : ip_utils::getInterfaceAddr(getLocalInterface(), family))
                     : IpAddr(conf.bindAddress, family);

    if (ret.getPort() == 0) {
        ret.setPort(conf.localPort);
    }

    return ret;
}

void
SIPAccount::setActiveCodecs(const std::vector<unsigned>& list)
{
    Account::setActiveCodecs(list);
    config_->activeCodecs = getActiveCodecs(MEDIA_ALL);
}

} // namespace sip_core
