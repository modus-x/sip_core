/*
 *  Copyright (C) 2004-2026 Savoir-faire Linux Inc.
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

// Unit tests for the out-of-dialog conference-control delivery helpers
// (isConferenceControlPayload + confInfoOutOfDialogEnabledFromEnv).
//
// These back the fix for the "in-dialog confInfo INFO 408 tears down a
// conference participant" bug: conference-control payloads are delivered
// OUT-OF-DIALOG so a delivery timeout can no longer trip RFC 3261 §12.2.1.2 and
// disconnect the call. The two helpers gate (which payloads reroute) and toggle
// (the SIP_CORE_CONFINFO_IN_DIALOG kill switch) that behavior.
//
// Pure table tests — no Manager / account fixtures (mirrors
// conference_protocol_tests.cpp). The full send/receive path can only be
// validated against a live PBX + a real participant.

#include "conference_protocol.h"

#include <iostream>
#include <map>
#include <string>

using sip_core::confInfoOutOfDialogEnabledFromEnv;
using sip_core::isConferenceControlPayload;

namespace {

int failures = 0;

void
check(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

using Payloads = std::map<std::string, std::string>;

} // namespace

int
main()
{
    // --- isConferenceControlPayload -------------------------------------------
    check(isConferenceControlPayload(Payloads {{"application/confInfo+json", "{}"}}),
          "confInfo+json is conference-control");
    check(isConferenceControlPayload(Payloads {{"application/confOrder+json", "{}"}}),
          "confOrder+json is conference-control");
    check(isConferenceControlPayload(Payloads {{"application/confVoiceActivity+json", "{}"}}),
          "confVoiceActivity+json is conference-control");

    check(!isConferenceControlPayload(Payloads {{"text/plain", "hello"}}),
          "plain chat is NOT conference-control");
    check(!isConferenceControlPayload(Payloads {}), "empty payload is NOT conference-control");
    check(!isConferenceControlPayload(Payloads {{"application/im-iscomposing+xml", "<x/>"}}),
          "is-composing is NOT conference-control");

    // A multipart body mixing chat with conference state must still reroute
    // (a confInfo part that timed out is what killed the call).
    check(isConferenceControlPayload(
              Payloads {{"text/plain", "hi"}, {"application/confInfo+json", "{}"}}),
          "mixed (chat + confInfo) is conference-control");

    // --- confInfoOutOfDialogEnabledFromEnv ------------------------------------
    // Default ON: only the exact string "1" reverts to legacy in-dialog INFO.
    check(confInfoOutOfDialogEnabledFromEnv(nullptr), "unset env -> out-of-dialog (default ON)");
    check(!confInfoOutOfDialogEnabledFromEnv("1"), "\"1\" -> legacy in-dialog (kill switch)");
    check(confInfoOutOfDialogEnabledFromEnv("0"), "\"0\" -> out-of-dialog");
    check(confInfoOutOfDialogEnabledFromEnv(""), "empty -> out-of-dialog");
    check(confInfoOutOfDialogEnabledFromEnv("true"), "non-\"1\" -> out-of-dialog");
    check(confInfoOutOfDialogEnabledFromEnv("11"), "\"11\" -> out-of-dialog (exact match only)");

    if (failures) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "All confinfo_out_of_dialog checks passed\n";
    return 0;
}
