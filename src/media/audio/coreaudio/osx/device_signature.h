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

#ifndef CORE_AUDIO_DEVICE_SIGNATURE_H_
#define CORE_AUDIO_DEVICE_SIGNATURE_H_

#include <string>
#include <vector>

namespace sip_core {
namespace coreaudio {

/**
 * Returns true if the given CoreAudio device name belongs to an internal
 * helper device that should not be treated as a user-visible audio device.
 *
 * Currently matches VoiceProcessingIO's "VPAUAggregateAudioDevice", whose
 * creation/destruction during AudioUnit lifecycle would otherwise look like
 * a device hot-plug event and trigger a costly stream restart cascade.
 */
bool shouldIgnoreDeviceName(const std::string& name);

/**
 * Produces a deterministic, order-independent signature of the user-visible
 * audio device topology. Internal helper devices (see shouldIgnoreDeviceName)
 * are filtered out before the signature is computed.
 *
 * Two calls return the same string iff the same set of capture device names
 * and the same set of playback device names are present, regardless of the
 * order in which they were enumerated.
 */
std::string makeDeviceSignature(const std::vector<std::string>& captureNames,
                                const std::vector<std::string>& playbackNames);

} // namespace coreaudio
} // namespace sip_core

#endif // CORE_AUDIO_DEVICE_SIGNATURE_H_
