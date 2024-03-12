/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
 *  Author: Alexandre Savard <alexandre.savard@savoirfairelinux.com>
 *  Author: Philippe Gorley <philippe.gorley@savoirfairelinux.com>
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

#include "audio/ringbufferpool.h"
#include "fileutils.h"
#include "logger.h"
#include "manager.h"
#include "recordable.h"

#include <iomanip>

namespace sip_core {

Recordable::Recordable()
{
    recorder_.reset();
    recorder_ = std::make_shared<MediaRecorder>();
}

Recordable::~Recordable() {}

std::string
Recordable::getPath() const
{
    if (recorder_)
        return recorder_->getPath();
    else
        return "";
}

void Recordable::setAutoFilename() {
    std::time_t t = std::time(nullptr);
    auto startTime = *std::localtime(&t);
    std::stringstream generatedFileName;
    generatedFileName << std::put_time(&startTime, "%Y%m%d-%H%M%S");
    setRecordingFilename(generatedFileName.str());
}

bool
Recordable::toggleRecording()
{
    if (!recorder_) {
        SIP_CORE_ERR("couldn't toggle recording, non existent recorder");
        return false;
    }
    if (!recording_) {
        startRecording();
    } else {
        stopRecording();
    }
    return recording_;
}

void
Recordable::setRecordingFilename(const std::string &filename) {
    std::stringstream ss;
    auto dir = Manager::instance().audioPreference.getRecordPath();
    if (dir.empty())
        dir = fileutils::get_home_dir();
    // Check if dir exists, create if it does not
    fileutils::check_dir(dir.c_str());
    ss << dir;
    if (dir.back() != DIR_SEPARATOR_CH)
        ss << DIR_SEPARATOR_CH;
    ss << filename;
    auto fullPath = ss.str();
    recorder_->setPath(fullPath);
    currentRecordingFilename_ = filename;
}

bool
Recordable::startRecording(const std::string& filename)
{
    std::lock_guard<std::mutex> lk {apiMutex_};
    if (!recorder_) {
        SIP_CORE_ERR("couldn't start recording, non existent recorder");
        return false;
    }

    if (!recording_) {
        if (filename != currentRecordingFilename_) {
            setRecordingFilename(filename);
        }

        if (filename.empty()) {
            SIP_CORE_ERR("couldn't start recording, filename is empty");
            return false;
        }

        recorder_->audioOnly(isAudioOnly_);
        recorder_->startRecording();
        recording_ = recorder_->isRecording();
    }

    return recording_;
}

bool
Recordable::startRecording()
{
    if (currentRecordingFilename_.empty()) {
        setAutoFilename();
    }
    return startRecording(currentRecordingFilename_);
}

void
Recordable::stopRecording()
{
    std::lock_guard<std::mutex> lk {apiMutex_};
    if (!recorder_) {
        SIP_CORE_WARN("couldn't stop recording, non existent recorder");
        return;
    }

    if (not recording_) {
        SIP_CORE_WARN("couldn't stop non-running recording");
        return;
    }

    recorder_->stopRecording();
    currentRecordingFilename_ = "";
    recording_ = false;
}

bool
Recordable::isAudioOnly() const
{
    return isAudioOnly_;
}

} // namespace sip_core
