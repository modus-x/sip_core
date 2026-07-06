/*
 *  Copyright (C) 2004-2024 Savoir-faire Linux Inc.
 *
 *  Native Windows (WASAPI) audio backend. Replaces the PortAudio backend with a
 *  direct Core Audio (WASAPI) implementation: shared-mode, event-driven capture
 *  and render, RDP-safe. Minimum target OS: Windows 8.
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
 */

#pragma once

#include "audio/audiolayer.h"
#include "noncopyable.h"

#include <memory>

namespace sip_core {

class WasapiLayer final : public AudioLayer
{
public:
    WasapiLayer(const AudioPreference& pref);
    ~WasapiLayer();

    std::vector<std::string> getCaptureDeviceList() const override;
    std::vector<std::string> getPlaybackDeviceList() const override;
    int getAudioDeviceIndex(const std::string& name, AudioDeviceType type) const override;
    std::string getAudioDeviceName(int index, AudioDeviceType type) const override;
    int getIndexCapture() const override;
    int getIndexPlayback() const override;
    int getIndexRingtone() const override;

    void startStream(AudioDeviceType stream = AudioDeviceType::ALL) override;
    void stopStream(AudioDeviceType stream = AudioDeviceType::ALL) override;

    void updatePreference(AudioPreference& pref, int index, AudioDeviceType type) override;

    bool isPreferredDeviceResolved(AudioDeviceType type) const override;

private:
    NON_COPYABLE(WasapiLayer);

    struct Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace sip_core
