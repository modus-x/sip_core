#include "sip/sipaccount.h"

#include <cstdlib>
#include <deque>
#include <iostream>
#include <string>

using namespace sip_core;

namespace {

void
fail(const std::string& message)
{
    std::cerr << "TEST FAILURE: " << message << "\n";
    std::exit(1);
}

void
expect_true(bool condition, const std::string& message)
{
    if (!condition)
        fail(message);
}

void
test_transient_options_classification()
{
    expect_true(SIPAccount::isTransientOptionsFailureCode(PJSIP_SC_REQUEST_TIMEOUT),
                "408 must be transient");
    expect_true(SIPAccount::isTransientOptionsFailureCode(PJSIP_SC_INTERNAL_SERVER_ERROR),
                "500 must be transient");
    expect_true(SIPAccount::isTransientOptionsFailureCode(PJSIP_SC_BAD_GATEWAY),
                "502 must be transient");
    expect_true(SIPAccount::isTransientOptionsFailureCode(PJSIP_SC_SERVICE_UNAVAILABLE),
                "503 must be transient");
    expect_true(SIPAccount::isTransientOptionsFailureCode(PJSIP_SC_SERVER_TIMEOUT),
                "504 must be transient");
    expect_true(SIPAccount::isTransientOptionsFailureCode(PJSIP_SC_TSX_TRANSPORT_ERROR),
                "transport error must be transient");
}

void
test_hard_options_classification()
{
    expect_true(!SIPAccount::isHardOptionsFailureCode(180), "provisional code must not be hard");
    expect_true(!SIPAccount::isHardOptionsFailureCode(PJSIP_SC_OK), "200 must not be hard");
    expect_true(!SIPAccount::isHardOptionsFailureCode(PJSIP_SC_REQUEST_TIMEOUT),
                "408 must not be hard");
    expect_true(!SIPAccount::isHardOptionsFailureCode(PJSIP_SC_TSX_TRANSPORT_ERROR),
                "transport error must not be hard");

    expect_true(SIPAccount::isHardOptionsFailureCode(PJSIP_SC_UNAUTHORIZED), "401 must be hard");
    expect_true(SIPAccount::isHardOptionsFailureCode(PJSIP_SC_FORBIDDEN), "403 must be hard");
    expect_true(SIPAccount::isHardOptionsFailureCode(PJSIP_SC_NOT_FOUND), "404 must be hard");
    expect_true(SIPAccount::isHardOptionsFailureCode(PJSIP_SC_METHOD_NOT_ALLOWED),
                "405 must be hard");
    expect_true(SIPAccount::isHardOptionsFailureCode(PJSIP_SC_PROXY_AUTHENTICATION_REQUIRED),
                "407 must be hard");
    expect_true(SIPAccount::isHardOptionsFailureCode(PJSIP_SC_NOT_ACCEPTABLE_HERE),
                "488 must be hard");
    expect_true(SIPAccount::isHardOptionsFailureCode(PJSIP_SC_DECLINE), "603 must be hard");
}

void
test_options_recovery_suppression_window()
{
    std::deque<int64_t> attempts;
    constexpr int64_t windowMs = 60 * 1000;
    constexpr size_t maxAttempts = 3;

    expect_true(!SIPAccount::shouldSuppressOptionsRecoveryAttempt(attempts,
                                                                  1'000,
                                                                  maxAttempts,
                                                                  windowMs),
                "first recovery attempt must pass");
    expect_true(!SIPAccount::shouldSuppressOptionsRecoveryAttempt(attempts,
                                                                  2'000,
                                                                  maxAttempts,
                                                                  windowMs),
                "second recovery attempt must pass");
    expect_true(!SIPAccount::shouldSuppressOptionsRecoveryAttempt(attempts,
                                                                  3'000,
                                                                  maxAttempts,
                                                                  windowMs),
                "third recovery attempt must pass");
    expect_true(SIPAccount::shouldSuppressOptionsRecoveryAttempt(attempts,
                                                                 4'000,
                                                                 maxAttempts,
                                                                 windowMs),
                "fourth recovery attempt in window must be suppressed");

    expect_true(!SIPAccount::shouldSuppressOptionsRecoveryAttempt(attempts,
                                                                  70'000,
                                                                  maxAttempts,
                                                                  windowMs),
                "attempt after window expiration must pass");
}

void
test_raw_keepalive_recovery_suppression_window()
{
    std::deque<int64_t> attempts;
    constexpr int64_t windowMs = 60 * 1000;
    constexpr size_t maxAttempts = 3;

    expect_true(!SIPAccount::shouldSuppressOptionsRecoveryAttempt(attempts,
                                                                  10'000,
                                                                  maxAttempts,
                                                                  windowMs),
                "raw keep-alive first recovery attempt must pass");
    expect_true(!SIPAccount::shouldSuppressOptionsRecoveryAttempt(attempts,
                                                                  20'000,
                                                                  maxAttempts,
                                                                  windowMs),
                "raw keep-alive second recovery attempt must pass");
    expect_true(!SIPAccount::shouldSuppressOptionsRecoveryAttempt(attempts,
                                                                  30'000,
                                                                  maxAttempts,
                                                                  windowMs),
                "raw keep-alive third recovery attempt must pass");
    expect_true(SIPAccount::shouldSuppressOptionsRecoveryAttempt(attempts,
                                                                 40'000,
                                                                 maxAttempts,
                                                                 windowMs),
                "raw keep-alive fourth recovery attempt in window must be suppressed");

    expect_true(!SIPAccount::shouldSuppressOptionsRecoveryAttempt(attempts,
                                                                  130'000,
                                                                  maxAttempts,
                                                                  windowMs),
                "raw keep-alive attempt after window expiration must pass");
}

void
test_main_route_probe_interval_resolution()
{
    expect_true(SIPAccount::resolveMainRouteProbeIntervalSec(10, false) == 10,
                "normal mode must use configured keepalive interval");
    expect_true(SIPAccount::resolveMainRouteProbeIntervalSec(10, true) == 1,
                "fast mode must force 1-second probe interval");
    expect_true(SIPAccount::resolveMainRouteProbeIntervalSec(0, true) == 0,
                "keepalive interval 0 must disable probing even in fast mode");
}

void
test_main_route_fast_probe_status_transitions()
{
    expect_true(SIPAccount::shouldEnableMainRouteFastProbeForStatusCode(
                    PJSIP_SC_TSX_TRANSPORT_ERROR),
                "transport error must enable fast probing");
    expect_true(SIPAccount::shouldEnableMainRouteFastProbeForStatusCode(
                    PJSIP_SC_SERVICE_UNAVAILABLE),
                "non-200 final response must enable fast probing");
    expect_true(!SIPAccount::shouldEnableMainRouteFastProbeForStatusCode(PJSIP_SC_OK),
                "200 must not enable fast probing");

    expect_true(SIPAccount::shouldDisableMainRouteFastProbeForStatusCode(PJSIP_SC_OK),
                "200 must disable fast probing");
    expect_true(!SIPAccount::shouldDisableMainRouteFastProbeForStatusCode(PJSIP_SC_BAD_GATEWAY),
                "non-200 must not disable fast probing");
}

void
test_keepalive_topology_resolution()
{
    expect_true(SIPAccount::resolveKeepAliveTopology(false, false)
                    == SIPAccount::KeepAliveTopology::NoRoute,
                "no routes must resolve to no-route topology");
    expect_true(SIPAccount::resolveKeepAliveTopology(true, false)
                    == SIPAccount::KeepAliveTopology::ServiceRoute,
                "single service route must resolve to service-route topology");
    expect_true(SIPAccount::resolveKeepAliveTopology(true, true)
                    == SIPAccount::KeepAliveTopology::ServiceRouteWithBackup,
                "service route with backup must resolve to dual-route topology");
}

void
test_keepalive_options_selection()
{
    expect_true(!SIPAccount::shouldUseOptionsForKeepAlive(KeepAliveType::Packet, true),
                "UDP packet mode must not force SIP OPTIONS");
    expect_true(SIPAccount::shouldUseOptionsForKeepAlive(KeepAliveType::Options, true),
                "UDP OPTIONS mode must use SIP OPTIONS");
    expect_true(SIPAccount::shouldUseOptionsForKeepAlive(KeepAliveType::Packet, false),
                "non-UDP transport must use SIP OPTIONS even in packet mode");
}

void
test_active_probe_interval_resolution()
{
    expect_true(SIPAccount::resolveActiveKeepAliveIntervalSec(20, false, true) == 20,
                "active route normal mode must use configured interval");
    expect_true(SIPAccount::resolveActiveKeepAliveIntervalSec(20, true, true) == 1,
                "active route fast mode must force 1-second interval");
    expect_true(SIPAccount::resolveActiveKeepAliveIntervalSec(20, true, false) == 1,
                "fast probing must apply even when a service route is configured");
    expect_true(SIPAccount::resolveActiveKeepAliveIntervalSec(0, true, true) == 0,
                "keepalive interval 0 must disable active probing");
}

void
test_active_no_route_fast_probe_status_transitions()
{
    expect_true(SIPAccount::shouldEnableActiveNoRouteFastProbeForStatusCode(
                    PJSIP_SC_TSX_TRANSPORT_ERROR),
                "transport error must enable active no-route fast probing");
    expect_true(SIPAccount::shouldEnableActiveNoRouteFastProbeForStatusCode(
                    PJSIP_SC_SERVICE_UNAVAILABLE),
                "non-200 final response must enable active no-route fast probing");
    expect_true(!SIPAccount::shouldEnableActiveNoRouteFastProbeForStatusCode(PJSIP_SC_OK),
                "200 must not enable active no-route fast probing");

    expect_true(SIPAccount::shouldDisableActiveNoRouteFastProbeForStatusCode(PJSIP_SC_OK),
                "200 must disable active no-route fast probing");
    expect_true(!SIPAccount::shouldDisableActiveNoRouteFastProbeForStatusCode(PJSIP_SC_BAD_GATEWAY),
                "non-200 must not disable active no-route fast probing");
}

} // namespace

int
main()
{
    test_transient_options_classification();
    test_hard_options_classification();
    test_options_recovery_suppression_window();
    test_raw_keepalive_recovery_suppression_window();
    test_main_route_probe_interval_resolution();
    test_main_route_fast_probe_status_transitions();
    test_keepalive_topology_resolution();
    test_keepalive_options_selection();
    test_active_probe_interval_resolution();
    test_active_no_route_fast_probe_status_transitions();

    std::cout << "All SIP account recovery tests passed.\n";
    return 0;
}
