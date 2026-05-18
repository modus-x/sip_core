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

#include "device_signature.h"

#include <algorithm>
#include <sstream>

namespace sip_core {
namespace coreaudio {

bool
shouldIgnoreDeviceName(const std::string& name)
{
    // Match as a prefix rather than a substring so a user-created aggregate
    // device that happens to embed the literal in its label (e.g.
    // "MyVPAUAggregateAudioDeviceClone") is still treated as a real device.
    // CoreAudio names VoiceProcessingIO's internal device with a fixed
    // "VPAUAggregateAudioDevice" prefix, sometimes followed by a numeric
    // suffix — never preceded by anything else.
    static constexpr char kVPAggregatePrefix[] = "VPAUAggregateAudioDevice";
    static constexpr std::size_t kVPAggregatePrefixLen = sizeof(kVPAggregatePrefix) - 1;
    return name.compare(0, kVPAggregatePrefixLen, kVPAggregatePrefix) == 0;
}

namespace {

void
appendSection(std::ostringstream& out, char tag, const std::vector<std::string>& names)
{
    std::vector<std::string> filtered;
    filtered.reserve(names.size());
    for (const auto& n : names) {
        if (!shouldIgnoreDeviceName(n))
            filtered.push_back(n);
    }
    std::sort(filtered.begin(), filtered.end());

    out << tag << ':';
    for (const auto& n : filtered)
        out << n << '|';
    out << ';';
}

} // namespace

std::string
makeDeviceSignature(const std::vector<std::string>& captureNames,
                    const std::vector<std::string>& playbackNames)
{
    std::ostringstream out;
    appendSection(out, 'c', captureNames);
    appendSection(out, 'p', playbackNames);
    return out.str();
}

} // namespace coreaudio
} // namespace sip_core
