/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
 *  Author: Emmanuel Milou <emmanuel.milou@savoirfairelinux.com>
 *  Author: Alexandre Bourget <alexandre.bourget@savoirfairelinux.com>
 *  Author: Yan Morin <yan.morin@savoirfairelinux.com>
 *  Author: Pierre-Luc Bacon <pierre-luc.bacon@savoirfairelinux.com>
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

#include <mutex>
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "sip/sipaccountbase.h"
#include "sip/siptransport.h"
#include "noncopyable.h"
#include "ring_types.h" // enable_if_base_of
#include "sipaccount_config.h"

#include <pjsip/sip_transport_tls.h>
#include <pjsip/sip_types.h>
#include <pjsip-ua/sip_regc.h>

#include <vector>
#include <map>
#include <cstdint>
#include <atomic>
#include <utility>
#include <deque>

namespace sip_core {

typedef std::vector<pj_ssl_cipher> CipherArray;

class SIPPresence;
class SIPCall;
class SIPEvents;

/**
 * @file sipaccount.h
 * @brief A SIP Account specify SIP specific functions and object = SIPCall/SIPVoIPLink)
 */
class SIPAccount : public SIPAccountBase
{
public:
    constexpr static auto ACCOUNT_TYPE = ACCOUNT_TYPE_SIP;

    std::shared_ptr<SIPAccount> shared()
    {
        return std::static_pointer_cast<SIPAccount>(shared_from_this());
    }
    std::shared_ptr<SIPAccount const> shared() const
    {
        return std::static_pointer_cast<SIPAccount const>(shared_from_this());
    }
    std::weak_ptr<SIPAccount> weak()
    {
        return std::static_pointer_cast<SIPAccount>(shared_from_this());
    }
    std::weak_ptr<SIPAccount const> weak() const
    {
        return std::static_pointer_cast<SIPAccount const>(shared_from_this());
    }

    /**
     * Constructor
     * @param accountID The account identifier
     */
    SIPAccount(const std::string& accountID, bool presenceEnabled);

    ~SIPAccount() noexcept;

    const SipAccountConfig& config() const
    {
        return *static_cast<const SipAccountConfig*>(&Account::config());
    }

    std::unique_ptr<AccountConfig> buildConfig() const override
    {
        return std::make_unique<SipAccountConfig>(getAccountID());
    }
    void setAccountDetails(const std::map<std::string, std::string>& details) override;
    inline void editConfig(std::function<void(SipAccountConfig& conf)>&& edit)
    {
        Account::editConfig(
            [&](AccountConfig& conf) { edit(*static_cast<SipAccountConfig*>(&conf)); });
    }

    std::string_view getAccountType() const override { return ACCOUNT_TYPE; }

    void setRegistrationStateDetailed(const std::pair<int, std::string>& details)
    {
        registrationStateDetailed_ = details;
    }

    void updateDialogViaSentBy(pjsip_dialog* dlg);

    void resetAutoRegistration();

    /**
     * Update NAT address, Via and Contact header from the REGISTER response
     * @param param pjsip reg cbparam
     * @param pool
     * @return update status
     */
    bool checkNATAddress(pjsip_regc_cbparam* param, pj_pool_t* pool);

    /**
     * Retrieve volatile details such as recent registration errors
     * @return std::map< std::string, std::string > The account volatile details
     */
    virtual std::map<std::string, std::string> getVolatileAccountDetails() const override;

    /**
     * Return the TLS settings, mainly used to return security information to
     * a client application
     */
    std::map<std::string, std::string> getTlsSettings() const;

    /**
     * Actually useless, since config loading is done in init()
     */
    void loadConfig() override;

    /**
     * Initialize the SIP voip link with the account parameters and send registration
     */
    void doRegister() override;

    /**
     * Send unregistration.
     */
    void doUnregister(std::function<void(bool)> cb = std::function<void(bool)>()) override;

    /**
     * Build and send SIP registration request
     */
    void sendRegister();

    /**
     * Build and send SIP unregistration request
     * @param destroy_transport If true, attempt to destroy the transport.
     */
    void sendUnregister();

    /**
     * Fire-and-forget unregistration for use during shutdown.
     * Sends UNREGISTER and immediately destroys regc (callback suppressed).
     */
    void doUnregisterFireAndForget();

    const pjsip_cred_info* getCredInfo() const { return cred_.data(); }

    /**
     * Get the number of credentials defined for
     * this account.
     * @param none
     * @return int The number of credentials set for this account.
     */
    unsigned getCredentialCount() const { return cred_.size(); }

    bool hasCredentials() const { return not cred_.empty(); }

    std::vector<std::map<std::string, std::string>> getCredentials() const
    {
        return config().getCredentials();
    }

    virtual void setRegistrationState(RegistrationState state,
                                      unsigned code = 0,
                                      const std::string& detail_str = {}) override;

    /**
     * A client sendings a REGISTER request MAY suggest an expiration
     * interval that indicates how long the client would like the
     * registration to be valid.
     *
     * @return the expiration value.
     */
    unsigned getRegistrationExpire() const
    {
        unsigned re = config().registrationExpire;
        return re ? re : PJSIP_REGC_EXPIRATION_NOT_SPECIFIED;
    }

    /**
     * Registration flag
     */
    bool isRegistered() const { return bRegister_; }

    /**
     * Get the registration structure that is used
     * for PJSIP in the registration process.
     * Settings are loaded from configuration file.
     * @return pjsip_regc* A pointer to the registration structure
     */
    pjsip_regc* getRegistrationInfo() { return regc_; }

    /**
     * Set the registration structure that is used
     * for PJSIP in the registration process;
     * @pram A pointer to the new registration structure
     * @return void
     */
    void setRegistrationInfo(pjsip_regc* regc)
    {
        if (regc_)
            destroyRegistrationInfo();
        regc_ = regc;
    }

    void destroyRegistrationInfo();

    /**
     * Get the port on which the transport/listener should use, or is
     * actually using.
     * @return pj_uint16 The port used for that account
     */
    uint16_t getLocalPort() const { return config().localPort; }

    void setLocalPort(uint16_t port)
    {
        editConfig([&](SipAccountConfig& config) { config.localPort = port; });
    }

    /**
     * @return pj_str_t "From" uri based on account information.
     * From RFC3261: "The To header field first and foremost specifies the desired
     * logical" recipient of the request, or the address-of-record of the
     * user or resource that is the target of this request. [...]  As such, it is
     * very important that the From URI not contain IP addresses or the FQDN
     * of the host on which the UA is running, since these are not logical
     * names."
     */
    std::string getFromUri() const override;

    /**
     * This method adds the correct scheme, hostname and append
     * the ;transport= parameter at the end of the uri, in accordance with RFC3261.
     * It is expected that "port" is present in the internal hostname_.
     *
     * @return pj_str_t "To" uri based on @param username
     * @param username A string formatted as : "username"
     */
    std::string getToUri(const std::string& username) const override;

    /**
     * In the current version, "srv" uri is obtained in the preformatted
     * way: hostname:port. This method adds the correct scheme and append
     * the ;transport= parameter at the end of the uri, in accordance with RFC3261.
     *
     * @return pj_str_t "server" uri based on @param hostPort
     * @param hostPort A string formatted as : "hostname:port"
     */
    std::string getServerUri() const;

    /**
     * Get the contact address
     * @return The current contact address
     */
    IpAddr getContactAddress() const;
    /**
     * Get the contact header
     * @return The current contact header
     */
    std::string getContactHeader() const;

    std::string getServiceRoute() const { return config().serviceRoute; }

    bool hasServiceRoute() const { return not config().serviceRoute.empty(); }

    std::string getBackServiceRoute() const { return config().backServiceRoute; }

    bool hasBackServiceRoute() const { return not config().backServiceRoute.empty(); }

    const IpAddr& getActualIpAddress() const;
    enum class KeepAliveTopology {
        NoRoute,
        ServiceRoute,
        ServiceRouteWithBackup,
    };

    static KeepAliveTopology resolveKeepAliveTopology(bool hasServiceRoute, bool hasBackServiceRoute);
    static bool shouldUseOptionsForKeepAlive(KeepAliveType keepAliveType, bool isUdpTransport);

    /**
     * Get the currently active service route (main or backup)
     */
    std::string getActiveServiceRoute() const;

    /**
     * Check if currently using backup service route
     */
    bool isUsingBackupRoute() const { return usingBackupRoute_; }

    /**
     * Switch to backup service route
     */
    void switchToBackupRoute();

    /**
     * Switch back to main service route
     */
    void switchToMainRoute();
    void switchRouteAndReregister(bool useBackup,
                                  const char* reason,
                                  bool skipMainRouteStartupProbe = false);

    bool isOptionsSuccess200(int statusCode) const;
    bool isTransportFailureFromOptions(int statusCode) const;
    bool isRouteFailureFromOptions(int statusCode) const;
    static uint32_t resolveMainRouteProbeIntervalSec(uint32_t keepAliveIntervalSec,
                                                     bool fastProbeEnabled);
    static uint32_t resolveActiveKeepAliveIntervalSec(uint32_t keepAliveIntervalSec,
                                                      bool fastProbeEnabled,
                                                      bool noRouteMode);
    static bool shouldEnableMainRouteFastProbeForStatusCode(int statusCode);
    static bool shouldDisableMainRouteFastProbeForStatusCode(int statusCode);
    static bool shouldEnableActiveNoRouteFastProbeForStatusCode(int statusCode);
    static bool shouldDisableActiveNoRouteFastProbeForStatusCode(int statusCode);
    static bool isTransientOptionsFailureCode(int statusCode);
    static bool isHardOptionsFailureCode(int statusCode);
    static bool shouldSuppressOptionsRecoveryAttempt(std::deque<int64_t>& attemptMs,
                                                     int64_t nowMs,
                                                     size_t maxAttempts,
                                                     int64_t windowMs);
    bool isMainRouteFastProbeEnabled() const;
    uint32_t getMainRouteProbeIntervalSec() const;
    bool isNoRouteKeepAliveMode() const;
    bool isActiveNoRouteFastProbeEnabled() const;
    uint32_t getActiveKeepAliveIntervalSec() const;
    void enableMainRouteFastProbe(const char* reason);
    void disableMainRouteFastProbe(const char* reason);
    void rescheduleMainRouteProbeNow(uint32_t seconds);
    void enableActiveNoRouteFastProbe(const char* reason, bool rescheduleNow = true);
    void disableActiveNoRouteFastProbe(const char* reason);
    void rescheduleActiveKeepAliveNow(uint32_t seconds);
    bool isTransientOptionsFailure(int statusCode) const;
    bool isHardOptionsFailure(int statusCode) const;
    void handleNoBackupOptionsRouteFailure(int statusCode);
    void handleUdpRawKeepAliveSendFailure(pj_status_t status);
    bool scheduleTransportRecovery(const char* reason, pj_status_t status);

    virtual bool getSrtpFallback() const override { return config().srtpFallback; }

    void setReceivedParameter(const std::string& received)
    {
        receivedParameter_ = received;
        via_addr_.host = sip_utils::CONST_PJ_STR(receivedParameter_);
    }

    const std::string& getReceivedParameter() const { return receivedParameter_; }

    pjsip_host_port* getViaAddr() { return &via_addr_; }

    int getRPort() const
    {
        if (rPort_ == -1)
            return config().localPort;
        else
            return rPort_;
    }

    void setRPort(int rPort)
    {
        rPort_ = rPort;
        via_addr_.port = rPort;
    }

    bool isRegistrationRefreshEnabled() const { return config().registrationRefreshEnabled; }

    bool setTransport(const std::shared_ptr<SipTransport>& = nullptr);

    bool switchTransport(libsip_core::TransportType transportType) override;

    /**
     * Try to register a new keepalive registration timer (only for UDP!) with current KA interval
     * from config!
     */
    void registerKeepAliveTimer();

    /**
     * Abort currently registered timer if any
     */
    void cancelKeepAliveTimer();

    /**
     * Starts a separate keep-alive timer to check main route availability
     * when using backup route.
     */
    void registerMainRouteKeepAliveTimer();
    /**
     * Starts a keep-alive timer to maintain NAT mapping towards backup route
     * while operating on the main route.
     */
    void registerBackupRouteKeepAliveTimer();

    /**
     * Cancels the main route keep-alive timer.
     */
    void cancelMainRouteKeepAliveTimer();
    /**
     * Cancels the backup route keep-alive timer.
     */
    void cancelBackupRouteKeepAliveTimer();

    /**
     * Check if we should switch back to main route when calls end.
     * Called when a call is detached from the account.
     */
    void checkSwitchToMainRouteOnCallEnd();

    /**
     * Override detach to check for route switching when calls end
     */
    bool detach(const std::shared_ptr<Call>& call)
    {
        bool result = Account::detach(call);
        if (result) {
            checkSwitchToMainRouteOnCallEnd();
        }
        return result;
    }

    // current transport
    virtual inline std::shared_ptr<SipTransport> getTransport() { return transport_; }

    // current transport type
    inline pjsip_transport_type_e getTransportType() const
    {
        if (transport_)
            return transport_->getPjSipTransportType();
        return config().transport == libsip_core::TransportType::TCP ? PJSIP_TRANSPORT_TCP
                                                                     : PJSIP_TRANSPORT_UDP;
    }

    /**
     * Shortcut for SipTransport::getTransportSelector(account.getTransport()).
     */
    pjsip_tpselector getTransportSelector();

    /* Returns true if the username and/or hostname match this account */
    MatchRank matches(std::string_view username, std::string_view hostname) const override;

    /**
     * Presence management
     */
    SIPPresence* getPresence() const;

    /**
     * SIP events management
     */
    SIPEvents* getSIPEvents() const;

    /**
     * Activate the module.
     * @param function Publish or subscribe to enable
     * @param enable Flag
     */
    void enablePresence(const bool& enable);
    /**
     * Activate the publish/subscribe.
     * @param enable Flag
     */
    void supportPresence(int function, bool enable);

    /**
     * Create outgoing SIPCall.
     * @param[in] toUrl the address to call
     * @param[in] mediaList list of medias
     * @return a shared pointer on the created call.
     */
    std::shared_ptr<Call> newOutgoingCall(
        std::string_view toUrl, const std::vector<libsip_core::MediaMap>& mediaList) override;

    /**
     * Create incoming SIPCall.
     * @param[in] from The origin of the call
     * @param mediaList A list of media
     * @param sipTr: SIP Transport
     * @return A shared pointer on the created call.
     */
    std::shared_ptr<SIPCall> newIncomingCall(
        const std::string& from,
        const std::vector<libsip_core::MediaMap>& mediaList,
        const std::shared_ptr<SipTransport>& sipTr = {}) override;

    void onRegister(pjsip_regc_cbparam* param);

    virtual void sendMessage(const std::string& to,
                             const std::map<std::string, std::string>& payloads,
                             uint64_t id,
                             bool retryOnTimeout = true,
                             bool onlyConnected = false) override;

    void connectivityChanged() override;
    bool hasRunningTransportForConnectivityChange() const;
    bool shouldHandleConnectivityChange() const;
    void handleConnectivityChangedForced(const char* reason);
    void prepareConnectivityRecovery(const char* reason);
    void dispatchPreparedConnectivityRecovery(const char* reason);
    bool isTransportRecoveryActive() const
    {
        return transportRecoveryPending_.load() || connectivityRecoveryInProgress_.load();
    }
    void reinviteActiveCalls();
    void scheduleConnectivityReinviteRetry(const std::shared_ptr<SIPCall>& sipCall);

    std::string getUserUri() const override;

    /**
     * Create the Ip address that the transport uses
     * @return IpAddr created
     */
    IpAddr createBindingAddress();

    void setActiveCodecs(const std::vector<unsigned>& list) override;
    bool isSrtpEnabled() const override
    {
        return config().srtpKeyExchange != KeyExchangeProtocol::NONE;
    }

    void setPushNotificationToken(const std::string& pushDeviceToken = "") override;

    /**
     * To be called by clients with relevant data when a push notification is received.
     */
    void pushNotificationReceived(const std::string& from,
                                  const std::map<std::string, std::string>& data);

    struct
    {
        pj_sockaddr socket;
        unsigned length {};
        pj_timer_entry timer {};
    } kaTarget;

    /**
     * Separate keep-alive for main route when using backup
     */
    struct
    {
        pj_sockaddr socket;
        unsigned length {};
        pj_timer_entry timer {};
    } kaMainRoute;
    /**
     * Separate keep-alive for backup route when using main route
     */
    struct
    {
        pj_sockaddr socket;
        unsigned length {};
        pj_timer_entry timer {};
    } kaBackupRoute;

    /**
     * Flag indicating an in-flight OPTIONS keep-alive transaction.
     * Used to prevent sending a new keep-alive while one is pending.
     */
    std::atomic<bool> ka_options_pending_ {false};

    /**
     * Flag indicating an in-flight OPTIONS keep-alive transaction for main route.
     */
    std::atomic<bool> ka_main_route_options_pending_ {false};
    /**
     * Flag indicating an in-flight OPTIONS keep-alive transaction for backup route.
     */
    std::atomic<bool> ka_backup_route_options_pending_ {false};

    /**
     * Flag indicating if main route is available (last keep-alive succeeded)
     * Public to allow access from static keep-alive callback
     */
    std::atomic<bool> mainRouteAvailable_ {false};

    void setCredentials(const std::vector<SipAccountConfig::Credentials>& creds);

    // set explicit transport destination and params for tdata
    bool setUpTransmissionData(pjsip_tx_data* tdata);
    bool setUpTransmissionData(pjsip_tx_data* tdata, const IpAddr& ip);

    const IpAddr& getServiceRouteIp() { return serviceRouteIp_; };
    const IpAddr& getBackServiceRouteIp() { return backServiceRouteIp_; };

    std::atomic<bool> needsResubscribe_ {false};
    std::atomic<bool> needsRepublish_ {false};
    std::atomic<bool> pendingTransportRebind_ {false};
    std::string callUri_ {};

    void startBackupKeepAliveAfterRegister();
    bool sendBackupRouteKeepAlive();

    std::atomic<bool> pendingBackupKeepAliveStart_ {false};

    std::mutex switchFromCallRetry;
    std::atomic<bool> routeSwitchPending_ {false};
    std::atomic<bool> transportSwitchPending_ {false};
    std::atomic<bool> transportRecoveryPending_ {false};
    std::atomic<bool> connectivityRecoveryRequested_ {false};
    std::atomic<bool> isShuttingDown_ {false};

    /**
     * When true, reinviteActiveCalls() will be called from onRegister()
     * after successful registration, instead of immediately from recoverTransport().
     */
    std::atomic<bool> pendingReinviteAfterRegister_ {false};

    /**
     * Flag indicating that connectivity recovery is in progress.
     * Transport-dependent operations should be skipped or deferred.
     */
    std::atomic<bool> connectivityRecoveryInProgress_ {false};
    std::atomic<bool> mainRouteFastProbeEnabled_ {false};
    std::atomic<bool> activeNoRouteFastProbeEnabled_ {false};
    std::atomic<bool> startupMainRouteProbePending_ {false};
    std::atomic<bool> skipMainRouteStartupProbeOnce_ {false};
    std::atomic<int64_t> lastTransportRecoveryMs_ {0};
    std::mutex optionsRecoveryMutex_;
    std::deque<int64_t> optionsRecoveryAttemptMs_;
    std::mutex rawKeepAliveRecoveryMutex_;
    std::deque<int64_t> rawKeepAliveRecoveryAttemptMs_;

private:
    void doRegister1_();
    void doRegister2_();

    // Initialize the address to be used in contact header. Might
    // be updated (as the contact header)after the registration.
    bool initContactAddress();
    void updateContactHeader();
public:
    void emitRegistrationStateSignal(RegistrationState state, unsigned details_code);
    void setRegistrationStateWithSignal(RegistrationState state,
                                        unsigned details_code,
                                        bool forceEmit);
    bool updateActiveKeepAliveTargetFromActualIpAddress();
    bool isOptionsKeepAliveMode() const;
    KeepAliveTopology getKeepAliveTopology() const;
    bool shouldRunStartupMainRouteProbe() const;
    std::string getServerUriForTarget(const std::string& target) const;
    std::string getActiveKeepAliveUri() const;
    std::string getMainRouteKeepAliveUri() const;
    std::string getBackupRouteKeepAliveUri() const;
    void handleActiveRouteOptionsSuccess(int statusCode);
    void handleActiveRouteOptionsFailure(int statusCode);
    void reregisterCurrentRoute(const char* reason);
    bool sendStartupMainRouteProbe();

private:

    NON_COPYABLE(SIPAccount);

    std::shared_ptr<Call> newRegisteredAccountCall(const std::string& id, const std::string& toUrl);

    /**
     * Start a SIP Call
     * @param call  The current call
     * @return true if all is correct
     */
    bool SIPStartCall(std::shared_ptr<SIPCall>& call);

    void usePublishedAddressPortInVIA();
    bool fullMatch(std::string_view username, std::string_view hostname) const;
    bool userMatch(std::string_view username) const;
    bool hostnameMatch(std::string_view hostname) const;
    bool proxyMatch(std::string_view hostname) const;

    /**
     * Callback called by the transport layer when the registration
     * transport state changes.
     */
    virtual void onTransportStateChanged(pjsip_transport_state state,
                                         const pjsip_transport_state_info* info);
    bool switchTransportInternal(libsip_core::TransportType transportType,
                                 bool persistConfig,
                                 bool markRebind,
                                 bool* changed = nullptr);
    void scheduleConnectivityRecovery(const char* reason);
    void recoverTransport(const std::string& reason, pj_status_t status);
    bool scheduleRecoveryInternal(const char* reason, pj_status_t status, bool debounced);
    bool shouldRecoverTransport(pjsip_transport_state state, pj_status_t status) const;
    bool isBenignTransportShutdown(pjsip_transport_state state, pj_status_t status) const;
    void markTransportRebindRequired(const char* reason);
    void prepareTransportReset(const char* reason, bool resetNetworkRuntimeState);
    void runPostRegisterRecoverySync();
    std::pair<std::string, pj_uint16_t> currentLocalBinding() const;
    void resetViaTransport();
    void resetNetworkRuntimeStateForConnectivityChange();
    void cancelAutoReregistrationTimer();
    void resetOptionsRecoveryWindow();
    bool shouldSuppressOptionsRecovery();
    void resetRawKeepAliveRecoveryWindow();
    bool shouldSuppressRawKeepAliveRecovery();

    struct
    {
        pj_bool_t active {false}; /**< Flag of reregister status. */
        pj_timer_entry timer {};  /**< Timer for reregistration.  */
        unsigned attempt_cnt {0}; /**< Attempt counter.     */
    } auto_rereg_ {};             /**< Reregister/reconnect data. */

    std::uniform_int_distribution<int> delay10ZeroDist_ {-10000, 10000};
    std::uniform_int_distribution<unsigned int> delay10PosDist_ {0, 10000};

    void scheduleReregistration();
    void autoReregTimerCb();

    /**
     * Current transport
     */
    std::shared_ptr<SipTransport> transport_ {};

    /**
     * If username is not provided, as it happens for Direct ip calls,
     * fetch the Real Name field of the user that is currently
     * running this program.
     * @return std::string The login name under which the software is running.
     */
    static std::string getLoginName();

    /**
     * Print contact header in certain format
     */
    std::string printContactHeader(const std::string& username,
                                   const std::string& displayName,
                                   const std::string& address,
                                   pj_uint16_t port,
                                   const std::string& deviceKey = {});

    /**
     * Resolved IP of hostname_ (for registration)
     */
    IpAddr hostIp_;

    /**
     * Resolved IP of serviceRoute_ (for registration)
     */
    IpAddr serviceRouteIp_;

    /**
     * Resolved IP of backServiceRoute_ (for registration)
     */
    IpAddr backServiceRouteIp_;

    /**
     * The pjsip client registration information
     */
    pjsip_regc* regc_ {nullptr};

    /**
     * To check if the account is registered
     */
    bool bRegister_;

    /**
     * Credential information stored for further registration.
     * Points to credentials_ members.
     */
    std::vector<pjsip_cred_info> cred_;

    /**
     * The TLS settings, used only if tls is chosen as a sip transport.
     */
    pjsip_tls_setting tlsSetting_;

    /**
     * Allocate a vector to be used by pjsip to store the supported ciphers on this system.
     */
    CipherArray ciphers_;

    /**
     * The STUN server name (hostname)
     */
    pj_str_t stunServerName_ {nullptr, 0};

    /**
     * The STUN server port
     */
    pj_uint16_t stunPort_ {};

    /**
     * Send Request Callback
     */
    static void onComplete(void* token, pjsip_event* event);

    /**
     * Details about the registration state.
     * This is a protocol Code:Description pair.
     */
    std::pair<int, std::string> registrationStateDetailed_;

    /**
     * Optional: "received" parameter from VIA header
     */
    std::string receivedParameter_;

    /**
     * Optional: "rport" parameter from VIA header
     */
    int rPort_ {-1};

    /**
     * Optional: via_addr construct from received parameters
     */
    pjsip_host_port via_addr_;

    // This is used at runtime . Mainly by SIPAccount::usePublishedAddressPortInVIA()
    std::string publishedIpStr_ {};

    mutable std::mutex contactMutex_;
    // Contact header
    std::string contactHeader_;
    // Contact address (the address part of a SIP URI)
    IpAddr contactAddress_ {};
    mutable std::mutex localBindingMutex_;
    std::string lastLocalBindingAddress_ {};
    pj_uint16_t lastLocalBindingPort_ {0};
    bool hasLocalBindingSnapshot_ {false};
    pjsip_transport* via_tp_ {nullptr};

    /**
     * Presence manager
     */
    SIPPresence* presence_;

    /**
     * SIP events manager
     */
    SIPEvents* sip_events_;

    /**
     * SIP port actually used,
     * this holds the actual port used for SIP, which may not be the port
     * selected in the configuration in the case that UPnP is used and the
     * configured port is already used by another client
     */
    pj_uint16_t publishedPortUsed_ {sip_utils::DEFAULT_SIP_PORT};

    /**
     * Flag indicating if backup service route is currently being used
     */
    bool usingBackupRoute_ {false};
};

} // namespace sip_core
