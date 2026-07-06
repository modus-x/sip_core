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

// Unit tests for the pure WASAPI helpers (wasapi_convert.h): sample-format
// conversion between the shared-mode mix format and the int16 the ring buffer
// expects, and the device-list / "{{Default}}" formatting that must stay
// byte-for-byte identical to the previous PortAudio backend. No Windows deps,
// so this compiles and runs on any host.

#include "media/audio/wasapi/wasapi_convert.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace sip_core::wasapi;

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

// ---- Format conversion -----------------------------------------------------

void
test_float32_to_s16_endpoints()
{
    // Interleaved stereo: full-scale +, full-scale -, zero, half.
    float in[] = {1.0f, -1.0f, 0.0f, 0.5f};
    int16_t out[4] = {};
    convertDeviceToS16(in, out, /*frames*/ 2, /*channels*/ 2, WaveSampleType::Float32);
    report("float32->s16: +1.0 -> 32767", out[0] == 32767, std::to_string(out[0]));
    report("float32->s16: -1.0 -> -32767", out[1] == -32767, std::to_string(out[1]));
    report("float32->s16: 0.0 -> 0", out[2] == 0, std::to_string(out[2]));
    report("float32->s16: 0.5 -> ~16383", out[3] == 16383, std::to_string(out[3]));
}

void
test_float32_clamps_out_of_range()
{
    float in[] = {2.0f, -3.0f};
    int16_t out[2] = {};
    convertDeviceToS16(in, out, 2, 1, WaveSampleType::Float32);
    report("float32->s16: >1.0 clamps to 32767", out[0] == 32767, std::to_string(out[0]));
    report("float32->s16: <-1.0 clamps to -32767", out[1] == -32767, std::to_string(out[1]));
}

void
test_s16_to_float32_roundtrip()
{
    int16_t in[] = {32767, -32768, 0, 16384};
    float dev[4] = {};
    convertS16ToDevice(in, dev, 2, 2, WaveSampleType::Float32);
    int16_t back[4] = {};
    convertDeviceToS16(dev, back, 2, 2, WaveSampleType::Float32);
    // Round-trip is lossy by at most 1 LSB (the /32768 vs *32767 asymmetry).
    bool ok = true;
    for (int i = 0; i < 4; ++i)
        if (std::abs(int(in[i]) - int(back[i])) > 1)
            ok = false;
    report("s16<->float32 round-trip within 1 LSB", ok);
}

void
test_pcm16_is_passthrough()
{
    int16_t in[] = {123, -456, 789, -1000};
    int16_t out[4] = {};
    convertDeviceToS16(in, out, 2, 2, WaveSampleType::Pcm16);
    bool ok = true;
    for (int i = 0; i < 4; ++i)
        if (in[i] != out[i])
            ok = false;
    report("pcm16->s16 is an exact passthrough", ok);
}

void
test_pcm32_scales_by_16bits()
{
    int32_t in[] = {int32_t(1) * 65536, int32_t(-2) * 65536};
    int16_t out[2] = {};
    convertDeviceToS16(in, out, 2, 1, WaveSampleType::Pcm32);
    report("pcm32->s16: (1<<16)>>16 == 1", out[0] == 1, std::to_string(out[0]));
    report("pcm32->s16: (-2<<16)>>16 == -2", out[1] == -2, std::to_string(out[1]));
}

void
test_unsupported_yields_silence()
{
    int16_t in[] = {1, 2, 3, 4};
    int16_t out[4] = {7, 7, 7, 7};
    convertDeviceToS16(in, out, 2, 2, WaveSampleType::Unsupported);
    bool ok = out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0;
    report("unsupported format -> silence", ok);
}

// ---- Device-list / {{Default}} formatting ----------------------------------

void
test_buildDeviceList_prepends_default_alias()
{
    std::vector<std::string> names {"Speakers (Realtek)", "Headset Earphone"};
    auto list = buildDeviceList(names, "Speakers (Realtek)");
    report("buildDeviceList: size = names + 1", list.size() == 3, std::to_string(list.size()));
    report("buildDeviceList: index 0 is the {{Default}} alias",
           list[0] == "{{Default}} - Speakers (Realtek)", list.empty() ? "" : list[0]);
    report("buildDeviceList: index 1 is the first raw name",
           list[1] == "Speakers (Realtek)");
    report("buildDeviceList: index 2 is the second raw name",
           list[2] == "Headset Earphone");
}

void
test_buildDeviceList_empty_has_no_alias()
{
    auto list = buildDeviceList({}, "whatever");
    report("buildDeviceList: empty device set yields empty list (no alias)", list.empty(),
           std::to_string(list.size()));
}

void
test_index_and_name_roundtrip()
{
    auto list = buildDeviceList({"Mic A", "Mic B"}, "Mic A");
    // list = ["{{Default}} - Mic A", "Mic A", "Mic B"]
    report("indexOfDevice: exact match returns its position",
           indexOfDevice(list, "Mic B") == 2, std::to_string(indexOfDevice(list, "Mic B")));
    report("indexOfDevice: absent name returns -1",
           indexOfDevice(list, "Mic Z") == -1);

    // Persisted name for a UI index: index 0 (default) persists as "".
    report("nameForIndex: index 0 -> empty (\"use default\")", nameForIndex(list, 0).empty());
    report("nameForIndex: index 2 -> raw name", nameForIndex(list, 2) == "Mic B",
           nameForIndex(list, 2));
    report("nameForIndex: out-of-range -> empty", nameForIndex(list, 99).empty());
    report("nameForIndex: negative -> empty", nameForIndex(list, -1).empty());
}

void
test_resolvedIndex_matches_portaudio_semantics()
{
    auto list = buildDeviceList({"Mic A", "Mic B"}, "Mic A");
    report("resolvedIndex: stored name resolves to its index",
           resolvedIndex(list, "Mic B") == 2, std::to_string(resolvedIndex(list, "Mic B")));
    report("resolvedIndex: empty pref (\"default\") -> 0", resolvedIndex(list, "") == 0);
    report("resolvedIndex: stale/unknown pref -> 0 (falls back to default)",
           resolvedIndex(list, "Removed Device") == 0);
    report("resolvedIndex: empty device list -> 0", resolvedIndex({}, "anything") == 0);
}

} // namespace

int
main()
{
    test_float32_to_s16_endpoints();
    test_float32_clamps_out_of_range();
    test_s16_to_float32_roundtrip();
    test_pcm16_is_passthrough();
    test_pcm32_scales_by_16bits();
    test_unsupported_yields_silence();
    test_buildDeviceList_prepends_default_alias();
    test_buildDeviceList_empty_has_no_alias();
    test_index_and_name_roundtrip();
    test_resolvedIndex_matches_portaudio_semantics();

    if (g_failures > 0) {
        std::cerr << "\n" << g_failures << " test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "\nAll WASAPI convert tests passed.\n";
    return EXIT_SUCCESS;
}
