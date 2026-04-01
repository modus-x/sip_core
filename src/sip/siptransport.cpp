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

namespace {

std::string
transportKey(const IpAddr& ipAddress)
{
    return ipAddress.toString(true);
}

bool
isUdpType(pjsip_transport_type_e type)
{
    return type == PJSIP_TRANSPORT_UDP || type == PJSIP_TRANSPORT_UDP6;
}

bool
isTcpType(pjsip_transport_type_e type)
{
    return type == PJSIP_TRANSPORT_TCP || type == PJSIP_TRANSPORT_TCP6;
}

} // namespace

void
UDPTransport::deleteTransport(pjsip_transport* t)
{
    if (!t)
        return;

    const auto status = pjsip_transport_dec_ref(t);
    if (status != PJ_SUCCESS) {
        SIP_CORE_WARN("Failed to decrement UDP transport ref: %s",
                      sip_utils::sip_strerror(status).c_str());
    }
}

UDPTransport::UDPTransport(pjsip_transport* t)
    : SipTransport()
    , transport_(nullptr, &deleteTransport)
{
    if (!t || pjsip_transport_add_ref(t) != PJ_SUCCESS)
        throw std::runtime_error("invalid transport for UDP, because cannot add ref");

    transport_.reset(t);
    SIP_CORE_DEBUG("UDPTransport@{} tr={}", fmt::ptr(this), fmt::ptr(transport_.get()));
}

UDPTransport::~UDPTransport()
{
    SIP_CORE_DEBUG("~UDPTransport@{} tr={}", fmt::ptr(this), fmt::ptr(transport_.get()));
}

void
UDPTransport::shutdown()
{
    if (auto* tp = transport_.get()) {
        auto status = pjsip_transport_shutdown(tp);
        if (status != PJ_SUCCESS) {
            SIP_CORE_WARN("Failed to shutdown UDP transport %p: %s",
                          tp,
                          sip_utils::sip_strerror(status).c_str());
        }
    }
}

TCPTransport::TCPTransport(pjsip_tpfactory* factory)
    : connection_factory_(factory)
{
    SIP_CORE_DEBUG("TCPTransport@{} tf={}", fmt::ptr(this), fmt::ptr(connection_factory_));
}

TCPTransport::~TCPTransport()
{
    shutdown();
    SIP_CORE_DEBUG("~TCPTransport@{} tf={}", fmt::ptr(this), fmt::ptr(connection_factory_));
}

void
TCPTransport::shutdown()
{
    auto* factory = connection_factory_;
    if (!factory)
        return;

    bool expected = false;
    if (!destroyed_.compare_exchange_strong(expected, true))
        return;

    connection_factory_ = nullptr;
    auto status = factory->destroy(factory);
    if (status != PJ_SUCCESS) {
        SIP_CORE_WARN("Failed to destroy TCP listener %p: %s",
                      factory,
                      sip_utils::sip_strerror(status).c_str());
    }
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
    if (!tp)
        return;

    std::shared_ptr<SipTransport> sipTransport;
    const auto pjsipType = pjsip_transport_get_type_from_flag(tp->flag);

    {
        std::lock_guard<std::mutex> const lock(transportMapMutex_);

        if (isUdpType(pjsipType)) {
            auto it = udpTransportIndex_.find(tp);
            if (it == udpTransportIndex_.end())
                return;

            auto mapIt = udpTransports_.find(it->second);
            if (mapIt != udpTransports_.end())
                sipTransport = mapIt->second;

            if (!SipTransport::isAlive(state)) {
                udpTransports_.erase(it->second);
                udpTransportIndex_.erase(it);
            } else if (!sipTransport) {
                udpTransportIndex_.erase(it);
            }
        } else if (isTcpType(pjsipType)) {
            auto* factory = tp->factory;
            if (!factory)
                return;

            auto it = tcpTransportIndex_.find(factory);
            if (it == tcpTransportIndex_.end())
                return;

            auto mapIt = tcpTransports_.find(it->second);
            if (mapIt != tcpTransports_.end())
                sipTransport = mapIt->second;

            if (!SipTransport::isAlive(state)) {
                tcpTransports_.erase(it->second);
                tcpTransportIndex_.erase(it);
            } else if (!sipTransport) {
                tcpTransportIndex_.erase(it);
            }
        } else {
            return;
        }
    }

    if (!sipTransport)
        return;

    auto ipAddress = IpAddr(tp->local_addr);
    SIP_CORE_DBG() << "transportStateChanged for " << ipAddress << " ptp " << tp << " info "
                   << tp->info << " state: " << SipTransport::stateToStr(state);
    sipTransport->stateCallback(state, info);
}

void
SipTransportBroker::shutdown()
{
    std::vector<std::shared_ptr<UDPTransport>> udpTransports;
    std::vector<std::shared_ptr<TCPTransport>> tcpTransports;

    {
        std::lock_guard<std::mutex> const lock(transportMapMutex_);
        if (isDestroying_.exchange(true))
            return;

        for (const auto& [_, transport] : udpTransports_) {
            if (transport)
                udpTransports.emplace_back(transport);
        }
        for (const auto& [_, transport] : tcpTransports_) {
            if (transport)
                tcpTransports.emplace_back(transport);
        }

        udpTransports_.clear();
        udpTransportIndex_.clear();
        tcpTransports_.clear();
        tcpTransportIndex_.clear();
    }

    for (const auto& transport : udpTransports)
        transport->shutdown();

    for (const auto& transport : tcpTransports)
        transport->shutdown();
}

void
SipTransportBroker::resetForConnectivityChange()
{
    std::vector<std::shared_ptr<UDPTransport>> udpTransports;
    std::vector<std::shared_ptr<TCPTransport>> tcpTransports;

    {
        std::lock_guard<std::mutex> const lock(transportMapMutex_);
        if (isDestroying_)
            return;

        for (const auto& [_, transport] : udpTransports_) {
            if (transport)
                udpTransports.emplace_back(transport);
        }
        for (const auto& [_, transport] : tcpTransports_) {
            if (transport)
                tcpTransports.emplace_back(transport);
        }

        udpTransports_.clear();
        udpTransportIndex_.clear();
        tcpTransports_.clear();
        tcpTransportIndex_.clear();
    }

    SIP_CORE_WARN("Connectivity change: resetting SIP transports (udp=%zu, tcp=%zu)",
                  udpTransports.size(),
                  tcpTransports.size());

    for (const auto& transport : udpTransports)
        transport->shutdown();

    for (const auto& transport : tcpTransports)
        transport->shutdown();

    SIP_CORE_DBG("Connectivity change: SIP transport cache reset complete");
}

std::shared_ptr<UDPTransport>
SipTransportBroker::getUdpTransport(const IpAddr& ipAddress)
{
    // cannot request transports when destroying
    if (isDestroying_) {
        return nullptr;
    }

    const auto key = transportKey(ipAddress);

    // Look up existing transport under the lock.
    {
        std::lock_guard<std::mutex> const lock(transportMapMutex_);
        if (isDestroying_)
            return nullptr;

        auto it = udpTransports_.find(key);
        if (it != udpTransports_.end() && it->second) {
            SIP_CORE_DBG("Reusing udp transport for %s", ipAddress.toString(true).c_str());
            return it->second;
        }
    }

    // Create outside the lock to avoid lock-ordering deadlock with PJSIP
    // transport-manager mutex (acquired by pjsip_transport_shutdown callbacks).
    auto ret = createUdpTransport(ipAddress);
    if (!ret || !ret->get())
        return nullptr;

    // Re-acquire lock and insert, checking for a concurrent insertion.
    {
        std::lock_guard<std::mutex> const lock(transportMapMutex_);
        if (isDestroying_)
            return nullptr;

        auto it = udpTransports_.find(key);
        if (it != udpTransports_.end() && it->second) {
            // Another thread already created one — use theirs.
            return it->second;
        }

        udpTransports_[key] = ret;
        udpTransportIndex_[ret->get()] = key;
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

    const auto key = transportKey(ipAddress);

    // Look up existing transport under the lock.
    {
        std::lock_guard<std::mutex> const lock(transportMapMutex_);
        if (isDestroying_)
            return nullptr;

        auto it = tcpTransports_.find(key);
        if (it != tcpTransports_.end() && it->second) {
            if (it->second->get_factory()) {
                SIP_CORE_DBG("Reusing tcp transport for %s", ipAddress.toString(true).c_str());
                return it->second;
            } else {
                // Clean stale key and stale reverse index entries.
                for (auto idxIt = tcpTransportIndex_.begin(); idxIt != tcpTransportIndex_.end();) {
                    if (idxIt->second == key) {
                        idxIt = tcpTransportIndex_.erase(idxIt);
                    } else {
                        ++idxIt;
                    }
                }
                tcpTransports_.erase(it);
            }
        }
    }

    // Create outside the lock to avoid lock-ordering deadlock with PJSIP
    // transport-manager mutex (acquired by pjsip_transport_shutdown callbacks).
    auto ret = createTcpTransport(ipAddress);
    if (!ret || !ret->get_factory())
        return nullptr;

    // Re-acquire lock and insert, checking for a concurrent insertion.
    {
        std::lock_guard<std::mutex> const lock(transportMapMutex_);
        if (isDestroying_)
            return nullptr;

        auto it = tcpTransports_.find(key);
        if (it != tcpTransports_.end() && it->second && it->second->get_factory()) {
            // Another thread already created one — use theirs.
            return it->second;
        }

        tcpTransports_[key] = ret;
        tcpTransportIndex_[ret->get_factory()] = key;
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
