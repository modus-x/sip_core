/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
 *  Author: Hugo Lefeuvre <hugo.lefeuvre@savoirfairelinux.com>
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

#include "audio_frame_resizer.h"
#include "audio_input.h"
#include "sip_core/media_const.h"
#include "fileutils.h" // access
#include "manager.h"
#include "media_decoder.h"
#include "resampler.h"
#include "ringbuffer.h"
#include "ringbufferpool.h"
#include "tracepoint.h"

#include <future>
#include <memory>

namespace sip_core {

static constexpr auto MS_PER_PACKET = std::chrono::milliseconds(20);
// After ~1 second (50 frames * 20ms) of no audio, consider the device broken
static constexpr unsigned int BROKEN_DEVICE_THRESHOLD = 50;

AudioInput::AudioInput(const std::string& id)
    : id_(id)
    , format_(Manager::instance().getRingBufferPool().getInternalAudioFormat())
    , frameSize_(format_.sample_rate * MS_PER_PACKET.count() / 1000)
    , resampler_(new Resampler)
    , resizer_(new AudioFrameResizer(format_,
                                     frameSize_,
                                     [this](std::shared_ptr<AudioFrame>&& f) {
                                         frameResized(std::move(f));
                                     }))
    , fileId_(id + "_file")
    , deviceGuard_()
    , loop_([] { return true; }, [this] { process(); }, [] {}, ThreadLoop::ThreadPriority::HIGH)
{
    SIP_CORE_DBG() << "Creating audio input with id: " << id;
    updateMuteStateForDeviceAvailability();
}

AudioInput::AudioInput(const std::string& id, const std::string& resource)
    : AudioInput(id)
{
    switchInput(resource);
}

AudioInput::~AudioInput()
{
    if (playingFile_) {
        Manager::instance().getRingBufferPool().unBindHalfDuplexOut(RingBufferPool::DEFAULT_ID, id_);
    }
    loop_.join();
}

void
AudioInput::process()
{
    readFromDevice();
}

void
AudioInput::updateStartTime(int64_t start)
{
    if (decoder_) {
        decoder_->updateStartTime(start);
    }
}

void
AudioInput::frameResized(std::shared_ptr<AudioFrame>&& ptr)
{
    std::shared_ptr<AudioFrame> frame = std::move(ptr);
    frame->pointer()->pts = sent_samples;
    sent_samples += frame->pointer()->nb_samples;

    notify(std::static_pointer_cast<MediaFrame>(frame));
}

void
AudioInput::setSeekTime(int64_t time)
{
    if (decoder_) {
        decoder_->setSeekTime(time);
    }
}

void
AudioInput::readFromDevice()
{
    {
        std::lock_guard<std::mutex> lk(resourceMutex_);
        if (decodingFile_)
            while (fileBuf_->isEmpty())
                readFromFile();
        if (playingFile_) {
            readFromQueue();
            return;
        }
    }

    // Note: read for device is called in an audio thread and we don't
    // want to have a loop which takes 100% of the CPU.
    // Here, we basically want to mix available data without any glitch
    // and even if one buffer doesn't have audio data (call in hold,
    // connections issues, etc). So mix every MS_PER_PACKET
    std::this_thread::sleep_until(wakeUp_);
    wakeUp_ += MS_PER_PACKET;

    auto& bufferPool = Manager::instance().getRingBufferPool();
    auto audioFrame = bufferPool.getData(id_);

    // Track consecutive empty frames to detect broken/non-functional audio device
    // If we get too many empty frames in a row, the device is likely broken
    // and we should send silence to keep the RTP stream alive
    if (not audioFrame) {
        consecutiveEmptyFrames_++;
        if (consecutiveEmptyFrames_ == BROKEN_DEVICE_THRESHOLD) {
            SIP_CORE_WARN("Audio Input: no data for %u consecutive frames, "
                         "device may be broken - sending silence to keep RTP alive",
                         BROKEN_DEVICE_THRESHOLD);
            // Re-check whether the capture device is still present so we
            // correctly enter forceMuteNoDevice_ state and can recover later.
            updateMuteStateForDeviceAvailability();
        }
    } else {
        // Reset counter when we get valid audio
        if (consecutiveEmptyFrames_ >= BROKEN_DEVICE_THRESHOLD) {
            SIP_CORE_INFO("Audio Input: device recovered, received audio data again");
            // Reset BEFORE re-checking so updateMuteStateForDeviceAvailability
            // correctly clears forceMuteNoDevice_.
            consecutiveEmptyFrames_ = 0;
            updateMuteStateForDeviceAvailability();
        }
        consecutiveEmptyFrames_ = 0;
    }

    // Send silence frames in these cases:
    // 1. User explicitly muted (muteState_ == true)
    // 2. No capture device available (forceMuteNoDevice_ == true)
    // 3. Device appears broken (many consecutive empty frames)
    // This ensures RTP packets are always sent to prevent server kicking us
    bool shouldSendSilence = muteState_ || forceMuteNoDevice_ || 
                             (not audioFrame && consecutiveEmptyFrames_ >= 50);

    if (not audioFrame) {
        if (!shouldSendSilence) {
            // Still waiting for device to provide data, don't send anything yet
            return;
        }
        // Create silence frame
        audioFrame = std::make_shared<AudioFrame>(bufferPool.getInternalAudioFormat(), frameSize_);
        libav_utils::fillWithSilence(audioFrame->pointer());
        audioFrame->has_voice = false;
    } else if (muteState_) {
        // User is muted but we got a frame - fill it with silence
        libav_utils::fillWithSilence(audioFrame->pointer());
        audioFrame->has_voice = false;
    }

    std::lock_guard<std::mutex> lk(fmtMutex_);
    if (bufferPool.getInternalAudioFormat() != format_)
        audioFrame = resampler_->resample(std::move(audioFrame), format_);
    resizer_->enqueue(std::move(audioFrame));

    if (recorderCallback_ && settingMS_.exchange(false)) {
        recorderCallback_(MediaStream("a:local", format_, sent_samples));
    }

    sip_core_tracepoint(audio_input_read_from_device_end, id_.c_str());
}

void
AudioInput::readFromQueue()
{
    if (!decoder_)
        return;
    if (paused_) {
        std::this_thread::sleep_for(MS_PER_PACKET);
        return;
    }
    decoder_->emitFrame(true);
}

void
AudioInput::readFromFile()
{
    if (!decoder_)
        return;
    const auto ret = decoder_->decode();
    switch (ret) {
    case DecodeStatus::Success:
        break;
    case DecodeStatus::EndOfFile:
        createDecoder();
        break;
    case DecodeStatus::ReadError:
        SIP_CORE_ERR() << "Failed to decode frame";
        break;
    case DecodeStatus::ReadBufferOverflow:
        SIP_CORE_ERR() << "Read buffer overflow detected";
        break;
    case DecodeStatus::FallBack:
        break;
    }
}

bool
AudioInput::initDevice(const std::string& device)
{
    updateMuteStateForDeviceAvailability();
    devOpts_ = {};
    devOpts_.input = device;
    devOpts_.channel = format_.nb_channels;
    devOpts_.framerate = format_.sample_rate;
    deviceGuard_ = Manager::instance().startAudioStream(AudioDeviceType::CAPTURE);
    playingDevice_ = true;
    return true;
}

void
AudioInput::configureFilePlayback(const std::string& path,
                                  std::shared_ptr<MediaDemuxer>& demuxer,
                                  int index)
{
    decoder_.reset();
    Manager::instance().getRingBufferPool().unBindHalfDuplexOut(RingBufferPool::DEFAULT_ID, id_);
    fileBuf_.reset();
    devOpts_ = {};
    devOpts_.input = path;
    devOpts_.name = path;
    auto decoder
        = std::make_unique<MediaDecoder>(demuxer, index, [this](std::shared_ptr<MediaFrame>&& frame) {
              if (muteState_) {
                  libav_utils::fillWithSilence(frame->pointer());
                  return;
              }
              fileBuf_->put(std::static_pointer_cast<AudioFrame>(frame));
          });
    decoder->emulateRate();

    fileBuf_ = Manager::instance().getRingBufferPool().createRingBuffer(id_);
    playingFile_ = true;
    decoder_ = std::move(decoder);
}

void
AudioInput::setPaused(bool paused)
{
    if (paused) {
        Manager::instance().getRingBufferPool().unBindHalfDuplexOut(RingBufferPool::DEFAULT_ID, id_);
        deviceGuard_.reset();
    } else {
        Manager::instance().getRingBufferPool().bindHalfDuplexOut(RingBufferPool::DEFAULT_ID, id_);
        deviceGuard_ = Manager::instance().startAudioStream(AudioDeviceType::PLAYBACK);
    }
    paused_ = paused;
}

void
AudioInput::flushBuffers()
{
    if (decoder_) {
        decoder_->flushBuffers();
    }
}

bool
AudioInput::initFile(const std::string& path)
{
    if (access(path.c_str(), R_OK) != 0) {
        SIP_CORE_ERR() << "File '" << path << "' not available";
        return false;
    }

    devOpts_ = {};
    devOpts_.input = path;
    devOpts_.name = path;
    devOpts_.loop = "1";
    // sets devOpts_'s sample rate and number of channels
    if (!createDecoder()) {
        SIP_CORE_WARN() << "Cannot decode audio from file, switching back to default device";
        return initDevice("");
    }
    fileBuf_ = Manager::instance().getRingBufferPool().createRingBuffer(fileId_);
    // have file audio mixed into the call buffer so it gets sent to the peer
    Manager::instance().getRingBufferPool().bindHalfDuplexOut(id_, fileId_);
    // have file audio mixed into the local buffer so it gets played
    Manager::instance().getRingBufferPool().bindHalfDuplexOut(RingBufferPool::DEFAULT_ID, fileId_);
    decodingFile_ = true;
    deviceGuard_ = Manager::instance().startAudioStream(AudioDeviceType::PLAYBACK);
    return true;
}

std::shared_future<DeviceParams>
AudioInput::switchInput(const std::string& resource)
{
    // Always switch inputs, even if it's the same resource, so audio will be in sync with video
    std::unique_lock<std::mutex> lk(resourceMutex_);

    SIP_CORE_DBG() << "Switching audio source to match '" << resource << "'";

    auto oldGuard = std::move(deviceGuard_);

    decoder_.reset();
    if (decodingFile_) {
        decodingFile_ = false;
        Manager::instance().getRingBufferPool().unBindHalfDuplexOut(id_, fileId_);
        Manager::instance().getRingBufferPool().unBindHalfDuplexOut(RingBufferPool::DEFAULT_ID,
                                                                    fileId_);
    }
    fileBuf_.reset();

    playingDevice_ = false;
    currentResource_ = resource;
    devOptsFound_ = false;
    consecutiveEmptyFrames_ = 0;  // Reset broken device detection counter
    updateMuteStateForDeviceAvailability();

    std::promise<DeviceParams> p;
    foundDevOpts_.swap(p);

    if (resource.empty()) {
        if (initDevice(""))
            foundDevOpts(devOpts_);
    } else {
        static const std::string& sep = libsip_core::Media::VideoProtocolPrefix::SEPARATOR;
        const auto pos = resource.find(sep);
        if (pos == std::string::npos)
            return {};

        const auto prefix = resource.substr(0, pos);
        if ((pos + sep.size()) >= resource.size())
            return {};

        const auto suffix = resource.substr(pos + sep.size());
        bool ready = false;
        if (prefix == libsip_core::Media::VideoProtocolPrefix::FILE)
            ready = initFile(suffix);
        else
            ready = initDevice(suffix);

        if (ready)
            foundDevOpts(devOpts_);
    }

    futureDevOpts_ = foundDevOpts_.get_future().share();
    wakeUp_ = std::chrono::high_resolution_clock::now() + MS_PER_PACKET;
    lk.unlock();
    loop_.start();
    if (onSuccessfulSetup_)
        onSuccessfulSetup_(MEDIA_AUDIO, 0);
    return futureDevOpts_;
}

void
AudioInput::foundDevOpts(const DeviceParams& params)
{
    if (!devOptsFound_) {
        devOptsFound_ = true;
        foundDevOpts_.set_value(params);
    }
}

void
AudioInput::setRecorderCallback(
    const std::function<void(const MediaStream& ms)>&
        cb)
{
    settingMS_.exchange(true);
    recorderCallback_ = cb;
    if (decoder_)
        decoder_->setContextCallback([this]() {
            if (recorderCallback_)
                recorderCallback_(getInfo());
        });
}


bool
AudioInput::createDecoder()
{
    decoder_.reset();
    if (devOpts_.input.empty()) {
        foundDevOpts(devOpts_);
        return false;
    }

    auto decoder = std::make_unique<MediaDecoder>([this](std::shared_ptr<MediaFrame>&& frame) {
        fileBuf_->put(std::static_pointer_cast<AudioFrame>(frame));
    });

    // NOTE don't emulate rate, file is read as frames are needed

    decoder->setInterruptCallback(
        [](void* data) -> int { return not static_cast<AudioInput*>(data)->isCapturing(); }, this);

    if (decoder->openInput(devOpts_) < 0) {
        SIP_CORE_ERR() << "Could not open input '" << devOpts_.input << "'";
        foundDevOpts(devOpts_);
        return false;
    }

    if (decoder->setupAudio() < 0) {
        SIP_CORE_ERR() << "Could not setup decoder for '" << devOpts_.input << "'";
        foundDevOpts(devOpts_);
        return false;
    }

    auto ms = decoder->getStream(devOpts_.input);
    devOpts_.channel = ms.nbChannels;
    devOpts_.framerate = ms.sampleRate;
    SIP_CORE_DBG() << "Created audio decoder: " << ms;

    decoder_ = std::move(decoder);
    foundDevOpts(devOpts_);
    decoder_->setContextCallback([this]() {
        if (recorderCallback_)
            recorderCallback_(getInfo());
    });
    return true;
}

void
AudioInput::setFormat(const AudioFormat& fmt)
{
    std::lock_guard<std::mutex> lk(fmtMutex_);
    format_ = fmt;
    resizer_->setFormat(format_, format_.sample_rate * MS_PER_PACKET.count() / 1000);
}

void
AudioInput::setMuted(bool isMuted)
{
    updateMuteStateForDeviceAvailability();
    if (forceMuteNoDevice_ && !isMuted) {
        SIP_CORE_WARN("Audio Input unmute ignored: no capture devices available");
        return;
    }
    muteState_ = isMuted;
    SIP_CORE_WARN("Audio Input muted [%s]", muteState_ ? "YES" : "NO");
}

MediaStream
AudioInput::getInfo() const
{
    std::lock_guard<std::mutex> lk(fmtMutex_);
    return MediaStream("a:local", format_, sent_samples);
}

MediaStream
AudioInput::getInfo(const std::string& name) const
{
    std::lock_guard<std::mutex> lk(fmtMutex_);
    auto ms = MediaStream(name, format_, sent_samples);
    return ms;
}

void
AudioInput::updateMuteStateForDeviceAvailability()
{
#if defined(TARGET_OS_IOS) && TARGET_OS_IOS
    return;
#endif
    bool hasCaptureDevice = false;
    if (auto driver = Manager::instance().getAudioDriver()) {
        hasCaptureDevice = !driver->getCaptureDeviceList().empty();
    }

    const bool deviceBroken = hasCaptureDevice
                              && consecutiveEmptyFrames_ >= BROKEN_DEVICE_THRESHOLD;
    const bool newForceMute = !hasCaptureDevice || deviceBroken;
    if (newForceMute && !forceMuteNoDevice_) {
        if (deviceBroken)
            SIP_CORE_WARN("Audio Input forcing mute: capture device present but not producing audio");
        else
            SIP_CORE_WARN("Audio Input forcing mute: no capture devices detected");
    } else if (!newForceMute && forceMuteNoDevice_) {
        SIP_CORE_INFO("Audio Input capture device detected; manual mute control restored");
    }

    forceMuteNoDevice_ = newForceMute;
    if (forceMuteNoDevice_) {
        muteState_ = true;
    }
}

} // namespace sip_core
