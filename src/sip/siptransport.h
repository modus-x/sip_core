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

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "noncopyable.h"

#include <pjsip/sip_transport.h>
#include <pjsip/sip_types.h>

#include "configurationmanager_interface.h"
#include <transport.h>

namespace sip_core {

using namespace libsip_core;

class SIPAccountBase;

using SipTransportStateCallback
    = std::function<void(pjsip_transport_state, const pjsip_transport_state_info*)>;

/**
 * SIP transport wraps some pjsip transport.
 */
class SipTransport : public Transport
{
public:
    SipTransport() {}

    virtual ~SipTransport() {}

    static const char* stateToStr(pjsip_transport_state state);

    void stateCallback(pjsip_transport_state state, const pjsip_transport_state_info* info);

    /* what pjsip type transport is this */
    pjsip_transport_type_e getPjSipTransportType() const;

    void addStateListener(uintptr_t lid, const SipTransportStateCallback& cb);
    bool removeStateListener(uintptr_t lid);

    static bool isAlive(pjsip_transport_state state);

    int getAddressFamily() const { return pjsip_transport_type_get_af(getPjSipTransportType()); };

private:
    NON_COPYABLE(SipTransport);

    // delete / add listeners safely
    std::mutex stateListenersMutex_;
    std::map<uintptr_t, SipTransportStateCallback> stateListeners_;
};

class UDPTransport : public SipTransport
{
public:
    UDPTransport(pjsip_transport*);

    ~UDPTransport();

    inline pjsip_transport* get() const { return transport_.get(); }

    inline TransportType getTransportType() const override { return TransportType::UDP; };

    inline bool isSecure() const override { return false; }

private:
    NON_COPYABLE(UDPTransport);

    // this will be called in Destructor of UDPTransport
    static void deleteTransport(pjsip_transport* t);

    // here we store the transport
    // before storing transport, we need to add ref to it because we are using bare pjsip struct
    std::unique_ptr<pjsip_transport, decltype(&deleteTransport)> transport_;
};

class TCPTransport : public SipTransport
{
public:
    TCPTransport(pjsip_tpfactory*);

    ~TCPTransport();

    inline pjsip_tpfactory* get_factory() const { return connection_factory_; }

    inline TransportType getTransportType() const override { return TransportType::TCP; };

    inline bool isSecure() const override { return false; }

private:
    NON_COPYABLE(TCPTransport);

    // here we store the transport
    // before storing transport, we need to add ref to it because we are using bare pjsip struct
    // this is manager by transport manager of PJSIP!
    pjsip_tpfactory* connection_factory_;
};

class IpAddr;

/**
 * Manages the transports and receive callbacks from PJSIP
 */
class SipTransportBroker
{
public:
    SipTransportBroker(pjsip_endpoint* endpt);
    ~SipTransportBroker();

    std::shared_ptr<UDPTransport> getUdpTransport(const IpAddr&);

    std::shared_ptr<TCPTransport> getTcpTransport(const IpAddr&);

    /**
     * Start graceful shutdown procedure for all transports
     */
    void shutdown();

    void transportStateChanged(pjsip_transport*,
                               pjsip_transport_state,
                               const pjsip_transport_state_info*);

private:
    NON_COPYABLE(SipTransportBroker);

    /**
     * Create UDPTransport wrapper without any saving into any maps
     */
    std::shared_ptr<UDPTransport> createUdpTransport(const IpAddr&);

    /**
     * Create TCPTransport wrapper without any saving into any maps
     */
    std::shared_ptr<TCPTransport> createTcpTransport(const IpAddr&);

    /**
     * Mutex to protect concurrent map modification
     */
    std::mutex transportMapMutex_ {};

    /**
     * UDP transport currently used
     */
    std::weak_ptr<UDPTransport> udpTransport_;

    /**
     * TCP transport currently used
     */
    std::weak_ptr<TCPTransport> tcpTransport_;

    pjsip_endpoint* endpt_;

    std::atomic_bool isDestroying_ {false};
};

} // namespace sip_core
