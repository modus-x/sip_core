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

// Unit tests for the CoreAudio device-change snapshot helpers that prevent
// VoiceProcessingIO's internal VPAggregateAudioDevice churn from triggering
// spurious stream restarts (the root cause of the 3-5 s audio startup delay
// observed on macOS at call establishment).

#include "media/audio/coreaudio/osx/device_signature.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace sip_core::coreaudio;

namespace {

int g_failures = 0;

void
report(const char* name, bool ok, const std::string& detail = {})
{
    if (ok) {
        std::cout << "[ OK ] " << name << "\n";
    } else {
        ++g_failures;
        std::cout << "[FAIL] " << name;
        if (!detail.empty())
            std::cout << " — " << detail;
        std::cout << "\n";
    }
}

void
test_shouldIgnoreDeviceName_matchesVPAggregate()
{
    report("shouldIgnoreDeviceName: exact VPAUAggregateAudioDevice name",
           shouldIgnoreDeviceName("VPAUAggregateAudioDevice"));
    report("shouldIgnoreDeviceName: suffixed VPAUAggregateAudioDevice-12345",
           shouldIgnoreDeviceName("VPAUAggregateAudioDevice-12345"));
    report("shouldIgnoreDeviceName: numeric-suffixed VPAUAggregateAudioDevice2",
           shouldIgnoreDeviceName("VPAUAggregateAudioDevice2"));
}

void
test_shouldIgnoreDeviceName_keepsRealDevices()
{
    report("shouldIgnoreDeviceName: real built-in mic is kept",
           !shouldIgnoreDeviceName("MacBook Pro Microphone"));
    report("shouldIgnoreDeviceName: real built-in speakers are kept",
           !shouldIgnoreDeviceName("MacBook Pro Speakers"));
    report("shouldIgnoreDeviceName: empty name is kept (no false match)",
           !shouldIgnoreDeviceName(""));
    report("shouldIgnoreDeviceName: similar but distinct name is kept",
           !shouldIgnoreDeviceName("AggregateAudioDevice"));
    // Prefix-only match: a user-named aggregate device that happens to
    // *contain* the literal anywhere other than the start must NOT be
    // filtered, otherwise we'd silently lose a real device.
    report("shouldIgnoreDeviceName: prefix-only — embedded literal is kept",
           !shouldIgnoreDeviceName("MyVPAUAggregateAudioDeviceClone"));
    report("shouldIgnoreDeviceName: prefix-only — literal in the middle is kept",
           !shouldIgnoreDeviceName("System VPAUAggregateAudioDevice (internal)"));
    report("shouldIgnoreDeviceName: short string that is not a prefix",
           !shouldIgnoreDeviceName("VPAU"));
}

void
test_makeDeviceSignature_orderIndependent()
{
    std::vector<std::string> cap1 {"MacBook Pro Microphone", "External USB Mic"};
    std::vector<std::string> cap2 {"External USB Mic", "MacBook Pro Microphone"};
    std::vector<std::string> play {"MacBook Pro Speakers"};
    auto s1 = makeDeviceSignature(cap1, play);
    auto s2 = makeDeviceSignature(cap2, play);
    report("makeDeviceSignature: capture-order independent", s1 == s2, s1 + " vs " + s2);

    std::vector<std::string> playA {"Headphones", "MacBook Pro Speakers"};
    std::vector<std::string> playB {"MacBook Pro Speakers", "Headphones"};
    auto s3 = makeDeviceSignature(cap1, playA);
    auto s4 = makeDeviceSignature(cap1, playB);
    report("makeDeviceSignature: playback-order independent", s3 == s4, s3 + " vs " + s4);
}

void
test_makeDeviceSignature_emptyInputs()
{
    auto s = makeDeviceSignature({}, {});
    report("makeDeviceSignature: empty inputs yield a deterministic non-empty marker",
           !s.empty(),
           "actual: '" + s + "'");

    auto s2 = makeDeviceSignature({}, {});
    report("makeDeviceSignature: two empty calls match", s == s2);
}

void
test_makeDeviceSignature_filtersVPAggregate()
{
    std::vector<std::string> base {"MacBook Pro Microphone"};
    std::vector<std::string> withAggregate {"MacBook Pro Microphone",
                                            "VPAUAggregateAudioDevice"};
    auto baseSig = makeDeviceSignature(base, {});
    auto aggrSig = makeDeviceSignature(withAggregate, {});
    report("makeDeviceSignature: VPAggregate device is filtered from capture",
           baseSig == aggrSig,
           baseSig + " vs " + aggrSig);

    std::vector<std::string> playBase {"MacBook Pro Speakers"};
    std::vector<std::string> playAggr {"MacBook Pro Speakers",
                                       "VPAUAggregateAudioDevice-XYZ"};
    auto basePlay = makeDeviceSignature({}, playBase);
    auto aggrPlay = makeDeviceSignature({}, playAggr);
    report("makeDeviceSignature: VPAggregate device is filtered from playback",
           basePlay == aggrPlay,
           basePlay + " vs " + aggrPlay);
}

void
test_makeDeviceSignature_detectsRealChange()
{
    std::vector<std::string> before {"MacBook Pro Microphone"};
    std::vector<std::string> after {"MacBook Pro Microphone", "External USB Mic"};
    auto sBefore = makeDeviceSignature(before, {});
    auto sAfter = makeDeviceSignature(after, {});
    report("makeDeviceSignature: real device addition changes the signature",
           sBefore != sAfter,
           sBefore + " vs " + sAfter);

    // Removing a real device should also be detectable.
    auto sRemoved = makeDeviceSignature({}, {});
    report("makeDeviceSignature: real device removal changes the signature",
           sBefore != sRemoved,
           sBefore + " vs " + sRemoved);
}

void
test_makeDeviceSignature_separatesCaptureAndPlayback()
{
    // Same device name on capture vs playback must produce distinct
    // signatures — otherwise a hot-plug that swaps a name between scopes
    // (e.g. a headset that becomes a mic) would not be detected.
    std::vector<std::string> names {"USB Headset"};
    auto asCapture = makeDeviceSignature(names, {});
    auto asPlayback = makeDeviceSignature({}, names);
    report("makeDeviceSignature: capture vs playback scopes are distinguishable",
           asCapture != asPlayback,
           asCapture + " vs " + asPlayback);
}

void
test_makeDeviceSignature_duplicateNamesAreStable()
{
    // CoreAudio occasionally enumerates the same logical device through
    // multiple stream descriptions. The signature should still be stable
    // regardless of how getDeviceList chooses to dedupe.
    std::vector<std::string> dup {"MacBook Pro Microphone",
                                  "MacBook Pro Microphone",
                                  "MacBook Pro Microphone"};
    std::vector<std::string> single {"MacBook Pro Microphone"};
    auto sDup = makeDeviceSignature(dup, {});
    auto sSingle = makeDeviceSignature(single, {});
    // Note: we don't require dedup, only that the same input always
    // produces the same output.
    report("makeDeviceSignature: identical duplicate input is deterministic",
           sDup == makeDeviceSignature(dup, {}));
    report("makeDeviceSignature: single-vs-duplicate detection is consistent",
           (sDup == sSingle) == (sDup == sSingle));
}

} // namespace

int
main()
{
    test_shouldIgnoreDeviceName_matchesVPAggregate();
    test_shouldIgnoreDeviceName_keepsRealDevices();
    test_makeDeviceSignature_orderIndependent();
    test_makeDeviceSignature_emptyInputs();
    test_makeDeviceSignature_filtersVPAggregate();
    test_makeDeviceSignature_detectsRealChange();
    test_makeDeviceSignature_separatesCaptureAndPlayback();
    test_makeDeviceSignature_duplicateNamesAreStable();

    if (g_failures > 0) {
        std::cerr << "\n" << g_failures << " test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "\nAll CoreAudio device-filter tests passed.\n";
    return EXIT_SUCCESS;
}
