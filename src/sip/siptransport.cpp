/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
 *  Author: Alexandre Savard <alexandre.savard@savoirfairelinux.com>
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

#include "sip/siptransport.h"
#include "connectivity/sip_utils.h"
#include "connectivity/ip_utils.h"

#include "compiler_intrinsics.h"
#include "sip/sipvoiplink.h"

#include <pjsip.h>
#include <pjsip/sip_types.h>
#include <pjsip/sip_transport_tls.h>
#include <pj/ssl_sock.h>
#include <pjnath.h>
#include <pjnath/stun_config.h>
#include <pjlib.h>
#include <pjlib-util.h>

#include <stdexcept>
#include <sstream>
#include <algorithm>

#define RETURN_IF_FAIL(A, VAL, ...) \
    if (!(A)) { \
        SIP_CORE_ERR(__VA_ARGS__); \
        return (VAL); \
    }

namespace sip_core {

constexpr const char* TRANSPORT_STATE_STR[] = {"CONNECTED",
                                               "DISCONNECTED",
                                               "SHUTDOWN",
                                               "DESTROY",
                                               "UNKNOWN STATE"};
constexpr const size_t TRANSPORT_STATE_SZ = std::size(TRANSPORT_STATE_STR);

void
SipTransport::deleteTransport(pjsip_transport* t)
{
    // pjsip_transport_dec_ref(t);
}

SipTransport::SipTransport(pjsip_transport* t)
    : transport_(nullptr, deleteTransport)
{
    if (not t or pjsip_transport_add_ref(t) != PJ_SUCCESS)
        throw std::runtime_error("invalid transport");

    // Set pointer here, right after the successful pjsip_transport_add_ref
    transport_.reset(t);

    SIP_CORE_DEBUG("SipTransport@{} tr={} rc={:d}",
                 fmt::ptr(this),
                 fmt::ptr(transport_.get()),
                 pj_atomic_get(transport_->ref_cnt));
}

SipTransport::SipTransport(pjsip_transport* t, const std::shared_ptr<TcpListener>& l)
    : SipTransport(t)
{
    tcpListener_ = l;
}

SipTransport::~SipTransport()
{
    SIP_CORE_DEBUG("~SipTransport@{} tr={} rc={:d}",
                 fmt::ptr(this),
                 fmt::ptr(transport_.get()),
                 pj_atomic_get(transport_->ref_cnt));
}

bool
SipTransport::isAlive(pjsip_transport_state state)
{
    return state != PJSIP_TP_STATE_DISCONNECTED && state != PJSIP_TP_STATE_SHUTDOWN
           && state != PJSIP_TP_STATE_DESTROY;
}

const char*
SipTransport::stateToStr(pjsip_transport_state state)
{
    return TRANSPORT_STATE_STR[std::min<size_t>(state, TRANSPORT_STATE_SZ - 1)];
}

void
SipTransport::stateCallback(pjsip_transport_state state, const pjsip_transport_state_info* info)
{
    connected_ = state == PJSIP_TP_STATE_CONNECTED;

    std::vector<SipTransportStateCallback> cbs;
    {
        std::lock_guard<std::mutex> lock(stateListenersMutex_);
        cbs.reserve(stateListeners_.size());
        for (auto& l : stateListeners_)
            cbs.push_back(l.second);
    }
    for (auto& cb : cbs)
        cb(state, info);
}

void
SipTransport::addStateListener(uintptr_t lid, SipTransportStateCallback cb)
{
    std::lock_guard<std::mutex> lock(stateListenersMutex_);
    auto pair = stateListeners_.insert(std::make_pair(lid, cb));
    if (not pair.second)
        pair.first->second = cb;
}

bool
SipTransport::removeStateListener(uintptr_t lid)
{
    std::lock_guard<std::mutex> lock(stateListenersMutex_);
    auto it = stateListeners_.find(lid);
    if (it != stateListeners_.end()) {
        stateListeners_.erase(it);
        return true;
    }
    return false;
}

SipTransportBroker::SipTransportBroker(pjsip_endpoint* endpt)
    : endpt_(endpt)
{}

SipTransportBroker::~SipTransportBroker()
{
    shutdown();

    udpTransports_.clear();
    tcpTransports_.clear();
    transports_.clear();

    SIP_CORE_DBG("destroying SipTransportBroker@%p", this);
}

void
SipTransportBroker::transportStateChanged(pjsip_transport* tp,
                                          pjsip_transport_state state,
                                          const pjsip_transport_state_info* info)
{
    SIP_CORE_DBG("pjsip transport@%p %s -> %s", tp, tp->info, SipTransport::stateToStr(state));

    // First make sure that this transport is handled by us
    // and remove it from any mapping if destroy pending or done.

    std::shared_ptr<SipTransport> sipTransport;
    // std::lock_guard<std::mutex> lock(transportMapMutex_);
    auto key = transports_.find(tp);
    if (key == transports_.end())
        return;

    sipTransport = key->second.lock();

    if (!isDestroying_ && state == PJSIP_TP_STATE_DESTROY) {
        // maps cleanup
        SIP_CORE_DBG("unmap pjsip transport@%p {SipTransport@%p}", tp, sipTransport.get());
        transports_.erase(key);

        // If UDP
        const auto type = tp->key.type;
        if (type == PJSIP_TRANSPORT_UDP or type == PJSIP_TRANSPORT_UDP6) {
            const auto updKey = std::find_if(udpTransports_.cbegin(),
                                             udpTransports_.cend(),
                                             [tp](const std::pair<IpAddr, pjsip_transport*>& pair) {
                                                 return pair.second == tp;
                                             });
            if (updKey != udpTransports_.cend())
                udpTransports_.erase(updKey);
        }
    }

    // Propagate the event to the appropriate transport
    // Note the SipTransport may not be in our mappings if marked as dead
    if (sipTransport)
        sipTransport->stateCallback(state, info);
}

std::shared_ptr<SipTransport>
SipTransportBroker::addTransport(pjsip_transport* t)
{
    if (t) {
        std::lock_guard<std::mutex> lock(transportMapMutex_);

        auto key = transports_.find(t);
        if (key != transports_.end()) {
            if (auto sipTr = key->second.lock())
                return sipTr;
        }

        auto sipTr = std::make_shared<SipTransport>(t);
        if (key != transports_.end())
            key->second = sipTr;
        else
            transports_.emplace(std::make_pair(t, sipTr));
        return sipTr;
    }

    return nullptr;
}

void
SipTransportBroker::shutdown()
{
    std::unique_lock<std::mutex> lock(transportMapMutex_);
    isDestroying_ = true;
    for (auto& t : transports_) {
        if (auto transport = t.second.lock()) {
            pjsip_transport_shutdown(transport->get());
        }
    }
}

std::shared_ptr<SipTransport>
SipTransportBroker::getUdpTransport(const IpAddr& ipAddress)
{
    std::lock_guard<std::mutex> lock(transportMapMutex_);
    auto itp = udpTransports_.find(ipAddress);
    if (itp != udpTransports_.end()) {
        auto it = transports_.find(itp->second);
        if (it != transports_.end()) {
            if (auto spt = it->second.lock()) {
                SIP_CORE_DBG("Reusing transport %s", ipAddress.toString(true).c_str());
                return spt;
            } else {
                // Transport still exists but have not been destroyed yet.
                SIP_CORE_WARN("Recycling transport %s", ipAddress.toString(true).c_str());
                auto ret = std::make_shared<SipTransport>(itp->second);
                it->second = ret;
                return ret;
            }
        } else {
            SIP_CORE_WARN("Cleaning up UDP transport %s", ipAddress.toString(true).c_str());
            udpTransports_.erase(itp);
        }
    }
    auto ret = createUdpTransport(ipAddress);
    if (ret) {
        udpTransports_[ipAddress] = ret->get();
        transports_[ret->get()] = ret;
    }
    return ret;
}

std::shared_ptr<SipTransport>
SipTransportBroker::createUdpTransport(const IpAddr& ipAddress)
{
    RETURN_IF_FAIL(ipAddress, nullptr, "Could not determine IP address for this transport");

    pjsip_udp_transport_cfg pj_cfg;
    pjsip_udp_transport_cfg_default(&pj_cfg, ipAddress.getFamily());
    pj_cfg.bind_addr = ipAddress;
    pjsip_transport* transport = nullptr;
    if (pj_status_t status = pjsip_udp_transport_start2(endpt_, &pj_cfg, &transport)) {
        SIP_CORE_ERR("pjsip_udp_transport_start2 failed with error %d: %s",
                 status,
                 sip_utils::sip_strerror(status).c_str());
        SIP_CORE_ERR("UDP IPv%s Transport did not start on %s",
                 ipAddress.isIpv4() ? "4" : "6",
                 ipAddress.toString(true).c_str());
        return nullptr;
    }

    SIP_CORE_DBG("Created UDP transport on address %s", ipAddress.toString(true).c_str());
    return std::make_shared<SipTransport>(transport);
}

pjsip_tpfactory *
SipTransportBroker::createTcpTransport(const IpAddr& ipAddress)
{
    RETURN_IF_FAIL(ipAddress, nullptr, "Could not determine IP address for this transport");

    pjsip_tcp_transport_cfg pj_cfg;
    pjsip_tcp_transport_cfg_default(&pj_cfg, ipAddress.getFamily());
    pj_cfg.bind_addr = ipAddress;
    pjsip_tpfactory *tcp;
    if (pj_status_t status = pjsip_tcp_transport_start3(endpt_, &pj_cfg, &tcp)) {
        SIP_CORE_ERR("pjsip_tcp_transport_start3 failed with error %d: %s",
                 status,
                 sip_utils::sip_strerror(status).c_str());
        SIP_CORE_ERR("TCP IPv%s Transport did not start on %s",
                 ipAddress.isIpv4() ? "4" : "6",
                 ipAddress.toString(true).c_str());
        return nullptr;
    }

    SIP_CORE_DBG("Created TCP transport on address %s", ipAddress.toString(true).c_str());
    return tcp;
}

std::shared_ptr<TcpListener>
SipTransportBroker::getTcpListener(const IpAddr& ipAddress)
{
    RETURN_IF_FAIL(ipAddress, nullptr, "Could not determine IP address for TCP local listener");
    SIP_CORE_DEBUG("Creating local TCP listener on {:s}...", ipAddress.toString(true));

    pjsip_tpfactory* listener = createTcpTransport(ipAddress);
    if (listener == nullptr) {
        SIP_CORE_ERR("TLS local listener did not start because transport was not created.");
        return nullptr;
    }
    return std::make_shared<TcpListener>(listener);
}

std::shared_ptr<SipTransport>
SipTransportBroker::getTcpTransport(const std::shared_ptr<TcpListener>& l,
                                    const IpAddr& remote,
                                    const std::string& remote_name)
{
    if (!l || !remote)
        return nullptr;
    IpAddr remoteAddr {remote};
    if (remoteAddr.getPort() == 0)
        remoteAddr.setPort(pjsip_transport_get_default_port_for_type(l->get()->type));

    SIP_CORE_DBG("Get new TCP transport to %s", remoteAddr.toString(true).c_str());
    pjsip_tpselector sel;
    sel.type = PJSIP_TPSELECTOR_LISTENER;
    sel.u.listener = l->get();
    sel.disable_connection_reuse = PJ_FALSE;

    pjsip_tx_data tx_data;
    tx_data.dest_info.name = pj_str_t {(char*) remote_name.data(), (pj_ssize_t) remote_name.size()};

    pjsip_transport* transport = nullptr;
    pj_status_t status = pjsip_endpt_acquire_transport2(endpt_,
                                                        l->get()->type,
                                                        remoteAddr.pjPtr(),
                                                        remoteAddr.getLength(),
                                                        &sel,
                                                        remote_name.empty() ? nullptr : &tx_data,
                                                        &transport);

    if (!transport || status != PJ_SUCCESS) {
        SIP_CORE_ERR("Could not get new TCP transport: %s", sip_utils::sip_strerror(status).c_str());
        return nullptr;
    }

    auto ret = std::make_shared<SipTransport>(transport, l);

    tcpTransports_[remote] = ret->get();
    transports_[ret->get()] = ret;

    // TODO: that's because pjsip_endpt_acquire_transport2 adds ref, but we need to destroy it in destructor
    pjsip_transport_dec_ref(transport);
    {
        std::lock_guard<std::mutex> lock(transportMapMutex_);
        transports_[ret->get()] = ret;
    }
    return ret;
}

} // namespace sip_core
