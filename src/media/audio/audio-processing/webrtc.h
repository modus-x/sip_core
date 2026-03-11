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
#include "configurationmanager_interface.h";

namespace webrtc {
class AudioProcessing;
}

namespace sip_core {

class WebRTCAudioProcessor final : public AudioProcessor
{
public:
    WebRTCAudioProcessor(AudioFormat format, unsigned frameSize, bool eNS);
    ~WebRTCAudioProcessor() = default;

    // Inherited via AudioProcessor
    std::shared_ptr<AudioFrame> getProcessed() override;

    void enableEchoCancel(bool enabled) override;
    void enableNoiseSuppression(bool enabled) override;
    void enableAutomaticGainControl(bool enabled) override;
    void enableVoiceActivityDetection(bool enabled) override;
    void setWebRtcParams(const libsip_core::WebRtcParams& params);
    void setVadSensitivity(int sensitivity);

private:
    std::unique_ptr<webrtc::AudioProcessing> apm;

    using fChannelBuffer = std::vector<std::vector<float>>;
    fChannelBuffer fRecordBuffer_;
    fChannelBuffer fPlaybackBuffer_;
    AudioBuffer iRecordBuffer_;
    AudioBuffer iPlaybackBuffer_;
    int analogLevel_ {0};
    int targetLevelDbfs_ {0};
    bool limiter_ {false};
    int compressionGainDb_ {0};
    bool agc_ {false};
    int vadSensitivity_ {3};
};
} // namespace sip_core
