#ifndef SIPEVENTS_H
#define SIPEVENTS_H

#include <string>
#include <list>
#include <mutex>
#include <vector>
#include <pj/pool.h>
#include "sip/sipaccount.h"

#include "noncopyable.h"
#include "custom_event_sub_client.h"

#define SIP_EVENTS_LOCK_FLAG        1
#define SIP_EVENTS_CLIENT_LOCK_FLAG 2

namespace sip_core {
class SIPAccount;
class CustomEventSubClient;

class SIPEvents
{
private:
    pj_bool_t enabled_;
    SIPAccount* acc_;                           /**<  Associated SIP account. */
    std::list<CustomEventSubClient*> sub_list_; /**< Subscribers list.*/

    std::recursive_mutex mutex_;
    pj_caching_pool cp_;
    pj_pool_t* pool_;

    std::vector<std::string> registered_modules_;

public:
    static pjsip_module test_mod;
    /**
     * Return associated sipaccount
     */
    SIPAccount* getAccount() const;
    /**
     * Return sipevents module ID which is actually the same as the VOIP link (???)
     */
    int getModId() const;
    /**
     *  Return a pool for generic functions.
     */
    pj_pool_t* getPool() const;
    /**
     * Activate the module.
     * @param enable Flag
     */
    void enable(bool enabled);
    /**
     * Send a SUBSCRIBE request to PBX
     * @param uri  Remote user that we want to subscribe
     */
    void subscribeClient(const std::string& uri, const std::string& event, bool flag);
    /**
     * Add a client to list.
     * @param b     CustomEventSubClient pointer
     */
    void addSubClient(CustomEventSubClient* c);
    /**
     * Remove a client from the list.
     * @param b     CustomEventSubClient pointer
     */
    void removeSubClient(CustomEventSubClient* c);
    /*
     * Register event package to event module
     */
    pj_status_t registerEventPkg(const std::string& event);

    bool isEnabled() { return enabled_; }
    const std::list<CustomEventSubClient*>& getEventSubscriptions() const { return sub_list_; }
    const std::vector<std::string>& registeredModules() const { return registered_modules_; }
    void lock();
    bool tryLock();
    void unlock();
    SIPEvents(SIPAccount* acc);
    ~SIPEvents();
};

} // namespace sip_core

#endif