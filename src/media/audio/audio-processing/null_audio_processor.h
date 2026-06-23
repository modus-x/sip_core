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

#pragma once

#include "audio_processor.h"

namespace sip_core {

class NullAudioProcessor final : public AudioProcessor
{
public:
    NullAudioProcessor(AudioFormat format, unsigned frameSize);
    ~NullAudioProcessor() = default;

    std::shared_ptr<AudioFrame> getProcessed() override;

    void enableEchoCancel(bool) override {};

    void enableNoiseSuppression(bool) override {};

    void enableAutomaticGainControl(bool) override {};

    void enableVoiceActivityDetection(bool enabled) override { vadEnabled_ = enabled; };

private:
    // Energy/RMS voice-activity detection. webrtc-audio-processing is not built
    // on macOS/iOS (Darwin is excluded from the contrib recipe), so the WebRTC
    // VAD is unavailable there; this lightweight detector lets the conference
    // "speaking" indicator work for both the local mic and each received stream.
    bool computeRawVoice(const std::shared_ptr<AudioFrame>& frame) const;

    std::atomic_bool vadEnabled_ {false};
};

} // namespace sip_core
