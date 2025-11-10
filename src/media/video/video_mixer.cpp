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

#include <cmath>
#include <atomic>
#include <unistd.h>
#include <mutex>
#include <unordered_map>

#include "videomanager_interface.h"

static constexpr auto MIN_LINE_ZOOM
    = 6; // Used by the ONE_BIG_WITH_SMALL layout for the small previews

namespace sip_core {
namespace video {

struct VideoMixer::VideoMixerSource
{
    Observable<std::shared_ptr<MediaFrame>>* source {nullptr};
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
            if (render_frame) {
                if (render_frame->width() != other.width()
                    or render_frame->height() != other.height()) {
                    w = 0;
                    h = 0;
                }
            }
            auto newFrame = std::make_shared<VideoFrame>();
            newFrame->copyFrom(other);
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
VideoMixer::switchInputs(const std::vector<std::string>& inputs)
{
    // Do not stop video inputs that are already there
    // But only detach it to get new index
    std::lock_guard lk(localInputsMtx_);
    decltype(localInputs_) newInputs;
    newInputs.reserve(inputs.size());
    for (const auto& input : inputs) {
        auto videoInput = getVideoInput(input);
        // Note, video can be a previously stopped device (eg. restart a screen sharing)
        // in this case, the videoInput will be found and must be restarted
        videoInput->restart();
        auto it = std::find(localInputs_.cbegin(), localInputs_.cend(), videoInput);
        auto onlyDetach = it != localInputs_.cend();
        if (onlyDetach) {
            videoInput->detach(this);
            localInputs_.erase(it);
        }
        newInputs.emplace_back(std::move(videoInput));
    }
    // Stop other video inputs
    stopInputs();
    localInputs_ = std::move(newInputs);

    // Re-attach videoInput to mix
    startInputs();
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
    for (auto& input : localInputs_)
        stopInput(input);

    localInputs_.clear();
}

void
VideoMixer::muteInputs(bool mute)
{
    for (auto& source : sources_) {
        for (auto& input : localInputs_) {
            if (source->source == input.get()) {
                source->muted.store(mute);
            }
        }
    }
}

void
VideoMixer::startInputs()
{
    // Attach videoInput to mixer and start / restart it if it was stopped before
    for (auto i = 0u; i != localInputs_.size(); ++i) {
        // attach video input to mixer AS:
        // no callId
        // video_X as streamId
        attachVideo(localInputs_[i].get(), "", sip_utils::streamId("", fmt::format("video_{}", i)));
    }
}

void
VideoMixer::setActiveStream(const std::string& id)
{
    activeStream_ = id;
    std::unique_lock lock(rwMutex_);
    updateLayout("setActiveStream");
}

void
VideoMixer::setVoiceActivity(const std::string& streamId, bool state)
{
    std::lock_guard<std::mutex> voiceLock(vocieActivivtyMtx_);
    voiceActivity_[streamId] = state;
    std::unique_lock lock(rwMutex_);
    updateLayout("setVoiceActivity(single)");
}

void
VideoMixer::setVoiceActivity(const std::map<std::string, bool>& states)
{
    std::lock_guard<std::mutex> voiceLock(vocieActivivtyMtx_);
    voiceActivity_ = states;
    std::unique_lock lock(rwMutex_);
    updateLayout("setVoiceActivity(map)");
}

void
VideoMixer::setVoiceActivity(const std::map<std::string, bool>&& states)
{
    std::lock_guard<std::mutex> voiceLock(vocieActivivtyMtx_);
    voiceActivity_ = std::move(states);
    std::unique_lock lock(rwMutex_);
    updateLayout("setVoiceActivity(move)");
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
    bool detach = false;
    std::unique_lock<std::mutex> lk(videoToStreamInfoMtx_);
    auto it = videoToStreamInfo_.find(frame);
    if (it != videoToStreamInfo_.end()) {
        SIP_CORE_DBG("Detaching video of call %s", it->second.callId.c_str());
        detach = true;
        // Handle the case where the current shown source leave the conference
        // Note, do not call resetActiveStream() to avoid multiple updates
        if (verifyActive(it->second.streamId))
            activeStream_ = {};
        videoToStreamInfo_.erase(it);
    }
    lk.unlock();
    if (detach)
        frame->detach(this);
}

void
VideoMixer::attached(Observable<std::shared_ptr<MediaFrame>>* ob)
{
    std::unique_lock lock(rwMutex_);

    auto src = std::unique_ptr<VideoMixerSource>(new VideoMixerSource);
    src->render_frame = std::make_shared<VideoFrame>();
    src->source = ob;
    SIP_CORE_DBG("Add new source [%p]", src.get());
    sources_.emplace_back(std::move(src));
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

    // Build a cache of stream infos to avoid taking videoToStreamInfoMtx_ while holding rwMutex_
    std::unordered_map<Observable<std::shared_ptr<MediaFrame>>*, StreamInfo> streamInfoCache;
    {
        std::lock_guard<std::mutex> lk(videoToStreamInfoMtx_);
        streamInfoCache.reserve(videoToStreamInfo_.size());
        for (const auto& kv : videoToStreamInfo_)
            streamInfoCache.emplace(kv.first, kv.second);
    }

    // Snapshot voice activity to avoid locking vocieActivivtyMtx_ under rwMutex_
    std::map<std::string, bool> voiceActivitySnapshot;
    {
        std::lock_guard<std::mutex> lock(vocieActivivtyMtx_);
        voiceActivitySnapshot = voiceActivity_;
    }

    nextProcess_ += std::chrono::duration_cast<std::chrono::microseconds>(FRAME_DURATION);
    const auto delay = nextProcess_ - std::chrono::steady_clock::now();
    if (delay.count() > 0)
        std::this_thread::sleep_for(delay);

    // Nothing to do.
    if (width_ == 0 or height_ == 0) {
        return;
    }

    VideoFrame& output = getNewFrame();
    try {
        output.reserve(format_, width_, height_);
    } catch (const std::bad_alloc& e) {
        SIP_CORE_ERR("[mixer:%s] VideoFrame::allocBuffer() failed", id_.c_str());
        return;
    }

    // fill with black to make the background black
    libav_utils::fillWithBlack(output.pointer());

    {
        std::lock_guard<std::mutex> lk(audioOnlySourcesMtx_);
        std::shared_lock lock(rwMutex_);

        // does current frame is SUCCESSFULLY rendered?
        bool successfullyRendered = audioOnlySources_.size() != 0 && sources_.size() == 0;

        // collection of patricipants, both audio & video
        std::vector<SourceInfo> sourcesInfo;
        sourcesInfo.reserve(sources_.size() + audioOnlySources_.size());

        int i = 0;

        // did active stream was found?
        bool activeFound = false;

        const int pendingLayoutUpdates = layoutUpdated_.load(std::memory_order_acquire);
        bool needsUpdate = pendingLayoutUpdates > 0;
        int layoutUpdatesGenerated = 0;
        bool layoutInvalidated = false;

        auto requestLayoutUpdate = [&](const char* reason) {
            addLayoutUpdate(reason);
            ++layoutUpdatesGenerated;
            needsUpdate = true;
        };

        auto invalidateAndRequestLayoutUpdate = [&](const char* reason) {
            addLayoutUpdate(reason);
            ++layoutUpdatesGenerated;
            needsUpdate = true;
            layoutInvalidated = true;
        };

        // first, iterate and draw audioOnlySources_
        for (auto& [callId, streamId] : audioOnlySources_) {
            auto isActiveSource = verifyActive(streamId);
            std::shared_ptr<VideoFrame> audioFrame = std::make_shared<VideoFrame>();
            audioFrame->reserve(format_, 640, 480);

            // set "wantedIndex" to current index of video source, for GRID layout
            auto wantedIndex = i;
            if (currentLayout_ == Layout::ONE_BIG) {
                // reset to zero if ONE_BIG layout
                wantedIndex = 0;
                activeFound = true;
            } else if (currentLayout_ == Layout::ONE_BIG_WITH_SMALL) {
                // show active stream FIRST
                if (isActiveSource) {
                    wantedIndex = 0;
                    activeFound = true;
                } else if (not activeFound) {
                    // active streams appears at i == 3
                    // 1 2 3 0 4 5 6
                    wantedIndex += 1;
                }
            }

            auto audioSource = std::make_unique<VideoMixer::VideoMixerSource>();

            // calc pos, but DO NOT render anything
            bool voiceActive = false;
            if (auto itVA = voiceActivitySnapshot.find(streamId); itVA != voiceActivitySnapshot.end())
                voiceActive = itVA->second;
            if (needsUpdate) {
                calc_position(audioSource, audioFrame, wantedIndex, voiceActive);
            }
            sourcesInfo.emplace_back(SourceInfo {{},
                                                 audioSource->x.load(),
                                                 audioSource->y.load(),
                                                 audioSource->w,
                                                 audioSource->h,
                                                 false,
                                                 callId,
                                                 streamId});
            i++;
        }

        // add video sources
        for (auto& x : sources_) {
            /* thread stop pending? */
            if (!loop_.isRunning())
                return;

            StreamInfo sinfo = {};
            if (auto itSI = streamInfoCache.find(x->source); itSI != streamInfoCache.end())
                sinfo = itSI->second;
            auto activeSource = verifyActive(sinfo.streamId);

            if (currentLayout_ != Layout::ONE_BIG or activeSource) {
                // make rendered frame temporarily unavailable for update()
                // to avoid concurrent access.
                std::shared_ptr<VideoFrame> input = x->getRenderFrame();
                std::shared_ptr<VideoFrame> fooInput = std::make_shared<VideoFrame>();

                // set "wantedIndex" to current index of video source, for GRID layout
                auto wantedIndex = i;
                if (currentLayout_ == Layout::ONE_BIG) {
                    // reset to zero if ONE_BIG layout
                    wantedIndex = 0;
                    activeFound = true;
                } else if (currentLayout_ == Layout::ONE_BIG_WITH_SMALL) {
                    // show active stream FIRST
                    if (activeSource) {
                        wantedIndex = 0;
                        activeFound = true;
                    } else if (not activeFound) {
                        // active streams appears at i == 3
                        // 1 2 3 0 4 5 6
                        wantedIndex += 1;
                    }
                }

                auto previousHasVideo = x->hasVideo;
                bool blackFrame = false;

                if (!input->height() or !input->width()) {
                    successfullyRendered = true;
                    fooInput->reserve(format_, width_, height_);
                    blackFrame = true;
                } else {
                    fooInput.swap(input);
                }

                const int frameWidth = fooInput->width();
                const int frameHeight = fooInput->height();
                const int frameOrientation = fooInput->getOrientation();
                const bool requiresInitialLayout = (x->w == 0 || x->h == 0);
                const bool geometryChanged = !blackFrame
                    && (frameWidth != x->lastLayoutFrameWidth
                        || frameHeight != x->lastLayoutFrameHeight
                        || frameOrientation != x->lastLayoutOrientation);

                if (requiresInitialLayout)
                    requestLayoutUpdate("initial source layout");

                if (geometryChanged && !x->geometryPending) {
                    requestLayoutUpdate("frame geometry changed");
                    x->geometryPending = true;
                }

                if (needsUpdate) {
                    bool voiceActive = false;
                    if (auto itVA = voiceActivitySnapshot.find(sinfo.streamId);
                        itVA != voiceActivitySnapshot.end())
                        voiceActive = itVA->second;
                    calc_position(x, fooInput, wantedIndex, voiceActive);
                    if (!blackFrame) {
                        x->lastLayoutFrameWidth = frameWidth;
                        x->lastLayoutFrameHeight = frameHeight;
                        x->lastLayoutOrientation = frameOrientation;
                        x->geometryPending = false;
                    }
                }

                if (!blackFrame) {
                    if (fooInput)
                        successfullyRendered |= render_frame(output, fooInput, x, needsUpdate);
                    else
                        SIP_CORE_WARN("[mixer:%s] Nothing to render for %p", id_.c_str(), x->source);
                }

                x->hasVideo = !blackFrame && successfullyRendered;
                if (previousHasVideo != x->hasVideo) {
                    invalidateAndRequestLayoutUpdate("video availability changed");
                }
            } else if (needsUpdate) {
                x->x.store(0);
                x->y.store(0);
                x->w = 0;
                x->h = 0;
                x->hasVideo = false;
            }

            ++i;
        }
        if (needsUpdate && successfullyRendered && !layoutInvalidated) {
            const int totalUpdatesToConsume = pendingLayoutUpdates + layoutUpdatesGenerated;
            if (totalUpdatesToConsume > 0)
                consumeLayoutUpdates(totalUpdatesToConsume, "layout processed");
            for (auto& x : sources_) {
                StreamInfo sinfo = {};
                if (auto itSI = streamInfoCache.find(x->source); itSI != streamInfoCache.end())
                    sinfo = itSI->second;
                sourcesInfo.emplace_back(SourceInfo {x->source,
                                                     x->x.load(),
                                                     x->y.load(),
                                                     x->w,
                                                     x->h,
                                                     x->hasVideo,
                                                     sinfo.callId,
                                                     sinfo.streamId});
            }
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
        int width = 0, height = 0;
        if(remove_black_borders_ && not source->isBig) {
            // calculte cropping according to aspects
            if(grid_aspect_ > input->width() / input->height()) {
                width = input->width();
                height = width / grid_aspect_;
            }
            else {
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

    const constexpr char borderFilter[] = "border";
    if (source->bordersFilter) {
        source->bordersFilter->feedInput(output.pointer(), borderFilter);
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
                          bool isActive)
{
    if (!width_ or !height_)
        return;

    int frameW, frameH, frameW_off, frameH_off;
    if(grid_aspect_ == 0.) {
        std::tie(frameW, frameH, frameW_off, frameH_off) 
            = calc_position_rel(source, input, index, isActive);
    } else {
        std::tie(frameW, frameH, frameW_off, frameH_off) 
            = calc_position_fixed(source, input, index, isActive);
    }

    // Update source's cache
    source->w = frameW - (padding_ + border_size_) * 2;
    source->h = frameH - (padding_ + border_size_) * 2;
    source->x.store(frameW_off + padding_ + border_size_);
    source->y.store(frameH_off + padding_ + border_size_);

    // Update border filter
    source->bordersFilter = std::unique_ptr<MediaFilter>(new MediaFilter());
    if (!initBorderFilter(*source->bordersFilter.get(),
                          "border",
                          input->format(),
                          source->x.load(),
                          source->y.load(),
                          source->w,
                          source->h,
                          isActive))
        source->bordersFilter.release();
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
    switch(layout_to_draw)
    {
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

        case Layout::ONE_BIG_WITH_SMALL:
        {
            const int max_preview_height = height_ / MIN_LINE_ZOOM;
            const int preview_height = std::min(static_cast<int>(width_ / (n * grid_aspect_)),
                                                max_preview_height);
            const int preview_width = preview_height * grid_aspect_;

            if(index == 0) {
                // In ONE_BIG_WITH_SMALL, the first line at the top is the previews
                // The rest is the active source
                frameH = height_ - preview_height;
                frameW = width_;

                frameW_off = 0;
                frameH_off = preview_height; // First line height
                source->isBig = true;
            }
            else {
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
        case Layout::GRID:
        {
            int rows = 0, columns = 0;
            double source_aspect = (double)width_ / (double)height_;
            
            // calculate optimal grid. Can be cached for optimization
            {
                if(source_aspect < grid_aspect_) { 
                    // vertical alignment
                    rows = 2 * height_ * grid_aspect_ / width_;
                    rows = std::min(rows, n);
                    columns = n / rows;
                }
                else { 
                    // horizontal alignment
                    columns = 2 * width_ / (height_ * grid_aspect_);
                    columns = std::min(columns, n);
                    rows = n / columns;
                }
            }

            if(source_aspect < grid_aspect_) { 
                // vertical alignment
                frameH = height_ / rows;
                frameW = height_ * grid_aspect_;
            }
            else { 
                // horizontal alignment
                frameW = width_ / columns;
                frameH = frameW / grid_aspect_;
            }

            frameH_off = (index / columns) * frameH;
            frameW_off = (index % columns) * frameW;

            // center participants
            frameH_off += (height_ - (rows * frameH)) / 2;
            if(n % columns != 0 && index >= ((columns - 1) * rows)) // if we draw last and not full row
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
                             bool active)
{
    if(border_size_ <= 0)
        return false;

    std::stringstream ss;
    ss << "[" << inputName << "] ";
    ss << "drawbox=x=" << x - (border_size_) << ":y=" << y - (border_size_)
       << ":w=" << width + (border_size_ * 2) << ":h=" << height + (border_size_ * 2)
       << ":color=" << (active ? active_border_color_ : inactive_border_color_)
       << ":t=" << border_size_;

    constexpr auto one = rational<int>(1);
    std::vector<MediaStream> msv;
    msv.emplace_back(inputName, format, one, width, height, 0, one);

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

    // Force coordinate recalculation for all sources,
    // this will trigger updateLayout()
    for (auto& source : sources_) {
        source->w = 0;
        source->h = 0;
        source->isBig = false;
    }
}

} // namespace video
} // namespace sip_core
