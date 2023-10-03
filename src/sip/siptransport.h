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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "connectivity/sip_utils.h"

#include "noncopyable.h"
#include "logger.h"

#include <pjsip.h>
#include <pjnath/stun_config.h>

#include <functional>
#include <mutex>
#include <condition_variable>
#include <map>
#include <string>
#include <vector>
#include <list>
#include <memory>

namespace sip_core {

class ChannelSocket;
class SIPAccountBase;
using onShutdownCb = std::function<void(void)>;

// TCP local listener
struct TcpListener
{
    TcpListener() {}
    TcpListener(pjsip_tpfactory* f)
        : listener(f)
    {}
    virtual ~TcpListener()
    {
        SIP_CORE_DBG("Destroying listener for TCP");
        listener->destroy(listener);
    }
    pjsip_tpfactory* get() { return listener; }

private:
    NON_COPYABLE(TcpListener);
    pjsip_tpfactory* listener {nullptr};
};

using SipTransportStateCallback
    = std::function<void(pjsip_transport_state, const pjsip_transport_state_info*)>;

/**
 * SIP transport wraps pjsip_transport.
 */
class SipTransport
{
public:
    SipTransport(pjsip_transport*);
    SipTransport(pjsip_transport*, const std::shared_ptr<TcpListener>&);
    // If the SipTransport is a channeled transport, we are already connected to the peer,
    // so, we can directly set tlsInfos_.peerCert and avoid any copy

    ~SipTransport();

    static const char* stateToStr(pjsip_transport_state state);

    void stateCallback(pjsip_transport_state state, const pjsip_transport_state_info* info);

    pjsip_transport* get() { return transport_.get(); }

    void addStateListener(uintptr_t lid, SipTransportStateCallback cb);
    bool removeStateListener(uintptr_t lid);

    bool isSecure() const { return PJSIP_TRANSPORT_IS_SECURE(transport_); }

    static bool isAlive(pjsip_transport_state state);

    /** Only makes sense for connection-oriented transports */
    bool isConnected() const noexcept { return connected_; }

    inline void setDeviceId(const std::string& deviceId) { deviceId_ = deviceId; }
    inline std::string_view deviceId() const { return deviceId_; }
    inline void setAccount(const std::shared_ptr<SIPAccountBase>& account) { account_ = account; }
    inline const std::weak_ptr<SIPAccountBase>& getAccount() const { return account_; }


private:
    NON_COPYABLE(SipTransport);

    // this will be called in DEstructor of SipTransport
    static void deleteTransport(pjsip_transport* t);

    std::unique_ptr<pjsip_transport, decltype(deleteTransport)&> transport_;
    std::shared_ptr<TcpListener> tcpListener_;
    std::mutex stateListenersMutex_;
    std::map<uintptr_t, SipTransportStateCallback> stateListeners_;
    std::weak_ptr<SIPAccountBase> account_ {};

    bool connected_ {false};
    std::string deviceId_ {};
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

    std::shared_ptr<SipTransport> getUdpTransport(const IpAddr&);

    std::shared_ptr<SipTransport> getTcpTransport(
        const std::shared_ptr<TcpListener>& l, const IpAddr& remote, const std::string& remote_name);
    
    std::shared_ptr<TcpListener>
        getTcpListener(const IpAddr& ipAddress);

    std::shared_ptr<SipTransport> addTransport(pjsip_transport*);

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
     * Create SIP UDP transport from account's setting
     * @param account The account for which a transport must be created.
     * @param IP protocol version to use, can be pj_AF_INET() or pj_AF_INET6()
     * @return a pointer to the new transport
     */
    std::shared_ptr<SipTransport> createUdpTransport(const IpAddr&);

    pjsip_tpfactory* createTcpTransport(const IpAddr&);

    /**
     * List of transports so we can bubble the events up. Transports are destroyed then there are no refs
     */
    std::map<pjsip_transport*, std::weak_ptr<SipTransport>> transports_ {};
    std::mutex transportMapMutex_ {};

    /**
     * Transports are stored in this map in order to retrieve them in case
     * several accounts would share the same port number.
     */
    std::map<IpAddr, pjsip_transport*> udpTransports_;

    std::map<IpAddr, pjsip_transport*> tcpTransports_;

    pjsip_endpoint* endpt_;
    std::atomic_bool isDestroying_ {false};
};

} // namespace sip_core
