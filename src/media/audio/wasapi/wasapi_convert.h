/*
 *  Copyright (C) 2004-2024 Savoir-faire Linux Inc.
 *
 *  Pure, platform-independent helpers for the WASAPI backend: sample-format
 *  conversion between the WASAPI shared-mode mix format and the int16 the
 *  ring buffer expects, and the device-list / "{{Default}}" name formatting
 *  that must stay byte-for-byte identical to the previous PortAudio backend.
 *
 *  Kept free of any Windows headers so it compiles and unit-tests on any host.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace sip_core {
namespace wasapi {

// The default-device alias prefix. Consumers persist and display these strings,
// so it MUST match the previous PortAudio backend exactly.
inline constexpr const char* DEFAULT_ALIAS_PREFIX = "{{Default}} - ";

// Sample layout WASAPI hands us in shared mode. GetMixFormat almost always
// yields Float32; Pcm16/Pcm32 are defensive fallbacks.
enum class WaveSampleType { Float32, Pcm16, Pcm32, Unsupported };

// Convert an interleaved device buffer to interleaved int16 (AV_SAMPLE_FMT_S16).
inline void
convertDeviceToS16(const void* src, int16_t* dst, size_t frames, unsigned channels, WaveSampleType t)
{
    const size_t n = frames * channels;
    switch (t) {
    case WaveSampleType::Float32: {
        const float* s = static_cast<const float*>(src);
        for (size_t i = 0; i < n; ++i) {
            float v = std::max(-1.0f, std::min(1.0f, s[i]));
            dst[i] = static_cast<int16_t>(v * 32767.0f);
        }
        break;
    }
    case WaveSampleType::Pcm16:
        std::memcpy(dst, src, n * sizeof(int16_t));
        break;
    case WaveSampleType::Pcm32: {
        // Divide (not >>16): arithmetic right shift of a negative value is only
        // implementation-defined, and the equivalent left shift below is UB.
        const int32_t* s = static_cast<const int32_t*>(src);
        for (size_t i = 0; i < n; ++i)
            dst[i] = static_cast<int16_t>(s[i] / 65536);
        break;
    }
    case WaveSampleType::Unsupported:
        std::fill_n(dst, n, static_cast<int16_t>(0));
        break;
    }
}

// Convert interleaved int16 to the interleaved device format.
inline void
convertS16ToDevice(const int16_t* src, void* dst, size_t frames, unsigned channels, WaveSampleType t)
{
    const size_t n = frames * channels;
    switch (t) {
    case WaveSampleType::Float32: {
        float* d = static_cast<float*>(dst);
        for (size_t i = 0; i < n; ++i)
            d[i] = static_cast<float>(src[i]) / 32768.0f;
        break;
    }
    case WaveSampleType::Pcm16:
        std::memcpy(dst, src, n * sizeof(int16_t));
        break;
    case WaveSampleType::Pcm32: {
        // Multiply (not <<16): left-shifting a negative int32_t is UB in C++17.
        int32_t* d = static_cast<int32_t*>(dst);
        for (size_t i = 0; i < n; ++i)
            d[i] = static_cast<int32_t>(src[i]) * 65536;
        break;
    }
    case WaveSampleType::Unsupported:
        std::memset(dst, 0, n * sizeof(int16_t));
        break;
    }
}

// Build the consumer-facing device list: index 0 is "{{Default}} - <defaultName>"
// (only when there is at least one real device), followed by the raw names in
// enumeration order. Mirrors PortAudioLayerImpl::getDevicesByType.
inline std::vector<std::string>
buildDeviceList(const std::vector<std::string>& names, const std::string& defaultName)
{
    std::vector<std::string> out;
    out.reserve(names.size() + 1);
    if (!names.empty())
        out.push_back(std::string(DEFAULT_ALIAS_PREFIX) + defaultName);
    for (const auto& n : names)
        out.push_back(n);
    return out;
}

// Exact-match name -> index over a device list; -1 if absent (public-API semantics).
inline int
indexOfDevice(const std::vector<std::string>& devices, const std::string& name)
{
    for (size_t i = 0; i < devices.size(); ++i)
        if (devices[i] == name)
            return static_cast<int>(i);
    return -1;
}

// Resolve a stored preference name to a UI index; unresolved/empty -> 0 (default).
// Mirrors PortAudioLayerImpl::getIndexByType.
inline int
resolvedIndex(const std::vector<std::string>& devices, const std::string& pref)
{
    if (devices.empty())
        return 0;
    int idx = indexOfDevice(devices, pref);
    return idx >= 0 ? idx : 0;
}

// Resolve a UI index to the raw name to persist. Index 0 (default) persists as
// the empty string. Mirrors PortAudioLayerImpl::getDeviceNameByType.
inline std::string
nameForIndex(const std::vector<std::string>& devices, int index)
{
    if (index == 0)
        return {};
    if (index < 0 || static_cast<size_t>(index) >= devices.size())
        return {};
    return devices[static_cast<size_t>(index)];
}

} // namespace wasapi
} // namespace sip_core
