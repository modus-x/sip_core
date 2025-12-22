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
#include "connectivity/ip_utils.h"
#include "connectivity/sip_utils.h"
#include "logger.h"

#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <iterator>
#include <mutex>
#include <pj/os.h>
#include <pj/types.h>
#include <pjsip/sip_transport.h>
#include <pjsip/sip_transport_tcp.h>
#include <pjsip/sip_transport_udp.h>
#include <pjsip/sip_types.h>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

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

static bool brokerDestroying {false};

void
UDPTransport::deleteTransport(pjsip_transport* t)
{
    // pjsip_transport_dec_ref(t);
}

UDPTransport::UDPTransport(pjsip_transport* t)
    : SipTransport()
    , transport_(nullptr, &deleteTransport)
{
    //    if (not t or pjsip_transport_add_ref(t) != PJ_SUCCESS)
    //        throw std::runtime_error("invalid transport for UDP, because cannot add ref");

    // Set pointer here, right after the successful pjsip_transport_add_ref
    transport_.reset(t);
    //
    //    SIP_CORE_DEBUG("UDPTransport@{} tr={} rc={:d}",
    //                   fmt::ptr(this),
    //                   fmt::ptr(transport_.get()),
    //                   pj_atomic_get(transport_->ref_cnt));
}

UDPTransport::~UDPTransport()
{
    //    SIP_CORE_DEBUG("~UDPTransport@{} tr={} rc={:d}",
    //                   fmt::ptr(this),
    //                   fmt::ptr(transport_.get()),
    //                   pj_atomic_get(transport_->ref_cnt));
}

TCPTransport::TCPTransport(pjsip_tpfactory* factory)
    : connection_factory_(factory)
{
    SIP_CORE_DEBUG("TCPTransport@{} tf={}", fmt::ptr(this), fmt::ptr(connection_factory_));
}

TCPTransport::~TCPTransport()
{
    if (!brokerDestroying) {
        get_factory()->destroy(get_factory());
    }
    SIP_CORE_DEBUG("~TCPTransport@{} tf={}", fmt::ptr(this), fmt::ptr(connection_factory_));
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
        std::lock_guard<std::mutex> const lock(stateListenersMutex_);
        cbs.reserve(stateListeners_.size());
        for (auto& l : stateListeners_)
            cbs.push_back(l.second);
    }
    for (auto& cb : cbs)
        cb(state, info);
}

void
SipTransport::addStateListener(uintptr_t lid, const SipTransportStateCallback& cb)
{
    std::lock_guard<std::mutex> const lock(stateListenersMutex_);
    auto pair = stateListeners_.insert(std::make_pair(lid, cb));
    if (not pair.second)
        pair.first->second = cb;
}

bool
SipTransport::removeStateListener(uintptr_t lid)
{
    std::lock_guard<std::mutex> const lock(stateListenersMutex_);
    auto it = stateListeners_.find(lid);
    if (it != stateListeners_.end()) {
        stateListeners_.erase(it);
        return true;
    }
    return false;
}

pjsip_transport_type_e
SipTransport::getPjSipTransportType() const
{
    switch (this->getTransportType()) {
    case TransportType::TCP:
        return PJSIP_TRANSPORT_TCP;
    default:
        return PJSIP_TRANSPORT_UDP;
    }
}

SipTransportBroker::SipTransportBroker(pjsip_endpoint* endpt)
    : endpt_(endpt)
{}

SipTransportBroker::~SipTransportBroker()
{
    // initiate shutdowning of all available transports
    shutdown();

    SIP_CORE_DBG("destroying SipTransportBroker@%p", this);
}

void
SipTransportBroker::transportStateChanged(pjsip_transport* tp,
                                          pjsip_transport_state state,
                                          const pjsip_transport_state_info* info)
{
    std::lock_guard<std::mutex> const lock(transportMapMutex_);

    // construct IpAddr from pjsip_transport state
    auto ipAddress = IpAddr(tp->local_addr);

    SIP_CORE_DBG() << "transportStateChanged for " << ipAddress << " ptp " << tp << " info "
                   << tp->info << " state: " << SipTransport::stateToStr(state);

    // First make sure that this transport is handled by us
    // and remove it from any mapping if destroy pending or done.
    std::shared_ptr<SipTransport> sipTransport;

    auto pjsipType = pjsip_transport_get_type_from_flag(tp->flag);

    if (pjsipType == PJSIP_TRANSPORT_TCP) {
        sipTransport = tcpTransport_.lock();
    } else if (pjsipType == PJSIP_TRANSPORT_UDP) {
        sipTransport = udpTransport_.lock();
    }
    // what is this transport ???
    else {
        return;
    }

    // all destroyed transport related data should be erased from memory
    // ignore if we called shutdown on broker before (meaning maps are not useful anymore)
    if (!isDestroying_ && state == PJSIP_TP_STATE_DESTROY) {
        SIP_CORE_DBG("destroying pjsip_transport@%p {SipTransport@%p}", tp, sipTransport.get());
    }

    // Propagate the event to the appropriate transport
    // Note the SipTransport may not be in our mappings!!! (if marked as dead)
    if (sipTransport)
        sipTransport->stateCallback(state, info);
}

void
SipTransportBroker::shutdown()
{
    std::unique_lock<std::mutex> const lock(transportMapMutex_);
    isDestroying_ = true;
    brokerDestroying = true;

    // stop all transports that still exist
    //    if (auto udp = udpTransport_.lock()) {
    //        pjsip_transport_shutdown(udp->get());
    //    }
    //
    //    if (auto tcp = tcpTransport_.lock()) {
    //        tcp->get_factory()->destroy(tcp->get_factory());
    //    }
}

std::shared_ptr<UDPTransport>
SipTransportBroker::getUdpTransport(const IpAddr& ipAddress)
{
    // cannot request transports when destroying
    if (isDestroying_) {
        return nullptr;
    }

    std::lock_guard<std::mutex> const lock(transportMapMutex_);

    if (auto spt = udpTransport_.lock()) {
        SIP_CORE_DBG("Reusing udp transport for %s", ipAddress.toString(true).c_str());
        return std::static_pointer_cast<UDPTransport>(spt);
    }

    // if we are here, we need to create new transport, because old is destroyed
    // or was not even created
    auto ret = createUdpTransport(ipAddress);
    if (ret) {
        udpTransport_ = ret;
    }
    return ret;
}

std::shared_ptr<UDPTransport>
SipTransportBroker::createUdpTransport(const IpAddr& ipAddress)
{
    RETURN_IF_FAIL(ipAddress, nullptr, "Could not determine IP address for this transport");

    pjsip_udp_transport_cfg pj_cfg;
    pjsip_udp_transport_cfg_default(&pj_cfg, ipAddress.getFamily());
    pj_cfg.bind_addr = ipAddress;
    pjsip_transport* transport = nullptr;
    if (pj_status_t const status = pjsip_udp_transport_start2(endpt_, &pj_cfg, &transport)) {
        SIP_CORE_ERR("pjsip_udp_transport_start2 failed with error %d: %s",
                     status,
                     sip_utils::sip_strerror(status).c_str());
        SIP_CORE_ERR("UDP IPv%s Transport did not start on %s",
                     ipAddress.isIpv4() ? "4" : "6",
                     ipAddress.toString(true).c_str());
        return nullptr;
    }

    SIP_CORE_DBG("Created UDP transport on address %s", ipAddress.toString(true).c_str());
    return std::make_shared<UDPTransport>(transport);
}

std::shared_ptr<TCPTransport>
SipTransportBroker::getTcpTransport(const IpAddr& ipAddress)
{
    // cannot request transports when destroying
    if (isDestroying_) {
        return nullptr;
    }

    std::lock_guard<std::mutex> const lock(transportMapMutex_);

    if (auto spt = tcpTransport_.lock()) {
        SIP_CORE_DBG("Reusing tcp transport for %s", ipAddress.toString(true).c_str());
        return std::static_pointer_cast<TCPTransport>(spt);
    }

    // if we are here, we need to create new transport, because old is destroyed
    // or was not even created
    auto ret = createTcpTransport(ipAddress);
    if (ret) {
        tcpTransport_ = ret;
    }
    return ret;
}

std::shared_ptr<TCPTransport>
SipTransportBroker::createTcpTransport(const IpAddr& ipAddress)
{
    RETURN_IF_FAIL(ipAddress, nullptr, "Could not determine IP address for this transport");

    pjsip_tcp_transport_cfg pj_cfg;
    pjsip_tcp_transport_cfg_default(&pj_cfg, ipAddress.getFamily());
    pj_cfg.bind_addr = ipAddress;
    pjsip_tpfactory* tcp;
    if (pj_status_t const status = pjsip_tcp_transport_start3(endpt_, &pj_cfg, &tcp)) {
        SIP_CORE_ERR("pjsip_tcp_transport_start3 failed with error %d: %s",
                     status,
                     sip_utils::sip_strerror(status).c_str());
        SIP_CORE_ERR("TCP IPv%s Transport did not start on %s",
                     ipAddress.isIpv4() ? "4" : "6",
                     ipAddress.toString(true).c_str());
        return nullptr;
    }

    SIP_CORE_DBG("Created TCP transport on address %s", ipAddress.toString(true).c_str());
    return std::make_shared<TCPTransport>(tcp);
}

} // namespace sip_core