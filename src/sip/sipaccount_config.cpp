/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3, or (at your option)
 *  any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, see <https://www.gnu.org/licenses/>.
 */

#include "sipaccount_config.h"
#include "account_const.h"
#include "account_schema.h"
#include "config/yamlparser.h"

extern "C" {
#include <pjlib-util/md5.h>
}

namespace sip_core {

static KeepAliveType getKeepAliveType(const std::string& value) {
    if (value == "packet")
        return KeepAliveType::Packet;
    if (value == "sip-options")
        return KeepAliveType::Options;
    // Default to SIP OPTIONS when unspecified or unrecognized
    return KeepAliveType::Options;
}

static constexpr const char*
getKeepAliveTypeName(KeepAliveType type)
{
    return type == KeepAliveType::Options ? "sip-options" : "packet";
}

namespace Conf {
constexpr const char* ID_KEY = "id";
constexpr const char* REGISTRATION_EXPIRE = "registrationExpire";
constexpr const char* USERNAME_KEY = "username";
constexpr const char* BIND_ADDRESS_KEY = "bindAddress";
constexpr const char* INTERFACE_KEY = "interface";
constexpr const char* PORT_KEY = "port";
constexpr const char* PUBLISH_ADDR_KEY = "publishAddr";
constexpr const char* PUBLISH_PORT_KEY = "publishPort";
constexpr const char* SAME_AS_LOCAL_KEY = "sameasLocal";
constexpr const char* DTMF_TYPE_KEY = "dtmfType";
constexpr const char* SERVICE_ROUTE_KEY = "serviceRoute";
constexpr const char* BACK_SERVICE_ROUTE_KEY = "backServiceRoute";
constexpr const char* ALLOW_IP_AUTO_REWRITE = "allowIPAutoRewrite";
constexpr const char* PRESENCE_ENABLED_KEY = "presenceEnabled";
constexpr const char* PRESENCE_PUBLISH_SUPPORTED_KEY = "presencePublishSupported";
constexpr const char* PRESENCE_SUBSCRIBE_SUPPORTED_KEY = "presenceSubscribeSupported";
constexpr const char* PRESENCE_STATUS_KEY = "presenceStatus";
constexpr const char* PRESENCE_NOTE_KEY = "presenceNote";
constexpr const char* PRESENCE_MODULE_ENABLED_KEY = "presenceModuleEnabled";
constexpr const char* KEEP_ALIVE_INTERVAL = "keepAliveInterval";
constexpr const char* KEEP_ALIVE_TYPE = "keepAliveType";

constexpr const char* CRED_KEY = "credentials";
constexpr const char* CRED_PASSWORD = "password";
constexpr const char* CRED_REALM = "realm";
constexpr const char* CRED_USERNAME = "username";
constexpr const char* CRED_HASH = "hash";

constexpr const char* SRTP_KEY = "srtp";
constexpr const char* SRTP_ENABLE_KEY = "enable";
constexpr const char* KEY_EXCHANGE_KEY = "keyExchange";
constexpr const char* RTP_FALLBACK_KEY = "rtpFallback";

} // namespace Conf

static const SipAccountConfig DEFAULT_CONFIG {};
static constexpr unsigned MIN_REGISTRATION_TIME = 60; // seconds

using yaml_utils::parseValueOptional;
using yaml_utils::parseVectorMap;

void
SipAccountConfig::serialize(YAML::Emitter& out) const
{
    out << YAML::BeginMap;
    out << YAML::Key << Conf::ID_KEY << YAML::Value << id;
    SipAccountBaseConfig::serializeDiff(out, DEFAULT_CONFIG);

    out << YAML::Key << Conf::BIND_ADDRESS_KEY << YAML::Value << bindAddress;
    out << YAML::Key << Conf::PORT_KEY << YAML::Value << localPort;
    out << YAML::Key << Conf::PUBLISH_PORT_KEY << YAML::Value << publishedPort;

    // keep alive interval
    out << YAML::Key << Conf::KEEP_ALIVE_INTERVAL << YAML::Value << keepAliveInterval;
    out << YAML::Key << Conf::KEEP_ALIVE_TYPE << YAML::Value << getKeepAliveTypeName(keepAliveType);

    // out << YAML::Key << PRESENCE_MODULE_ENABLED_KEY << YAML::Value
    //     << (presence_ and presence_->isEnabled());

    out << YAML::Key << Conf::REGISTRATION_EXPIRE << YAML::Value << registrationExpire;
    out << YAML::Key << Conf::SERVICE_ROUTE_KEY << YAML::Value << serviceRoute;
    out << YAML::Key << Conf::BACK_SERVICE_ROUTE_KEY << YAML::Value << backServiceRoute;
    out << YAML::Key << Conf::ALLOW_IP_AUTO_REWRITE << YAML::Value << allowIPAutoRewrite;

    if (serializeCredentials) {
        out << YAML::Key << Conf::CRED_KEY << YAML::Value << getCredentials();
    }

    // srtp submap
    out << YAML::Key << Conf::SRTP_KEY << YAML::Value << YAML::BeginMap;
    out << YAML::Key << Conf::KEY_EXCHANGE_KEY << YAML::Value
        << sip_utils::getKeyExchangeName(srtpKeyExchange);
    out << YAML::Key << Conf::RTP_FALLBACK_KEY << YAML::Value << srtpFallback;
    out << YAML::EndMap;

    out << YAML::EndMap;
}

void
SipAccountConfig::unserialize(const YAML::Node& node)
{
    SipAccountBaseConfig::unserialize(node);
    parseValueOptional(node, Conf::USERNAME_KEY, username);
    parseValueOptional(node, Conf::BIND_ADDRESS_KEY, bindAddress);
    parseValueOptional(node, Conf::PORT_KEY, localPort);
    parseValueOptional(node, Conf::PUBLISH_PORT_KEY, publishedPort);

    parseValueOptional(node, Conf::KEEP_ALIVE_INTERVAL, keepAliveInterval);
    std::string tmpKeyKaType;
    parseValueOptional(node, Conf::KEEP_ALIVE_TYPE, tmpKeyKaType);
    keepAliveType = getKeepAliveType(tmpKeyKaType);

    parseValueOptional(node, Conf::REGISTRATION_EXPIRE, registrationExpire);
    registrationExpire = std::max(MIN_REGISTRATION_TIME, registrationExpire);
    parseValueOptional(node, Conf::SERVICE_ROUTE_KEY, serviceRoute);
    parseValueOptional(node, Conf::BACK_SERVICE_ROUTE_KEY, backServiceRoute);
    parseValueOptional(node, Conf::ALLOW_IP_AUTO_REWRITE, allowIPAutoRewrite);

    parseValueOptional(node, Conf::PRESENCE_MODULE_ENABLED_KEY, presenceEnabled);
    parseValueOptional(node, Conf::PRESENCE_PUBLISH_SUPPORTED_KEY, publishSupported);
    parseValueOptional(node, Conf::PRESENCE_SUBSCRIBE_SUPPORTED_KEY, subscribeSupported);

    // get srtp submap
    const auto& srtpMap = node[Conf::SRTP_KEY];
    std::string tmpKey;
    parseValueOptional(srtpMap, Conf::KEY_EXCHANGE_KEY, tmpKey);
    srtpKeyExchange = sip_utils::getKeyExchangeProtocol(tmpKey);
    parseValueOptional(srtpMap, Conf::RTP_FALLBACK_KEY, srtpFallback);

    const auto& credsMap = node[Conf::CRED_KEY];
    serializeCredentials = credsMap ? true : false;

    if (serializeCredentials) {
        auto creds = parseVectorMap(credsMap,
                                      {Conf::CRED_REALM,
                                       Conf::CRED_USERNAME,
                                       Conf::CRED_PASSWORD,
                                       Conf::CRED_HASH});
        setCredentials(creds);
    }
}

std::map<std::string, std::string>
SipAccountConfig::toMap() const
{
    auto a = SipAccountBaseConfig::toMap();
    a.emplace(Conf::CONFIG_ACCOUNT_USERNAME, username);
    a.emplace(Conf::CONFIG_LOCAL_PORT, std::to_string(localPort));
    a.emplace(Conf::CONFIG_ACCOUNT_DTMF_TYPE, dtmfType);
    a.emplace(Conf::CONFIG_LOCAL_INTERFACE, interface);
    a.emplace(Conf::CONFIG_PUBLISHED_PORT, std::to_string(publishedPort));
    a.emplace(Conf::CONFIG_PUBLISHED_SAMEAS_LOCAL, publishedSameasLocal ? TRUE_STR : FALSE_STR);
    a.emplace(Conf::CONFIG_PUBLISHED_ADDRESS, publishedIp);
    a.emplace(Conf::CONFIG_KEEP_ALIVE_INTERVAL, std::to_string(keepAliveInterval));
    a.emplace(Conf::CONFIG_KEEP_ALIVE_TYPE, getKeepAliveTypeName(keepAliveType));
    a.emplace(Conf::CONFIG_ACCOUNT_ROUTESET, serviceRoute);
    a.emplace(Conf::CONFIG_ACCOUNT_BACK_ROUTESET, backServiceRoute);
    a.emplace(Conf::CONFIG_ACCOUNT_REGISTRATION_EXPIRE, std::to_string(registrationExpire));

    std::string password {};
    std::string hash {};
    if (not credentials.empty()) {
        for (const auto& cred : credentials)
            if (cred.username == username) {
                password = cred.password;
                hash = cred.password_h;
                break;
            }
    }
    a.emplace(Conf::CONFIG_ACCOUNT_PASSWORD, std::move(password));
    a.emplace(Conf::CONFIG_ACCOUNT_HASH, std::move(hash));

    return a;
}

void
SipAccountConfig::fromMap(const std::map<std::string, std::string>& details)
{
    SipAccountBaseConfig::fromMap(details);

    // general sip settings
    parseString(details, Conf::CONFIG_ACCOUNT_USERNAME, username);
    parseInt(details, Conf::CONFIG_LOCAL_PORT, localPort);
    parseString(details, Conf::CONFIG_BIND_ADDRESS, bindAddress);
    parseString(details, Conf::CONFIG_ACCOUNT_ROUTESET, serviceRoute);
    parseString(details, Conf::CONFIG_ACCOUNT_BACK_ROUTESET, backServiceRoute);
    parseBool(details, Conf::CONFIG_ACCOUNT_IP_AUTO_REWRITE, allowIPAutoRewrite);
    parseString(details, Conf::CONFIG_LOCAL_INTERFACE, interface);
    parseBool(details, Conf::CONFIG_PUBLISHED_SAMEAS_LOCAL, publishedSameasLocal);
    parseString(details, Conf::CONFIG_PUBLISHED_ADDRESS, publishedIp);
    parseInt(details, Conf::CONFIG_PUBLISHED_PORT, publishedPort);
    parseBool(details, Conf::CONFIG_PRESENCE_ENABLED, presenceEnabled);
    parseString(details, Conf::CONFIG_ACCOUNT_DTMF_TYPE, dtmfType);
    parseInt(details, Conf::CONFIG_ACCOUNT_REGISTRATION_EXPIRE, registrationExpire);

    // srtp settings
    parseBool(details, Conf::CONFIG_SRTP_RTP_FALLBACK, srtpFallback);
    auto iterSrtp = details.find(Conf::CONFIG_SRTP_KEY_EXCHANGE);
    if (iterSrtp != details.end())
        srtpKeyExchange = sip_utils::getKeyExchangeProtocol(iterSrtp->second);

    // keepalive settings
    parseInt(details, Conf::CONFIG_KEEP_ALIVE_INTERVAL, keepAliveInterval);
    auto iterKaType = details.find(Conf::CONFIG_KEEP_ALIVE_TYPE);
    if (iterKaType != details.end())
        keepAliveType = getKeepAliveType(iterKaType->second);

    std::map<std::string, std::string> creds;
    creds[Conf::CRED_USERNAME] = username;
    parseString(details, Conf::CONFIG_ACCOUNT_PASSWORD, creds[Conf::CRED_PASSWORD]);
    parseString(details, Conf::CONFIG_ACCOUNT_HASH, creds[Conf::CRED_HASH]);
    creds[Conf::CRED_REALM] = "*";
    setCredentials({creds});

}

SipAccountConfig::Credentials::Credentials(const std::map<std::string, std::string>& cred)
{
    auto itrealm = cred.find(Conf::CRED_REALM);
    auto user = cred.find(Conf::CRED_USERNAME);
    auto passw = cred.find(Conf::CRED_PASSWORD);
    auto hash = cred.find(Conf::CRED_HASH);
    realm = itrealm != cred.end() ? itrealm->second : "";
    username = user != cred.end() ? user->second : "";
    password = passw != cred.end() ? passw->second : "";
    password_h = hash != cred.end() ? hash->second : "";
}

std::map<std::string, std::string>
SipAccountConfig::Credentials::toMap() const
{
    return {{Conf::CRED_REALM, realm},
            {Conf::CRED_USERNAME, username},
            {Conf::CRED_PASSWORD, password},
            {Conf::CRED_HASH, password_h}};
}

void
SipAccountConfig::Credentials::computePasswordHash()
{
    pj_md5_context pms;

    /* Compute md5 hash = MD5(username ":" realm ":" password) */
    pj_md5_init(&pms);
    pj_md5_update(&pms, (const uint8_t*) username.data(), username.length());
    pj_md5_update(&pms, (const uint8_t*) ":", 1);
    pj_md5_update(&pms, (const uint8_t*) realm.data(), realm.length());
    pj_md5_update(&pms, (const uint8_t*) ":", 1);
    pj_md5_update(&pms, (const uint8_t*) password.data(), password.length());

    unsigned char digest[16];
    pj_md5_final(&pms, digest);

    char hash[32];

    for (int i = 0; i < 16; ++i)
        pj_val_to_hex_digit(digest[i], &hash[2 * i]);

    password_h = {hash, 32};
}

std::vector<std::map<std::string, std::string>>
SipAccountConfig::getCredentials() const
{
    std::vector<std::map<std::string, std::string>> ret;
    ret.reserve(credentials.size());
    for (const auto& c : credentials) {
        ret.emplace_back(c.toMap());
    }
    return ret;
}

void
SipAccountConfig::setCredentials(const std::vector<std::map<std::string, std::string>>& creds)
{
    credentials.clear();
    credentials.reserve(creds.size());
    for (const auto& cred : creds)
        credentials.emplace_back(cred);
}

} // namespace sip_core
