//
// Created by Modus Operandi on 02.11.2024.
//

#ifndef TRANSPORT_H
#define TRANSPORT_H

#include <stdexcept>
#include <cstdint>
#include <string_utils.h>

#include <string>
#include <memory>
#include <string_view>
#include "configurationmanager_interface.h"

namespace sip_core {

using namespace libsip_core;

class Account;

inline TransportType
getTransportType(const std::string& name)
{
    auto str = std::lower(name);

    if (str == "tcp") {
        return TransportType::TCP;
    }
    if (str == "udp") {
        return TransportType::UDP;
    }
    if (str == "tls") {
        return TransportType::TLS;
    }

    throw std::runtime_error("unknown transport type: " + name);
}

static constexpr const char*
getTransportTypeName(TransportType type)
{
    // Function body
    switch (type) {
    case TransportType::TCP:
        return "tcp";
    case TransportType::UDP:
        return "udp";
    case TransportType::TLS:
        return "tls";
    }
}

class Transport
{
public:
    virtual ~Transport() = default;

    /* what transport type is this */
    virtual inline TransportType getTransportType() const = 0;

    /* is it secure? can be potentially inline */
    virtual bool isSecure() const = 0;

    /* corresponding device id */
    inline const std::string_view deviceId() const { return deviceId_; };

    /* corresponding account */
    inline const std::weak_ptr<Account>& getAccount() const { return account_; }

    /* is connected? */
    inline bool isConnected() const { return connected_; }

protected:
    std::weak_ptr<Account> account_ {};

    std::string deviceId_ {};

    bool connected_ {false};
};

} // namespace sip_core

#endif // TRANSPORT_H
