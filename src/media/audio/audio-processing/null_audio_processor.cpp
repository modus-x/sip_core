/*
 *  Copyright (C) 2021-2022 Savoir-faire Linux Inc.
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

#include "null_audio_processor.h"

#include <cassert>
#include <cmath>
#include <cstdint>

namespace sip_core {

NullAudioProcessor::NullAudioProcessor(AudioFormat format, unsigned frameSize)
    : AudioProcessor(format, frameSize)
{
    SIP_CORE_DBG("[null_audio] NullAudioProcessor, frame size = %d (=%d ms), channels = %d",
                 frameSize,
                 frameDurationMs_,
                 format.nb_channels);
}

// ponytail: fixed RMS threshold (~-41 dBFS on int16 full-scale). Energy VAD is
// enough for an on/off "speaking" indicator and the base-class onset/hangover
// stabilizer hides threshold imprecision. Upgrade path: add a setVadSensitivity
// virtual mapping 0-3 -> threshold band if per-user tuning is ever needed.
static constexpr double kRmsVoiceThreshold = 300.0;

bool
NullAudioProcessor::computeRawVoice(const std::shared_ptr<AudioFrame>& frame) const
{
    // The WebRTC path assumes interleaved signed-16-bit PCM; bail gracefully
    // (rather than assert) on any other layout so release builds never crash.
    if (format_.sampleFormat != AV_SAMPLE_FMT_S16)
        return false;

    const auto* avf = frame->pointer();
    if (!avf || !avf->data[0] || avf->nb_samples <= 0)
        return false;

    const auto* samples = reinterpret_cast<const int16_t*>(avf->data[0]);
    const size_t n = static_cast<size_t>(avf->nb_samples) * format_.nb_channels;

    // int64 accumulator: 32767^2 * frame samples overflows int32.
    int64_t acc = 0;
    for (size_t i = 0; i < n; ++i)
        acc += static_cast<int64_t>(samples[i]) * samples[i];

    const double rms = std::sqrt(static_cast<double>(acc) / static_cast<double>(n));
    return rms > kRmsVoiceThreshold;
}

std::shared_ptr<AudioFrame>
NullAudioProcessor::getProcessed()
{
    if (tidyQueues()) {
        return {};
    }

    playbackQueue_.dequeue();
    auto record = recordQueue_.dequeue();
    if (!record)
        return {};

    if (!vadEnabled_) {
        // Clear any stale state so disabling VAD actually drops the indicator.
        record->has_voice = false;
        return record;
    }

    record->has_voice = getStabilizedVoiceActivity(computeRawVoice(record));
    return record;
};

} // namespace sip_core
