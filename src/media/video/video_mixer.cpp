/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
 *  Author: Guillaume Roguez <Guillaume.Roguez@savoirfairelinux.com>
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

#include "libav_deps.h" // MUST BE INCLUDED FIRST

#include "video_mixer.h"
#include "media_buffer.h"
#include "client/videomanager.h"
#include "manager.h"
#include "media_filter.h"
#include "sinkclient.h"
#include "logger.h"
#include "filter_transpose.h"
#ifdef RING_ACCEL
#include "accel.h"
#endif
#include "connectivity/sip_utils.h"
#include "string_utils.h"
#include "video_source_utils.h"

#include <cmath>
#include <algorithm>
#include <atomic>
#include <unistd.h>
#include <mutex>
#include <unordered_map>

#include "videomanager_interface.h"

static constexpr auto MIN_LINE_ZOOM
    = 6; // Used by the ONE_BIG_WITH_SMALL layout for the small previews

namespace sip_core {
namespace video {

namespace {
std::string
normalizeDisplayName(std::string name)
{
    if (name.empty())
        return "unknown";

    auto found = name.find('@');
    if (found != std::string_view::npos)
        name = name.substr(0, found);

    found = name.find("<sip:");
    if (found != std::string_view::npos)
        name = name.substr(found + 5);

    found = name.find('>');
    if (found != std::string_view::npos)
        name = name.substr(0, found);

    if (name.empty())
        return "unknown";

    return name;
}

std::string
escapeDrawtext(std::string_view text)
{
    std::string escaped;
    escaped.reserve(text.size() * 2);

    for (char c : text) {
        switch (c) {
        case '\\':
        case '\'':
        case ':':
        case ',':
        case '[':
        case ']':
        case ';':
        case '%':
            escaped.push_back('\\');
            escaped.push_back(c);
            break;
        case '\n':
        case '\r':
            escaped.push_back(' ');
            break;
        default:
            escaped.push_back(c);
            break;
        }
    }

    return escaped;
}
} // namespace

struct VideoMixer::VideoMixerSource
{
    Observable<std::shared_ptr<MediaFrame>>* source {nullptr};
    std::string overlayLabel;
    int rotation {0};
    std::unique_ptr<MediaFilter> transposeFilter {nullptr};
    std::unique_ptr<MediaFilter> bordersFilter {nullptr};
    std::shared_ptr<VideoFrame> render_frame;
    void atomic_copy(const VideoFrame& other)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (muted.load()) {
            auto black_frame = std::make_shared<VideoFrame>();
            black_frame->reserve(AV_PIX_FMT_YUV420P, other.width(), other.height());
            libav_utils::fillWithBlack(black_frame->pointer());
            render_frame = black_frame;
        } else {
            auto newFrame = std::make_shared<VideoFrame>();
            // Deep copy: allocate an independent buffer and copy pixel data.
            // copyFrom() only does av_frame_ref() which keeps data pointers
            // aimed at the original buffer (e.g. V4L2 mmap'd memory).  If
            // the device is torn down on another thread those pages get
            // unmapped while the mixer is still reading them -> SIGSEGV.
            newFrame->reserve(other.format(), other.width(), other.height());
            av_frame_copy(newFrame->pointer(), other.pointer());
            av_frame_copy_props(newFrame->pointer(), other.pointer());
            render_frame = newFrame;
        }
    }

    std::shared_ptr<VideoFrame> getRenderFrame()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return render_frame;
    }

    // Current render informations
    std::atomic<int> x {0};
    std::atomic<int> y {0};
    int w {};
    int h {};
    int lastLayoutFrameWidth {0};
    int lastLayoutFrameHeight {0};
    int lastLayoutOrientation {0};
    bool isBig {false};
    bool geometryPending {false};
    bool hasVideo {true};
    int stableIndex {0};
    std::atomic<bool> muted {false};

private:
    std::mutex mutex_;
};

static constexpr const auto MIXER_FRAMERATE = 30;
static constexpr const auto FRAME_DURATION = std::chrono::duration<double>(1. / MIXER_FRAMERATE);

VideoMixer::VideoMixer(const std::string& id, const std::string& localInput, bool attachHost)
    : VideoGenerator::VideoGenerator()
    , id_(id)
    , sink_(Manager::instance().createSinkClient(id, true))
    , loop_([] { return true; }, std::bind(&VideoMixer::process, this), [] {})
{
    // Local video camera is the main participant. add it if exists
    if (not localInput.empty() && attachHost) {
        auto videoInput = getVideoInput(localInput);
        localInputs_.emplace_back(videoInput);
    }

    // start loop, but do no start video input
    loop_.start();
    nextProcess_ = std::chrono::steady_clock::now();

    SIP_CORE_DBG("[mixer:%s] New instance created", id_.c_str());
}

VideoMixer::~VideoMixer()
{
    stopSink();
    stopInputs();

    loop_.join();

    SIP_CORE_DBG("[mixer:%s] Instance destroyed", id_.c_str());
}

// let the caller control when inputs are actually attached to video mixer
void
VideoMixer::switchInputs(const std::vector<std::string>& inputs, bool muted)
{
    std::vector<std::shared_ptr<VideoInput>> oldInputs;
    {
        std::lock_guard lk(localInputsMtx_);
        oldInputs = localInputs_;
    }

    // Do not stop video inputs that are already there.
    // Detach reused inputs first to refresh their source index.
    decltype(localInputs_) newInputs;
    newInputs.reserve(inputs.size());
    for (const auto& input : inputs) {
        auto videoInput = getVideoInput(input);
        auto normalizedInput = normalizeVideoSwitchSource(input);
        if (videoInput->getName() != normalizedInput) {
            // The global getVideoInput cache may return a VideoInput whose
            // actual capture source was changed by SIPCall::switchInput
            // (e.g. camera→display during a 1:1 call).  Force it back to
            // the requested source.
            videoInput->switchInput(normalizedInput);
        } else {
            // Note, video can be a previously stopped device (eg. restart
            // a screen sharing) — in this case it must be restarted.
            videoInput->restart();
        }
        auto it = std::find(oldInputs.cbegin(), oldInputs.cend(), videoInput);
        if (it != oldInputs.cend())
            videoInput->detach(this);
        newInputs.emplace_back(std::move(videoInput));
    }

    // Stop inputs no longer part of the new set.
    for (const auto& input : oldInputs) {
        auto stillPresent = std::find(newInputs.cbegin(), newInputs.cend(), input)
                            != newInputs.cend();
        if (!stillPresent)
            stopInput(input);
    }

    {
        std::lock_guard lk(localInputsMtx_);
        localInputs_ = std::move(newInputs);
    }

    // Set the mute flag BEFORE attaching so that attached() creates
    // sources already in the correct muted state — no frame leak.
    nextLocalSourceMuted_.store(muted);

    // Re-attach videoInput to mix
    startInputs();

    nextLocalSourceMuted_.store(false);
}

void
VideoMixer::stopInput(const std::shared_ptr<VideoFrameActiveWriter>& input)
{
    // Detach videoInputs from mixer, but DO NOT STOP IT
    input->detach(this);
}

void
VideoMixer::stopInputs()
{
    std::vector<std::shared_ptr<VideoInput>> localInputs;
    {
        std::lock_guard lk(localInputsMtx_);
        localInputs.swap(localInputs_);
    }
    for (auto& input : localInputs)
        stopInput(input);
}

void
VideoMixer::muteInputs(bool mute)
{
    std::vector<VideoFrameActiveWriter*> localInputs;
    {
        std::lock_guard lk(localInputsMtx_);
        localInputs.reserve(localInputs_.size());
        for (const auto& input : localInputs_)
            localInputs.emplace_back(input.get());
    }

    std::shared_lock lock(rwMutex_);
    for (auto& source : sources_) {
        for (auto* input : localInputs) {
            if (source->source == input) {
                source->muted.store(mute);
            }
        }
    }
}

void
VideoMixer::startInputs()
{
    std::vector<std::shared_ptr<VideoInput>> localInputs;
    {
        std::lock_guard lk(localInputsMtx_);
        localInputs = localInputs_;
    }

    // Attach videoInput to mixer and start / restart it if it was stopped before
    for (auto i = 0u; i != localInputs.size(); ++i) {
        // attach video input to mixer AS:
        // no callId
        // video_X as streamId
        attachVideo(localInputs[i].get(), "", sip_utils::streamId("", fmt::format("video_{}", i)));
    }
}

void
VideoMixer::setActiveStream(const std::string& id)
{
    std::unique_lock lock(rwMutex_);
    activeStream_ = id;
    updateLayout("setActiveStream");
}

void
VideoMixer::setVoiceActivity(const std::string& streamId, bool state)
{
    std::unique_lock lock(rwMutex_);
    bool layoutChanged = false;
    applyVoiceActivityStateLocked(streamId, state, std::chrono::steady_clock::now(), layoutChanged);
    if (layoutChanged)
        updateLayout("setVoiceActivity(single)");
}

void
VideoMixer::setVoiceActivity(const std::map<std::string, bool>& states)
{
    std::unique_lock lock(rwMutex_);
    bool layoutChanged = false;
    const auto now = std::chrono::steady_clock::now();
    for (const auto& [streamId, state] : states)
        applyVoiceActivityStateLocked(streamId, state, now, layoutChanged);
    removeStaleVoiceStatesLocked(states, layoutChanged);
    if (layoutChanged)
        updateLayout("setVoiceActivity(map)");
}

void
VideoMixer::setVoiceActivity(const std::map<std::string, bool>&& states)
{
    std::unique_lock lock(rwMutex_);
    bool layoutChanged = false;
    const auto now = std::chrono::steady_clock::now();
    for (const auto& [streamId, state] : states)
        applyVoiceActivityStateLocked(streamId, state, now, layoutChanged);
    removeStaleVoiceStatesLocked(states, layoutChanged);
    if (layoutChanged)
        updateLayout("setVoiceActivity(move)");
}

void
VideoMixer::setVoiceInactiveHoldMs(int holdMs)
{
    std::unique_lock lock(rwMutex_);
    const auto clampedHoldMs = clampVoiceInactiveHoldMs(holdMs);
    if (voiceInactiveHoldMs_ == clampedHoldMs)
        return;

    voiceInactiveHoldMs_ = clampedHoldMs;
    bool layoutChanged = false;
    const auto now = std::chrono::steady_clock::now();
    for (const auto& [streamId, rawState] : voiceActivityRaw_) {
        if (rawState) {
            voiceInactiveDeadlines_.erase(streamId);
            continue;
        }

        auto displayIt = voiceActivityDisplay_.find(streamId);
        const bool displayState = displayIt != voiceActivityDisplay_.end() ? displayIt->second
                                                                           : false;
        if (!displayState) {
            voiceInactiveDeadlines_.erase(streamId);
            continue;
        }

        if (voiceInactiveHoldMs_ == 0) {
            voiceActivityDisplay_[streamId] = false;
            voiceInactiveDeadlines_.erase(streamId);
            layoutChanged = true;
        } else {
            voiceInactiveDeadlines_[streamId] = now
                                                + std::chrono::milliseconds(voiceInactiveHoldMs_);
        }
    }

    if (layoutChanged)
        updateLayout("setVoiceInactiveHoldMs");
}

bool
VideoMixer::moveSource(size_t from_index, size_t to_index)
{
    std::unique_lock lock(rwMutex_);

    size_t size = sources_.size();
    if (from_index == to_index || from_index >= size || to_index >= size)
        return false;

    auto it_from = sources_.begin();
    std::advance(it_from, from_index);

    if (from_index < to_index) {
        if (to_index == sources_.size()) {
            sources_.splice(sources_.end(), sources_, it_from);
        } else {
            auto it_to = sources_.begin();
            std::advance(it_to, to_index);
            sources_.splice(std::next(it_to), sources_, it_from);
        }
    } else { // from > to
        auto it_to = sources_.begin();
        std::advance(it_to, to_index);
        sources_.splice(it_to, sources_, it_from);
    }
    updateLayout("moveSource");
    return true;
}

// just report that layout was updated
void
VideoMixer::updateLayout(const char* reason)
{
    if (activeStream_.empty())
        currentLayout_ = Layout::GRID;
    addLayoutUpdate(reason);
}

int
VideoMixer::addLayoutUpdate(const char* reason)
{
    const int previous = layoutUpdated_.fetch_add(1, std::memory_order_acq_rel);
    const int current = previous + 1;
    SIP_CORE_DBG("[mixer:%s] layoutUpdated_ += 1 (%s) %d -> %d",
                 id_.c_str(),
                 reason ? reason : "unknown",
                 previous,
                 current);
    return current;
}

void
VideoMixer::consumeLayoutUpdates(int count, const char* reason)
{
    if (count <= 0)
        return;
    int previous = layoutUpdated_.fetch_sub(count, std::memory_order_acq_rel);
    int current = previous - count;
    if (current < 0) {
        SIP_CORE_WARN("[mixer:%s] layoutUpdated_ underflow (%s): %d - %d < 0, clamping to 0",
                      id_.c_str(),
                      reason ? reason : "unknown",
                      previous,
                      count);
        current = 0;
        layoutUpdated_.store(0, std::memory_order_release);
    }
    SIP_CORE_DBG("[mixer:%s] layoutUpdated_ -= %d (%s) %d -> %d",
                 id_.c_str(),
                 count,
                 reason ? reason : "unknown",
                 previous,
                 current);
}

int
VideoMixer::clampVoiceInactiveHoldMs(int holdMs)
{
    return holdMs < 0 ? 0 : holdMs;
}

void
VideoMixer::applyVoiceActivityStateLocked(const std::string& streamId,
                                          bool state,
                                          std::chrono::steady_clock::time_point now,
                                          bool& layoutChanged)
{
    auto rawIt = voiceActivityRaw_.find(streamId);
    const bool previousRaw = rawIt != voiceActivityRaw_.end() ? rawIt->second : false;
    auto displayIt = voiceActivityDisplay_.find(streamId);
    const bool previousDisplay = displayIt != voiceActivityDisplay_.end() ? displayIt->second
                                                                          : false;

    if (rawIt == voiceActivityRaw_.end() && !state)
        return;

    if (previousRaw == state)
        return;

    if (rawIt == voiceActivityRaw_.end())
        voiceActivityRaw_.emplace(streamId, state);
    else
        rawIt->second = state;

    if (state) {
        voiceInactiveDeadlines_.erase(streamId);
        if (!previousDisplay) {
            voiceActivityDisplay_[streamId] = true;
            layoutChanged = true;
        } else {
            voiceActivityDisplay_[streamId] = true;
        }
        return;
    }

    // state is false
    if (voiceInactiveHoldMs_ == 0) {
        voiceInactiveDeadlines_.erase(streamId);
        if (previousDisplay) {
            voiceActivityDisplay_[streamId] = false;
            layoutChanged = true;
        } else {
            voiceActivityDisplay_[streamId] = false;
        }
    } else if (previousDisplay) {
        voiceInactiveDeadlines_[streamId] = now + std::chrono::milliseconds(voiceInactiveHoldMs_);
    } else {
        voiceInactiveDeadlines_.erase(streamId);
    }
}

void
VideoMixer::removeStaleVoiceStatesLocked(const std::map<std::string, bool>& states,
                                         bool& layoutChanged)
{
    for (auto it = voiceActivityRaw_.begin(); it != voiceActivityRaw_.end();) {
        if (states.find(it->first) != states.end()) {
            ++it;
            continue;
        }
        const auto streamId = it->first;
        auto displayIt = voiceActivityDisplay_.find(streamId);
        if (displayIt != voiceActivityDisplay_.end() && displayIt->second)
            layoutChanged = true;
        voiceActivityDisplay_.erase(streamId);
        voiceInactiveDeadlines_.erase(streamId);
        it = voiceActivityRaw_.erase(it);
    }
}

bool
VideoMixer::expireVoiceHoldsLocked(std::chrono::steady_clock::time_point now)
{
    bool layoutChanged = false;
    for (auto it = voiceInactiveDeadlines_.begin(); it != voiceInactiveDeadlines_.end();) {
        const auto& streamId = it->first;
        auto rawIt = voiceActivityRaw_.find(streamId);
        if (rawIt == voiceActivityRaw_.end()) {
            voiceActivityDisplay_.erase(streamId);
            it = voiceInactiveDeadlines_.erase(it);
            continue;
        }
        if (rawIt->second) {
            it = voiceInactiveDeadlines_.erase(it);
            continue;
        }
        if (it->second > now) {
            ++it;
            continue;
        }
        auto displayIt = voiceActivityDisplay_.find(streamId);
        if (displayIt != voiceActivityDisplay_.end() && displayIt->second) {
            displayIt->second = false;
            layoutChanged = true;
        }
        it = voiceInactiveDeadlines_.erase(it);
    }
    return layoutChanged;
}

void
VideoMixer::addAudioOnlySource(const std::string& callId,
                               const std::string& streamId,
                               const std::string& overlayLabel)
{
    std::unique_lock lock(rwMutex_);
    auto key = AudioOnlySourceKey {callId, streamId};
    auto [it, inserted] = audioOnlySources_.try_emplace(
        key, AudioOnlySource {callId, streamId, overlayLabel});
    if (!inserted) {
        it->second.callId = callId;
        it->second.streamId = streamId;
        if (!overlayLabel.empty())
            it->second.overlayLabel = overlayLabel;
    }

    // Eagerly create the render-side state so the placeholder gets a
    // stable grid slot at insertion time. Inherit the slot of the
    // corresponding video stream when it exists so that a participant
    // transitioning from video to audio-only (e.g. when held by the host)
    // keeps the same position in the grid instead of jumping to the front.
    auto& renderSource = audioOnlyRenderSources_[key];
    if (!renderSource) {
        renderSource = std::unique_ptr<VideoMixerSource>(new VideoMixerSource);
        renderSource->hasVideo = false;
        // Audio-only stream IDs are derived by replacing "video" with
        // "audio" (see VideoRtpSession::stopReceiver). Reverse that so we
        // can find the previous video streamId in stableOrder_.
        auto videoStreamId = streamId;
        string_replace(videoStreamId, "audio", "video");
        if (auto orderIt = stableOrder_.find(videoStreamId);
            orderIt != stableOrder_.end()) {
            renderSource->stableIndex = orderIt->second;
        } else {
            renderSource->stableIndex = nextStableIndex_++;
            // Register under the video streamId so a later video re-attach
            // (e.g. after unhold) lands on the same slot via attached().
            stableOrder_[videoStreamId] = renderSource->stableIndex;
        }
    }

    updateLayout();
}

void
VideoMixer::removeAudioOnlySource(const std::string& callId, const std::string& streamId)
{
    std::unique_lock lock(rwMutex_);
    auto key = AudioOnlySourceKey {callId, streamId};
    if (audioOnlySources_.erase(key)) {
        // Drop the cached render-side state in lock-step with the metadata
        // entry so the unique_ptr<VideoMixerSource> destructor (which needs
        // the complete VideoMixerSource type) runs from this .cpp.
        audioOnlyRenderSources_.erase(key);
        updateLayout();
    }
}

void
VideoMixer::attachVideo(Observable<std::shared_ptr<MediaFrame>>* frame,
                        const std::string& callId,
                        const std::string& streamId)
{
    if (!frame)
        return;
    SIP_CORE_DBG("Attaching video with streamId %s", streamId.c_str());
    {
        std::lock_guard<std::mutex> lk(videoToStreamInfoMtx_);
        videoToStreamInfo_[frame] = StreamInfo {callId, streamId};
    }
    frame->attach(this);
}

void
VideoMixer::detachVideo(Observable<std::shared_ptr<MediaFrame>>* frame)
{
    if (!frame)
        return;

    bool shouldDetach = false;
    std::string removedStreamId;
    {
        std::lock_guard<std::mutex> lk(videoToStreamInfoMtx_);
        auto it = videoToStreamInfo_.find(frame);
        if (it != videoToStreamInfo_.end()) {
            SIP_CORE_DBG("Detaching video of call %s", it->second.callId.c_str());
            shouldDetach = true;
            removedStreamId = it->second.streamId;
            videoToStreamInfo_.erase(it);
        }
    }

    if (shouldDetach)
        frame->detach(this);

    if (!removedStreamId.empty()) {
        // Handle the case where the current shown source leaves the conference.
        // Note: do not call resetActiveStream() here to avoid duplicate layout updates.
        std::unique_lock lock(rwMutex_);
        if (activeStream_ == removedStreamId)
            activeStream_.clear();
    }
}

VideoMixer::VideoToStream
VideoMixer::getVideoToStreamInfo() const
{
    std::lock_guard<std::mutex> lk(videoToStreamInfoMtx_);
    return videoToStreamInfo_;
}

std::map<std::string, bool>
VideoMixer::getVoiceActivity()
{
    std::shared_lock lk(rwMutex_);
    return voiceActivityDisplay_;
}

void
VideoMixer::attached(Observable<std::shared_ptr<MediaFrame>>* ob)
{
    std::unique_lock lock(rwMutex_);

    // Safety net: if a source for the same observable already exists (e.g. due
    // to a redundant enterConference/detach-reattach cycle where the deferred
    // detach has not yet been processed), remove the stale entry first so that
    // we never hold two sources for the same participant.
    for (auto it = sources_.begin(); it != sources_.end(); ++it) {
        if ((*it)->source == ob) {
            SIP_CORE_WARN("[mixer:%s] Removing stale duplicate source [%p] before re-attach",
                          id_.c_str(),
                          it->get());
            sources_.erase(it);
            break;
        }
    }

    // Also drain any pending detach for the same observable so we don't
    // accidentally remove the source we are about to insert.
    {
        std::lock_guard<std::mutex> ql(pendingDetachMtx_);
        pendingDetaches_.erase(
            std::remove(pendingDetaches_.begin(), pendingDetaches_.end(), ob),
            pendingDetaches_.end());
    }

    // Look up the streamId for this source (populated by attachVideo() before
    // calling frame->attach(this), so the mapping is guaranteed to exist).
    std::string streamId;
    {
        std::lock_guard<std::mutex> lk(videoToStreamInfoMtx_);
        auto it = videoToStreamInfo_.find(ob);
        if (it != videoToStreamInfo_.end())
            streamId = it->second.streamId;
    }

    auto src = std::unique_ptr<VideoMixerSource>(new VideoMixerSource);
    src->render_frame = std::make_shared<VideoFrame>();
    src->source = ob;
    // Inherit the mute state set by switchInputs() so that sources
    // created for muted local inputs never produce visible frames.
    src->muted.store(nextLocalSourceMuted_.load());

    // Assign a stable index: reuse the previous index for reattached sources
    // so that grid positions survive detach/reattach cycles.
    if (!streamId.empty()) {
        auto orderIt = stableOrder_.find(streamId);
        if (orderIt != stableOrder_.end()) {
            src->stableIndex = orderIt->second;
        } else {
            src->stableIndex = nextStableIndex_++;
            stableOrder_[streamId] = src->stableIndex;
        }
    } else {
        src->stableIndex = nextStableIndex_++;
    }

    SIP_CORE_DBG("Add new source [%p] muted=%d stableIndex=%d",
                 src.get(),
                 src->muted.load(),
                 src->stableIndex);

    // Insert at the sorted position to maintain stable ordering.
    auto pos = std::find_if(sources_.begin(), sources_.end(), [&](const auto& s) {
        return s->stableIndex > src->stableIndex;
    });
    sources_.insert(pos, std::move(src));

    SIP_CORE_DEBUG("Total sources: {:d}", sources_.size());
    updateLayout("attached()");
}

void
VideoMixer::detached(Observable<std::shared_ptr<MediaFrame>>* ob)
{
    // Avoid taking rwMutex_ inside observable callback; enqueue and process in mixer loop
    enqueueDetach(ob);
}

void
VideoMixer::update(Observable<std::shared_ptr<MediaFrame>>* ob,
                   const std::shared_ptr<MediaFrame>& frame_p)
{
    std::shared_lock lock(rwMutex_);

    for (const auto& x : sources_) {
        if (x->source == ob) {
#ifdef RING_ACCEL
            std::shared_ptr<VideoFrame> frame;
            try {
                frame = HardwareAccel::transferToMainMemory(*std::static_pointer_cast<VideoFrame>(
                                                                frame_p),
                                                            AV_PIX_FMT_NV12);
                x->atomic_copy(*std::static_pointer_cast<VideoFrame>(frame));
            } catch (const std::runtime_error& e) {
                SIP_CORE_ERR("[mixer:%s] Accel failure: %s", id_.c_str(), e.what());
                return;
            }
#else
            x->atomic_copy(*std::static_pointer_cast<VideoFrame>(frame_p));
#endif
            return;
        }
    }
}

void
VideoMixer::enqueueDetach(Observable<std::shared_ptr<MediaFrame>>* ob)
{
    std::lock_guard<std::mutex> ql(pendingDetachMtx_);
    pendingDetaches_.push_back(ob);
}

std::string
VideoMixer::getCallDisplayName(const std::unique_ptr<VideoMixer::VideoMixerSource>& source,
                               const std::string& fallbackCallId)
{
    if (source && !source->overlayLabel.empty())
        return normalizeDisplayName(source->overlayLabel);

    std::string callId = fallbackCallId;

    if (callId.empty() && source && source->source) {
        std::lock_guard<std::mutex> lk(videoToStreamInfoMtx_);
        auto it = videoToStreamInfo_.find(source->source);
        if (it != videoToStreamInfo_.end())
            callId = it->second.callId;
    }

    if (callId.empty())
        return "host";

    // Use the per-frame display-name cache populated in process() before
    // rwMutex_ was acquired.  This avoids calling Manager::getCallFromCallID()
    // while holding rwMutex_ shared, which could deadlock with Conference
    // threads that hold call/Manager locks and need rwMutex_ exclusive.
    auto cacheIt = displayNameCache_.find(callId);
    if (cacheIt != displayNameCache_.end())
        return cacheIt->second;

    // Fallback for calls not yet in the cache (e.g. just joined this frame).
    // This path is rarely taken and will self-correct next frame.
    if (auto call = Manager::instance().getCallFromCallID(callId)) {
        auto name = call->getPeerDisplayName();
        if (name.empty())
            name = call->getPeerNumber();
        return normalizeDisplayName(name);
    }

    return "unknown";
}

void
VideoMixer::processPendingDetaches()
{
    std::vector<Observable<std::shared_ptr<MediaFrame>>*> local;
    {
        std::lock_guard<std::mutex> ql(pendingDetachMtx_);
        if (pendingDetaches_.empty())
            return;
        local.swap(pendingDetaches_);
    }

    if (local.empty())
        return;

    std::unique_lock lock(rwMutex_);
    for (auto* ob : local) {
        for (const auto& x : sources_) {
            if (x->source == ob) {
                SIP_CORE_DBG("Remove source [%p]", x.get());
                sources_.remove(x);
                SIP_CORE_DEBUG("Total sources: {:d}", sources_.size());
                updateLayout("processPendingDetaches");
                break;
            }
        }
    }
}

void
VideoMixer::process()
{
    // First, process any pending detach requests safely
    processPendingDetaches();

    {
        std::unique_lock lock(rwMutex_);
        if (expireVoiceHoldsLocked(std::chrono::steady_clock::now()))
            updateLayout("voice inactive hold expired");
    }

    nextProcess_ += std::chrono::duration_cast<std::chrono::microseconds>(FRAME_DURATION);
    const auto delay = nextProcess_ - std::chrono::steady_clock::now();
    if (delay.count() > 0)
        std::this_thread::sleep_for(delay);

    // Snapshot mixer dimensions under the lock so that setParameters()
    // on another thread cannot change them between allocation and rendering.
    int frameWidth, frameHeight;
    AVPixelFormat frameFormat;
    {
        std::shared_lock lock(rwMutex_);
        frameWidth = width_;
        frameHeight = height_;
        frameFormat = format_;
    }

    // Nothing to do.
    if (frameWidth == 0 or frameHeight == 0) {
        return;
    }

    VideoFrame& output = getNewFrame();
    try {
        output.reserve(frameFormat, frameWidth, frameHeight);
    } catch (const std::bad_alloc& e) {
        SIP_CORE_ERR("[mixer:%s] VideoFrame::allocBuffer() failed", id_.c_str());
        return;
    }

    // fill with black to make the background black
    libav_utils::fillWithBlack(output.pointer());

    // Populate display-name cache *before* taking rwMutex_ to avoid calling
    // Manager::getCallFromCallID() under the shared lock (deadlock hazard).
#if !CONFERENCE_METADATA
    {
        auto streamInfoCopy = getVideoToStreamInfo();
        displayNameCache_.clear();
        for (const auto& [obs, info] : streamInfoCopy) {
            if (info.callId.empty() || displayNameCache_.count(info.callId))
                continue;
            if (auto call = Manager::instance().getCallFromCallID(info.callId)) {
                auto name = call->getPeerDisplayName();
                if (name.empty())
                    name = call->getPeerNumber();
                displayNameCache_[info.callId] = normalizeDisplayName(name);
            } else {
                displayNameCache_[info.callId] = "unknown";
            }
        }
    }
#endif

    {
        std::shared_lock lock(rwMutex_);

        // collection of patricipants, both audio & video
        std::vector<SourceInfo> sourcesInfo;
        sourcesInfo.reserve(sources_.size() + audioOnlySources_.size());

        // Build a cache of stream infos to avoid taking videoToStreamInfoMtx_ while holding rwMutex_
        VideoToStream streamInfoCache = getVideoToStreamInfo();

        // Snapshot voice activity while rwMutex_ is held by the caller.
        std::map<std::string, bool> voiceActivitySnapshot = voiceActivityDisplay_;

        const int pendingLayoutUpdates = layoutUpdated_.load(std::memory_order_acquire);
        bool needsUpdate = pendingLayoutUpdates > 0;
        int layoutUpdatesGenerated = 0;
        bool layoutInvalidated = false;

        std::shared_ptr<VideoFrame> audioOnlyFrame;
        if (!audioOnlySources_.empty()) {
            int aoW = std::max(2, frameWidth);
            int aoH = std::max(2, frameHeight);
            if (grid_aspect_ > 0.) {
                const auto currentAspect = static_cast<double>(aoW)
                                           / static_cast<double>(aoH);
                if (currentAspect > grid_aspect_) {
                    aoW = std::max(2, static_cast<int>(std::round(aoH * grid_aspect_)));
                } else {
                    aoH = std::max(2, static_cast<int>(std::round(aoW / grid_aspect_)));
                }
            }
            if (aoW % 2 != 0)
                aoW -= 1;
            if (aoH % 2 != 0)
                aoH -= 1;
            aoW = std::max(2, aoW);
            aoH = std::max(2, aoH);

            audioOnlyFrame = std::make_shared<VideoFrame>();
            audioOnlyFrame->reserve(frameFormat, aoW, aoH);
            libav_utils::fillWithBlack(audioOnlyFrame->pointer());
        }

        int i = 0;
        if (!activeStream_.empty())
            i++; // reserve 0 index place for active stream

        // Build a stableIndex-sorted iteration order across video sources
        // and audio-only placeholders. Without this, audio-only placeholders
        // are always rendered before any video source, which makes a held
        // participant jump to the front of the grid the moment its video
        // is replaced by the audio-only placeholder. Sorting both kinds by
        // stableIndex preserves the pre-hold ordering: audio-only sources
        // inherit the held participant's video slot in addAudioOnlySource()
        // (and the same slot is reused by attached() on unhold).
        struct PendingItem
        {
            int stableIndex;
            bool isAudioOnly;
            std::unique_ptr<VideoMixer::VideoMixerSource>* uptr;
            AudioOnlySource* audioOnlySource;
        };
        std::vector<PendingItem> pending;
        pending.reserve(sources_.size() + audioOnlySources_.size());
        for (auto& src : sources_) {
            pending.push_back({src->stableIndex, false, &src, nullptr});
        }
        for (auto& [key, audioOnlySource] : audioOnlySources_) {
            auto& renderSource = audioOnlyRenderSources_[key];
            if (!renderSource) {
                // Defensive lazy init in case process() runs before any
                // addAudioOnlySource() has populated the render-side map.
                renderSource = std::unique_ptr<VideoMixer::VideoMixerSource>(
                    new VideoMixer::VideoMixerSource);
                renderSource->hasVideo = false;
                renderSource->stableIndex = nextStableIndex_++;
            }
            pending.push_back(
                {renderSource->stableIndex, true, &renderSource, &audioOnlySource});
        }
        std::sort(pending.begin(), pending.end(),
                  [](const PendingItem& a, const PendingItem& b) {
                      return a.stableIndex < b.stableIndex;
                  });

        // Iterate and render in stableIndex order. Each placeholder uses a
        // persistent VideoMixerSource cached in audioOnlyRenderSources_ so
        // that calc_position() / initBorderFilter() only run when the layout
        // actually changes (or the cached geometry is still uninitialized).
        // Without that, every audio-only placeholder would rebuild its
        // FFmpeg border/text filter graph on every mixer frame — wasting
        // CPU and flooding the logs while a participant is held.
        for (auto& item : pending) {
            /* thread stop pending? */
            if (!loop_.isRunning())
                return;

            auto& src = *item.uptr;

            if (item.isAudioOnly) {
                auto& audioOnlySource = *item.audioOnlySource;
                // Refresh in case the metadata entry was updated via
                // addAudioOnlySource() since the last frame.
                src->overlayLabel = audioOnlySource.overlayLabel;

                bool voiceActive = false;
                if (auto itVA = voiceActivitySnapshot.find(audioOnlySource.streamId);
                    itVA != voiceActivitySnapshot.end())
                    voiceActive = itVA->second;

                if (!audioOnlyFrame || !audioOnlyFrame->pointer()) {
                    SIP_CORE_WARN("[mixer:%s] No placeholder frame for audio-only source %s",
                                  id_.c_str(),
                                  audioOnlySource.streamId.c_str());
                    i++;
                    continue;
                }

                const bool aoNeedsUpdate = needsUpdate || src->w == 0 || src->h == 0;
                if (aoNeedsUpdate) {
                    processSource(src,
                                  audioOnlyFrame,
                                  i,
                                  audioOnlySource.streamId,
                                  voiceActive,
                                  audioOnlySource.callId);
                }
                render_frame(output, audioOnlyFrame, src, aoNeedsUpdate);

                sourcesInfo.emplace_back(SourceInfo {{},
                                                     src->x.load(),
                                                     src->y.load(),
                                                     src->w,
                                                     src->h,
                                                     src->hasVideo,
                                                     audioOnlySource.callId,
                                                     audioOnlySource.streamId});
                i++;
                continue;
            }

            // Video source path.
            if (src->w == 0 || src->h == 0)
                needsUpdate = true;

            StreamInfo sinfo = {};
            if (auto itSI = streamInfoCache.find(src->source); itSI != streamInfoCache.end())
                sinfo = itSI->second;

            bool voiceActive = false;
            if (auto itVA = voiceActivitySnapshot.find(sinfo.streamId);
                itVA != voiceActivitySnapshot.end())
                voiceActive = itVA->second;

            // make rendered frame temporarily unavailable for update()
            // to avoid concurrent access.
            std::shared_ptr<VideoFrame> input = src->getRenderFrame();

            // Skip processing if input frame is null (can happen when video is just attached
            // or when all participants turn off video)
            if (!input) {
                SIP_CORE_DBG("[mixer:%s] No frame yet for source %p", id_.c_str(), src->source);
                sourcesInfo.emplace_back(SourceInfo {src->source,
                                                     src->x.load(),
                                                     src->y.load(),
                                                     src->w,
                                                     src->h,
                                                     false,
                                                     sinfo.callId,
                                                     sinfo.streamId});
                ++i;
                continue;
            }

            if (input->height() and input->width()) {
                if (input->width() != src->lastLayoutFrameWidth
                    || input->height() != src->lastLayoutFrameHeight
                    || input->getOrientation() != src->lastLayoutOrientation) {
                    needsUpdate = true;
                    src->lastLayoutFrameWidth = input->width();
                    src->lastLayoutFrameHeight = input->height();
                    src->lastLayoutOrientation = input->getOrientation();
                }
            }

            if (needsUpdate)
                processSource(src, input, i, sinfo.streamId, voiceActive, sinfo.callId);

            bool frameRendered = false;
            if (src->w > 0 and src->h > 0 and input->height() and input->width()) {
                frameRendered = render_frame(output, input, src, needsUpdate);
            } else if (input->height() == 0 or input->width() == 0) {
                SIP_CORE_WARN("[mixer:%s] Nothing to render for %p", id_.c_str(), src->source);
            }

            if (frameRendered != src->hasVideo) {
                src->hasVideo = frameRendered;
                layoutInvalidated = true;
            }

            sourcesInfo.emplace_back(SourceInfo {src->source,
                                                 src->x.load(),
                                                 src->y.load(),
                                                 src->w,
                                                 src->h,
                                                 src->hasVideo,
                                                 sinfo.callId,
                                                 sinfo.streamId});

            ++i;
        }

        if (needsUpdate && !layoutInvalidated) {
            const int totalUpdatesToConsume = pendingLayoutUpdates + layoutUpdatesGenerated;
            if (totalUpdatesToConsume > 0)
                consumeLayoutUpdates(totalUpdatesToConsume, "layout processed");

            if (onSourcesUpdated_)
                onSourcesUpdated_(std::move(sourcesInfo));
        }
    }

    output.pointer()->pts = av_rescale_q_rnd(av_gettime() - startTime_,
                                             {1, AV_TIME_BASE},
                                             {1, MIXER_FRAMERATE},
                                             static_cast<AVRounding>(AV_ROUND_NEAR_INF
                                                                     | AV_ROUND_PASS_MINMAX));
    lastTimestamp_ = output.pointer()->pts;
    publishFrame();
}

void
VideoMixer::processSource(std::unique_ptr<VideoMixer::VideoMixerSource>& source,
                          const std::shared_ptr<VideoFrame> frame,
                          int& i,
                          const std::string& streamId,
                          bool isVoiceActive,
                          const std::string& callId)
{
    // set "wantedIndex" to current index of video source, for GRID layout
    auto wantedIndex = i;
    const bool sourceIsActive = !activeStream_.empty() && activeStream_ == streamId;
    if (currentLayout_ == Layout::ONE_BIG) {
        // show active stream FIRST
        if (sourceIsActive) {
            wantedIndex = 0;
            i--; // negilate i++ further
        } else {
            // Hidden in ONE_BIG: zero out geometry and skip calc_position
            // entirely so it cannot overwrite w/h back to non-zero values.
            source->x.store(0);
            source->y.store(0);
            source->w = 0;
            source->h = 0;
            source->hasVideo = false;
            source->bordersFilter.reset();
            return;
        }
    } else {
        if (currentLayout_ == Layout::ONE_BIG_WITH_SMALL && sourceIsActive) {
            wantedIndex = 0;
            i--; // negilate i++ further
        }
    }

    calc_position(source, frame, wantedIndex, isVoiceActive, callId);
}

bool
VideoMixer::render_frame(VideoFrame& output,
                         const std::shared_ptr<VideoFrame>& input,
                         std::unique_ptr<VideoMixerSource>& source,
                         bool positionChanged)
{
    if (!width_ or !height_ or !input->pointer() or input->pointer()->format == -1)
        return false;

    int cell_width = source->w;
    int cell_height = source->h;
    int xoff = source->x.load();
    int yoff = source->y.load();

    int angle = input->getOrientation();
    const constexpr char filterIn[] = "mixin";
    if (angle != source->rotation || positionChanged) {
        // calculate width and height for cropping
        int width = 0, height = 0;
        if (remove_black_borders_ && not source->isBig) {
            // calculte cropping according to aspects
            if (grid_aspect_ > input->width() / input->height()) {
                width = input->width();
                height = width / grid_aspect_;
            } else {
                height = input->height();
                width = height * grid_aspect_;
            }
        }
        source->transposeFilter = video::getTransposeFilterWithCrop(filterIn,
                                                                    angle,
                                                                    width,
                                                                    height,
                                                                    input->format());
        source->rotation = angle;
    }
    std::shared_ptr<VideoFrame> frame;
    if (source->transposeFilter) {
        source->transposeFilter->feedInput(input->pointer(), filterIn);
        frame = std::static_pointer_cast<VideoFrame>(
            std::shared_ptr<MediaFrame>(source->transposeFilter->readOutput()));
    } else {
        frame = input;
    }

    scaler_.scale_and_pad(*frame, output, xoff, yoff, cell_width, cell_height, true);

    if (source->bordersFilter) {
        source->bordersFilter->feedInput(output.pointer(), borderFilterName_);
        std::unique_ptr<MediaFrame> clone = source->bordersFilter->readOutput();
        if (clone.get())
            output.copyFrom(*std::static_pointer_cast<VideoFrame>(
                std::shared_ptr<MediaFrame>(clone.release())));
    }

    return true;
}

void
VideoMixer::calc_position(std::unique_ptr<VideoMixerSource>& source,
                          const std::shared_ptr<VideoFrame>& input,
                          int index,
                          bool isActive,
                          const std::string& callId)
{
    if (!width_ or !height_)
        return;

    // Defensive check: input should never be null at this point
    if (!input) {
        SIP_CORE_WARN("[mixer:%s] calc_position called with null input", id_.c_str());
        return;
    }

    int frameW, frameH, frameW_off, frameH_off;
    if (grid_aspect_ == 0.) {
        std::tie(frameW, frameH, frameW_off, frameH_off) = calc_position_rel(source,
                                                                             input,
                                                                             index,
                                                                             isActive);
    } else {
        std::tie(frameW, frameH, frameW_off, frameH_off) = calc_position_fixed(source,
                                                                               input,
                                                                               index,
                                                                               isActive);
    }

    // Update source's cache
    source->w = frameW - (padding_ + border_size_) * 2;
    source->h = frameH - (padding_ + border_size_) * 2;
    source->x.store(frameW_off + padding_ + border_size_);
    source->y.store(frameH_off + padding_ + border_size_);

    // Update border filter
#if CONFERENCE_METADATA
    // Text overlays are disabled when CONFERENCE_METADATA is on, so the display
    // name is unused.  Skip the Manager call that would otherwise be made under
    // rwMutex_ (deadlock hazard, see plan Fix D).
    std::string display;
#else
    std::string display = getCallDisplayName(source, callId);
#endif
    auto tryInitBorderFilter = [&](bool withText) {
        auto filter = std::make_unique<MediaFilter>();
        if (!initBorderFilter(*filter,
                              display,
                              input->format(),
                              source->x.load(),
                              source->y.load(),
                              source->w,
                              source->h,
                              isActive,
                              withText))
            return false;
        source->bordersFilter = std::move(filter);
        return true;
    };
#if CONFERENCE_METADATA
    if (!tryInitBorderFilter(false))
        source->bordersFilter.reset();
#else
    if (!tryInitBorderFilter(true))
        source->bordersFilter.reset();
#endif
}

VideoMixer::gripRect
VideoMixer::calc_position_rel(std::unique_ptr<VideoMixerSource>& source,
                              const std::shared_ptr<VideoFrame>& input,
                              int index,
                              bool isActive)
{
    // Compute cell size/position
    int cell_width, cell_height, cellW_off, cellH_off;
    const int n = currentLayout_ == Layout::ONE_BIG ? 1
                                                    : sources_.size() + audioOnlySources_.size();
    const int zoom = currentLayout_ == Layout::ONE_BIG_WITH_SMALL ? std::max(MIN_LINE_ZOOM, n)
                                                                  : ceil(sqrt(n));
    if (currentLayout_ == Layout::ONE_BIG_WITH_SMALL && index == 0) {
        // In ONE_BIG_WITH_SMALL, the first line at the top is the previews
        // The rest is the active source
        cell_width = width_;
        cell_height = height_ - height_ / zoom;
    } else {
        cell_width = width_ / zoom;
        cell_height = height_ / zoom;

        if (n == 1) {
            // On some platforms (at least macOS/android) - Having one frame at the same
            // size of the mixer cause it to be grey.
            // Removing some pixels solve this. We use 16 because it's a multiple of 8
            // (value that we prefer for video management)
            cell_width -= 16;
            cell_height -= 16;
        }
    }
    if (currentLayout_ == Layout::ONE_BIG_WITH_SMALL) {
        if (index == 0) {
            cellW_off = 0;
            cellH_off = height_ / zoom; // First line height
        } else {
            cellW_off = (index - 1) * cell_width;
            // Show sources in center
            cellW_off += (width_ - (n - 1) * cell_width) / 2;
            cellH_off = 0;
        }
    } else {
        cellW_off = (index % zoom) * cell_width;
        if (currentLayout_ == Layout::GRID && n % zoom != 0 && index >= (zoom * ((n - 1) / zoom))) {
            // Last line, center participants if not full
            cellW_off += (width_ - (n % zoom) * cell_width) / 2;
        }
        cellH_off = (index / zoom) * cell_height;
        if (n == 1) {
            // Centerize (cellwidth = width_ - 16)
            cellW_off += 8;
            cellH_off += 8;
        }
    }

    // Compute frame size/position
    float zoomW, zoomH;
    int frameW, frameH, frameW_off, frameH_off;

    if (input->getOrientation() % 180) {
        // Rotated frame
        zoomW = (float) input->height() / cell_width;
        zoomH = (float) input->width() / cell_height;
        frameH = std::round(input->width() / std::max(zoomW, zoomH));
        frameW = std::round(input->height() / std::max(zoomW, zoomH));
    } else {
        zoomW = (float) input->width() / cell_width;
        zoomH = (float) input->height() / cell_height;
        frameW = std::round(input->width() / std::max(zoomW, zoomH));
        frameH = std::round(input->height() / std::max(zoomW, zoomH));
    }

    // Center the frame in the cell
    frameW_off = cellW_off + (cell_width - frameW) / 2;
    frameH_off = cellH_off + (cell_height - frameH) / 2;

    return {frameW, frameH, frameW_off, frameH_off};
}

VideoMixer::gripRect
VideoMixer::calc_position_fixed(std::unique_ptr<VideoMixerSource>& source,
                                const std::shared_ptr<VideoFrame>& input,
                                int index,
                                bool isActive)
{
    int frameW, frameH, frameW_off, frameH_off;
    const int n = sources_.size() + audioOnlySources_.size();

    auto layout_to_draw = n == 1 ? Layout::ONE_BIG : currentLayout_;
    switch (layout_to_draw) {
    case Layout::ONE_BIG:
        // On some platforms (at least macOS/android) - Having one frame at the same
        // size of the mixer cause it to be grey.
        // Removing some pixels solve this. We use 16 because it's a multiple of 8
        // (value that we prefer for video management)
        frameW = width_ - 16;
        frameH = height_ - 16;

        frameW_off = index * frameW;
        frameH_off = index * frameH;

        // Centerize (cellwidth = width_ - 16)
        frameW_off += 8;
        frameH_off += 8;

        source->isBig = true;
        break;

    case Layout::ONE_BIG_WITH_SMALL: {
        const int max_preview_height = height_ / MIN_LINE_ZOOM;
        const int preview_height = std::min(static_cast<int>(width_ / (n * grid_aspect_)),
                                            max_preview_height);
        const int preview_width = preview_height * grid_aspect_;

        if (index == 0) {
            // In ONE_BIG_WITH_SMALL, the first line at the top is the previews
            // The rest is the active source
            frameH = height_ - preview_height;
            frameW = width_;

            frameW_off = 0;
            frameH_off = preview_height; // First line height
            source->isBig = true;
        } else {
            frameW = preview_width;
            frameH = preview_height;

            frameW_off = (index - 1) * frameW;
            // Show sources in center
            frameW_off += (width_ - (n - 1) * frameW) / 2;
            frameH_off = 0;
            source->isBig = false;
        }
        break;
    }
    case Layout::GRID: {
        int rows = 0, columns = 0;
        double source_aspect = (double) width_ / (double) height_;

        // calculate optimal grid. Can be cached for optimization
        bool isVerticalAlign = source_aspect < grid_aspect_;
        {
            if (isVerticalAlign) {
                // vertical alignment
                rows = 2 * height_ * grid_aspect_ / width_;
                rows = std::min(rows, n);
                columns = ((n + 1) / rows) + 1;
                if (columns == 1)
                    isVerticalAlign = !isVerticalAlign;
            } else {
                // horizontal alignment
                columns = 2 * width_ / (height_ * grid_aspect_);
                columns = std::min(columns, n);
                rows = ((n - 1) / columns) + 1;
                if (rows == 1)
                    isVerticalAlign = !isVerticalAlign;
            }
        }

        if (isVerticalAlign) {
            frameW = width_ / columns;
            frameH = frameW / grid_aspect_;
        } else {
            frameH = height_ / rows;
            frameW = frameH * grid_aspect_;
        }

        frameH_off = (index / columns) * frameH;
        frameW_off = (index % columns) * frameW;

        // center participants
        frameH_off += (height_ - (rows * frameH)) / 2;
        if (n % columns != 0 && index >= (columns * (rows - 1))) // if we draw last and not full row
            frameW_off += (width_ - (n % columns) * frameW) / 2;
        else
            frameW_off += (width_ - (columns * frameW)) / 2;

        source->isBig = false;
        break;
    }
    }

    return {frameW, frameH, frameW_off, frameH_off};
}

bool
VideoMixer::initBorderFilter(MediaFilter& filter,
                             std::string inputName,
                             int format,
                             int x,
                             int y,
                             int width,
                             int height,
                             bool active,
                             bool withText)
{
    if (border_size_ <= 0)
        return false;

    std::stringstream ss;
    ss << "[" << borderFilterName_ << "] ";
    ss << "drawbox=x=" << x - (border_size_) << ":y=" << y - (border_size_)
       << ":w=" << width + (border_size_ * 2) << ":h=" << height + (border_size_ * 2)
       << ":color=" << (active ? active_border_color_ : inactive_border_color_)
       << ":t=" << border_size_;

    if (withText) {
        const int text_height = std::max(12, height / 15);
        constexpr int text_padding = 10;
        ss << ",drawtext=text='" << escapeDrawtext(inputName) << "'"
           << ":fontcolor=white:fontsize=" << text_height << ":x=" << x << "+(" << width
           << "-text_w)/2"
           << ":y=" << y << "+" << height - text_padding << "-text_h";
    }

    constexpr auto one = rational<int>(1);
    std::vector<MediaStream> msv;
    msv.emplace_back(borderFilterName_, format, one, width_, height_, 0, one);

    auto ret = filter.initialize(ss.str(), msv);
    if (ret < 0) {
        SIP_CORE_ERR() << "filter init fail";
        return false;
    }

    return true;
}

void
VideoMixer::setParameters(const Parameters& params)
{
    std::unique_lock lock(rwMutex_);

    width_ = params.width;
    height_ = params.height;
    format_ = params.format;
    grid_aspect_ = fabs(params.grid_aspect);
    padding_ = params.padding < 0 ? 0 : params.padding;
    border_size_ = params.border_size < 0 ? 0 : params.border_size;
    active_border_color_ = params.active_border_color;
    inactive_border_color_ = params.inactive_border_color;
    remove_black_borders_ = params.remove_black_borders;
    voiceInactiveHoldMs_ = clampVoiceInactiveHoldMs(params.voice_inactive_hold_ms);

    // cleanup the previous frame to have a nice copy in rendering method
    std::shared_ptr<VideoFrame> previous_p(obtainLastFrame());
    if (previous_p)
        libav_utils::fillWithBlack(previous_p->pointer());

    startSink();
    updateLayout("setParameters");
    startTime_ = av_gettime();
}

void
VideoMixer::startSink()
{
    stopSink();

    if (width_ == 0 or height_ == 0) {
        SIP_CORE_WARN("[mixer:%s] MX: unable to start with zero-sized output", id_.c_str());
        return;
    }

    if (not sink_->start()) {
        SIP_CORE_ERR("[mixer:%s] MX: sink startup failed", id_.c_str());
        return;
    }

    if (this->attach(sink_.get()))
        sink_->setFrameSize(width_, height_);
}

void
VideoMixer::stopSink()
{
    this->detach(sink_.get());
    sink_->stop();
}

int
VideoMixer::getWidth() const
{
    return width_;
}

int
VideoMixer::getHeight() const
{
    return height_;
}

AVPixelFormat
VideoMixer::getPixelFormat() const
{
    return format_;
}

MediaStream
VideoMixer::getStream(const std::string& name) const
{
    MediaStream ms;
    ms.name = name;
    ms.format = format_;
    ms.isVideo = true;
    ms.height = height_;
    ms.width = width_;
    ms.frameRate = {MIXER_FRAMERATE, 1};
    ms.timeBase = {1, MIXER_FRAMERATE};
    ms.firstTimestamp = lastTimestamp_;

    return ms;
}

void
VideoMixer::setVideoLayout(Layout newLayout)
{
    std::unique_lock lock(rwMutex_);
    currentLayout_ = newLayout;

    if (currentLayout_ == Layout::GRID)
        activeStream_ = {};

    // Force a clean single-frame recalculation for all sources.
    // Reset cached frame dimensions so process() detects a geometry change,
    // and pre-set hasVideo to the expected post-switch state to avoid
    // multi-frame settling caused by layoutInvalidated.
    const bool allVisible = (currentLayout_ != Layout::ONE_BIG);
    for (auto& source : sources_) {
        source->w = 0;
        source->h = 0;
        source->isBig = false;
        source->lastLayoutFrameWidth = 0;
        source->lastLayoutFrameHeight = 0;
        source->lastLayoutOrientation = 0;
        source->hasVideo = allVisible;
    }

    addLayoutUpdate("setVideoLayout");
}

} // namespace video
} // namespace sip_core
