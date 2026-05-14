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
#pragma once
#include "sipaccountbase_config.h"

namespace sip_core {
constexpr static std::string_view ACCOUNT_TYPE_SIP = "SIP";

enum class KeepAliveType : unsigned {

    // simple UDP packet with ping!
    Packet,

    // full SIP OPTIONS message
    Options,
};

struct SipAccountConfig : public SipAccountBaseConfig {
    SipAccountConfig(const std::string& id = {}, const std::string& path = {}): SipAccountBaseConfig(std::string(ACCOUNT_TYPE_SIP), id, path) {}
    void serialize(YAML::Emitter& out) const override;
    void unserialize(const YAML::Node& node) override;
    std::map<std::string, std::string> toMap() const override;
    void fromMap(const std::map<std::string, std::string>&) override;

    /**
     * Local port to whih this account is bound
     */
    uint16_t localPort {sip_utils::DEFAULT_SIP_PORT};

    /**
     * Potential ip addresss on which this account is bound
     */
    std::string bindAddress {};

    /**
     * Published port, used only if defined by the user
     */
    uint16_t publishedPort {sip_utils::DEFAULT_SIP_PORT};


    /**
     * How often should be ka called?
     */
    uint32_t keepAliveInterval {25};

    /**
     * What should be send
     */
    KeepAliveType keepAliveType {KeepAliveType::Options};

    /**
     * interface name on which this account is bound
     */
    std::string interface;

    /**
     * Network settings
     */
    unsigned registrationExpire {600};
    bool registrationRefreshEnabled {true};

    // If true, the contact addreass and header will be rewritten
    // using the information received from the registrar.
    bool allowIPAutoRewrite {true};

    /**
     * Input Outbound Proxy Server Address
     */
    std::string serviceRoute;

    /**
     * Backup Outbound Proxy Server Address
     */
    std::string backServiceRoute;

    /**
     * Determine if the softphone should fallback on non secured media channel if SRTP negotiation
     * fails. Make sure other SIP endpoints share the same behavior since it could result in
     * encrypted data to be played through the audio device.
     */
    bool srtpFallback {false};
    /**
     * Specifies the type of key exchange used for SRTP, if any.
     * This only determine if the media channel is secured.
     */
    KeyExchangeProtocol srtpKeyExchange {KeyExchangeProtocol::SDES};

    bool presenceEnabled {false};
    bool publishSupported {false};
    bool subscribeSupported {false};

    /**
     * Convenience top-level password (parsed from YAML "password" on the account).
     * When non-empty and the account has no explicit credentials block, the
     * client may build a single Credentials entry from (username, password).
     * Kept out of serialize() on purpose — never written back to disk so we
     * don't accidentally round-trip plaintext into a generated config.
     */
    std::string password {};

#ifdef RQM
    /**
     * Optional directory path. When set (non-empty), the MediaEncoder's
     * fragmented-MP4 local mirror writes ftyp+moov+moof+mdat bytes to a
     * file at "<localDesktopRecords>/rqm-<epoch>.mp4" so the daemon's
     * host always has a playable recording independent of the RTP wire.
     *
     * When unset/empty, NO local mp4 mirror is written. This is the
     * default — RQM only writes a local copy when the operator opts in
     * via the account config.
     *
     * Parsed from YAML key "localDesktopRecords" on the account.
     */
    std::string localDesktopRecords {};
#endif

    /**
     * Map of credential for this account
     */
    struct Credentials
    {
        std::string realm {};
        std::string username {};
        std::string password {};
        std::string password_h {};
        Credentials(const std::string& r, const std::string& u, const std::string& p)
            : realm(r)
            , username(u)
            , password(p)
        {}
        Credentials(const std::map<std::string, std::string>& r);
        std::map<std::string, std::string> toMap() const;

        // TODO: broken function. I do not know how, but hash is computed in wrong way
        void computePasswordHash();
    };
    std::vector<Credentials> credentials;
    std::vector<std::map<std::string, std::string>> getCredentials() const;
    void setCredentials(const std::vector<std::map<std::string, std::string>>& creds);

    // should be serialize creds?
    bool serializeCredentials {false};
};

}
