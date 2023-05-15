#ifndef CUSTOM_EVENT_SUB_CLIENT_H
#define CUSTOM_EVENT_SUB_CLIENT_H

#include <pjsip-simple/evsub.h>
#include <pjsip-simple/evsub_msg.h>
#include <pjsip/sip_endpoint.h>
#include <pjsip/sip_transport.h>
#include <pj/timer.h>

#include <string>
#include "sipevents.h"

namespace sip_core {

class SIPEvents;

// subscribe to some user defined events
class CustomEventSubClient
{
private:
    static int modId_;

    NON_COPYABLE(CustomEventSubClient);
    SIPEvents* manager_;   /**< Associated SIPEvents pointer */
    pj_str_t uri_;         /**< pres_client URI. */
    pj_str_t contact_;     /**< Contact learned from subscrp. */
    pj_str_t display_;     /**< pres_client display name. */
    pjsip_dialog* dlg_;    /**< The underlying dialog. */
    pj_bool_t monitored_;  /**< Should we monitor? */
    pj_str_t name_;        /**< pres_client name. */
    pj_caching_pool cp_;
    pj_pool_t* pool_;      /**< Pool for this pres_client. */
    pjsip_evsub* sub_;     /**< pres_client presence subscription */
    unsigned term_code_;   /**< Subscription termination code */
    pj_str_t term_reason_; /**< Subscription termination reason */
    pj_timer_entry timer_; /**< Resubscription timer */
    void* user_data_;      /**< Application data. */
    int lock_count_;
    int lock_flag_;
    pj_str_t event_;
    std::string last_message_;
    /**
     * Transaction functions of event subscription client side.
     */
    static void client_evsub_on_state(pjsip_evsub* sub, pjsip_event* event);
    static void client_evsub_on_tsx_state(pjsip_evsub* sub,
                                          pjsip_transaction* tsx,
                                          pjsip_event* event);
    static void client_evsub_on_rx_notify(pjsip_evsub* sub,
                                          pjsip_rx_data* rdata,
                                          int* p_st_code,
                                          pj_str_t** p_st_text,
                                          pjsip_hdr* res_hdr,
                                          pjsip_msg_body** p_body);
    static void client_timer_cb(pj_timer_heap_t* th, pj_timer_entry* entry);

    /**
     * Plan a retry or a renew a subscription.
     * @param reschedule    Allow for reschedule.
     * @param msec          Delay value in milliseconds.
     */
    void rescheduleTimer(bool reschedule, unsigned msec);
    /**
     * Process the un/subscribe request transmission.
     */
    pj_status_t updateSubscription();
    /*
     * Compare the reason of a transaction end with the given string.
     */
    bool isTermReason(const std::string&);
    /**
     * return the code after a transaction is terminated.
     */
    unsigned getTermCode();

public:
    CustomEventSubClient(const std::string& uri, const std::string& event, SIPEvents* manager);
    ~CustomEventSubClient();
    /**
     * Compare with another pres_client's uris.
     * @param b     Other pres_client pointer
     */
    bool match(CustomEventSubClient* b);
    /**
     * Enable the monitoring and report signal to the client.
     * The PBX server must approve and maintain the subrciption before the pres_client is added in
     * the pres_client list.
     * @param flag  State of subscription. True if active.
     */
    void enable(bool flag);
    /**
     * Get associated parent manager
     */
    SIPEvents* getManager();
    /**
     * Data lock function
     */
    bool lock();
    /**
     * Data unlock function
     */
    void unlock();
    /**
     * Send a SUBCRIBE to the PXB or directly to a pres_client in the IP2IP context.
     */
    bool subscribe();
    /**
     * Send a SUBCRIBE to the PXB or directly to a pres_client in the IP2IP context but
     * the 0s timeout make the dialog expire immediately.
     */
    bool unsubscribe();
    /**
     * Return  the monitor variable.
     */
    bool isSubscribed();
    /**
     * Return the pres_client URI
     */
    std::string_view getURI();

    /**
     * A message from the URIs
     */
    const std::string& getLastMessage() const;

    /**
     * TODO: explain this:
     */
    void incLock() { lock_count_++; }
    void decLock() { lock_count_--; }
};

} // namespace sip_core

#endif