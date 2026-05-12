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
    std::unique_ptr<MediaFilter> mainFilter {nullptr};
    std::unique_ptr<MediaFilter> postprocessFilter {nullptr};
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
    std::unique_lock lock(rwMutex_);
    activeStream_ = id;
    updateLayout("setActiveStream");
}

void
VideoMixer::setVoiceActivity(const std::string& streamId, bool state)
{
    std::unique_lock lock(rwMutex_);
    voiceActivity_[streamId] = state;
    updateLayout("setVoiceActivity(single)");
}

void
VideoMixer::setVoiceActivity(const std::map<std::string, bool>& states)
{
    std::unique_lock lock(rwMutex_);
    voiceActivity_ = states;
    updateLayout("setVoiceActivity(map)");
}

void
VideoMixer::setVoiceActivity(const std::map<std::string, bool>&& states)
{
    std::unique_lock lock(rwMutex_);
    voiceActivity_ = std::move(states);
    updateLayout("setVoiceActivity(move)");
}

bool
VideoMixer::moveSource(size_t source_index, size_t dest_index)
{
    std::unique_lock lock(rwMutex_);

    size_t size = sources_.size();
    if (source_index == dest_index || source_index >= size || dest_index >= size)
        return false;

    auto it_from = sources_.begin();
    std::advance(it_from, source_index);

    if (source_index < dest_index) {
        if (dest_index == sources_.size()) {
            sources_.splice(sources_.end(), sources_, it_from);
        } else {
            auto it_to = sources_.begin();
            std::advance(it_to, dest_index);
            sources_.splice(std::next(it_to), sources_, it_from);
        }
    } else { // source > dest
        auto it_to = sources_.begin();
        std::advance(it_to, dest_index);
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
    return voiceActivity_;
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
            if (getHWFrame(std::static_pointer_cast<VideoFrame>(frame_p), frame) < 0) {
                SIP_CORE_ERR("[mixer:%s] VideoFrame::failed to transfer hardware frame", id_.c_str());
                return;
            }
            if(frame)
                x->atomic_copy(*std::static_pointer_cast<VideoFrame>(frame));
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
VideoMixer::getCallDisplayName(const std::unique_ptr<VideoMixer::VideoMixerSource>& source)
{
    std::string name;
    auto& callId = videoToStreamInfo_[source->source].callId;
    if (auto call = Manager::instance().getCallFromCallID(callId)) {
        name = call->getPeerDisplayName();
        if (name.empty()) {
            name = call->getPeerNumber();
        }
        if (name.empty()) {
            return name = "unknown";
        }
        auto found = name.find('@');
        if (found != std::string_view::npos)
            name = name.substr(0, found);

        found = name.find("<sip:");
        if (found != std::string_view::npos)
            name = name.substr(found + 5);
    }
    else return "host";

    return name;
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
        std::shared_lock lock(rwMutex_);

#ifdef RING_ACCEL
        if (accel_) {
            if (auto hw_frame = getHWFrameFromSWFrame(output)) {
                output.copyFrom(*hw_frame);
            }
            else {
                SIP_CORE_ERR("[mixer:%s] VideoFrame::main hardware buffer allocation failed", id_.c_str());
                return;
            }
        }
#endif

        // does current frame is SUCCESSFULLY rendered?
        bool layoutRendered = audioOnlySources_.size() != 0 && sources_.size() == 0;

        // collection of patricipants, both audio & video
        std::vector<SourceInfo> sourcesInfo;
        sourcesInfo.reserve(sources_.size() + audioOnlySources_.size());

        // Build a cache of stream infos to avoid taking videoToStreamInfoMtx_ while holding rwMutex_
        VideoToStream streamInfoCache = getVideoToStreamInfo();

        // Snapshot voice activity to avoid locking vocieActivivtyMtx_ under rwMutex_
        std::map<std::string, bool> voiceActivitySnapshot = voiceActivity_;

        const int pendingLayoutUpdates = layoutUpdated_.load(std::memory_order_acquire);
        bool needsUpdate = pendingLayoutUpdates > 0;
        int layoutUpdatesGenerated = 0;
        bool layoutInvalidated = false;

        int i = 0;
        if(!activeStream_.empty())
            i++; // reserve 0 index place for active stream

        // first, iterate and draw audioOnlySources_
        for (auto& [callId, streamId] : audioOnlySources_) {
            /* thread stop pending? */
            if (!loop_.isRunning())
                return;
                
            std::shared_ptr<VideoFrame> audioFrame = std::make_shared<VideoFrame>();
            audioFrame->reserve(format_, 640, 480);

#ifdef RING_ACCEL
            if (accel_) {
                if (auto hw_frame = getHWFrameFromSWFrame(*audioFrame)) {
                    audioFrame->copyFrom(*hw_frame);
                }
                else {
                    SIP_CORE_ERR("[mixer:%s] VideoFrame::failed to transfer hardware frame", id_.c_str());
                    return;
                }
            }
#endif

            auto audioSource = std::make_unique<VideoMixer::VideoMixerSource>();
            audioSource->hasVideo = false;

            bool voiceActive = false;
            if (auto itVA = voiceActivitySnapshot.find(streamId); itVA != voiceActivitySnapshot.end())
                voiceActive = itVA->second;

            // calc pos, but DO NOT render anything
            if(needsUpdate)
                processSource(audioSource, audioFrame, i, streamId, voiceActive);

            sourcesInfo.emplace_back(SourceInfo {{},
                                                 audioSource->x.load(),
                                                 audioSource->y.load(),
                                                 audioSource->w,
                                                 audioSource->h,
                                                 audioSource->hasVideo,
                                                 callId,
                                                 streamId});
            i++;
        }

        // add video sources
        for (auto& x : sources_) {
            /* thread stop pending? */
            if (!loop_.isRunning())
                return;

            if (x->w == 0 || x->h == 0)
                needsUpdate;

            StreamInfo sinfo = {};
            if (auto itSI = streamInfoCache.find(x->source); itSI != streamInfoCache.end())
                sinfo = itSI->second;

            bool voiceActive = false;
            if (auto itVA = voiceActivitySnapshot.find(sinfo.streamId);
                itVA != voiceActivitySnapshot.end())
                voiceActive = itVA->second;

            // make rendered frame temporarily unavailable for update()
            // to avoid concurrent access.
            std::shared_ptr<VideoFrame> input = x->getRenderFrame();
            if (input->height() and input->width()) {
                if (input->width() != x->lastLayoutFrameWidth
                        || input->height() != x->lastLayoutFrameHeight
                        || input->getOrientation() != x->lastLayoutOrientation)
                {
                    needsUpdate = true;
                    x->lastLayoutFrameWidth = input->width();
                    x->lastLayoutFrameHeight = input->height();
                    x->lastLayoutOrientation = input->getOrientation();
                }
            }

            if(needsUpdate)
                processSource(x, input, i, sinfo.streamId, voiceActive);

            bool frameRendered = false;
            if (input and input->height() and input->width()) {
                    frameRendered = render_frame(output, input, x, needsUpdate);
                    layoutRendered |= frameRendered;
            }
            else
                SIP_CORE_WARN("[mixer:%s] Nothing to render for %p", id_.c_str(), x->source);

            if (frameRendered != x->hasVideo) {
                x->hasVideo = frameRendered;
                layoutInvalidated = true;
            }

            sourcesInfo.emplace_back(SourceInfo {x->source,
                                                 x->x.load(),
                                                 x->y.load(),
                                                 x->w,
                                                 x->h,
                                                 x->hasVideo,
                                                 sinfo.callId,
                                                 sinfo.streamId});
            
            ++i;
        }

        if (needsUpdate && layoutRendered && !layoutInvalidated) {
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

#ifdef RING_ACCEL
        auto frame = getUnlinkedHWFrame(output);
        if (frame) {
            output.copyFrom(*frame.get());
        }
#endif
    publishFrame();
}

void
VideoMixer::processSource(std::unique_ptr<VideoMixer::VideoMixerSource>& source,
                          const std::shared_ptr<VideoFrame> frame,
                          int& i,
                          const std::string& streamId,
                          bool isVoiceActive)
{
    // set "wantedIndex" to current index of video source, for GRID layout
    auto wantedIndex = i;
    if(currentLayout_ == Layout::ONE_BIG) {
        // show active stream FIRST
        if (activeStream_ == streamId) {
            wantedIndex = 0;
            i--; // negilate i++ further
        }
        else {
            source->x.store(0);
            source->y.store(0);
            source->w = 0;
            source->h = 0;
            source->hasVideo = false;
        }
    }
    else {
        if (currentLayout_ == Layout::ONE_BIG_WITH_SMALL && activeStream_ == streamId) {
            wantedIndex = 0;
            i--; // negilate i++ further
        }
    }
    
    calc_position(source, frame, wantedIndex, isVoiceActive);
}

bool
VideoMixer::render_frame(VideoFrame& output,
                         const std::shared_ptr<VideoFrame>& input,
                         std::unique_ptr<VideoMixerSource>& source,
                         bool positionChanged)
{
    if (!width_ or !height_ or !input->pointer() or input->pointer()->format == -1)
        return false;

#ifdef RING_ACCEL
    std::lock_guard lock(accelMtx_);
    if(accel_) {
        if(source->mainFilter) {
            source->mainFilter->feedInput(input->pointer(), "overlay");
            output.pointer()->pts = input->pointer()->pts; // for correct framesync
            source->mainFilter->feedInput(output.pointer(), "main");
            std::unique_ptr<MediaFrame> clone = source->mainFilter->readOutput();
            if(clone.get())
                output.copyFrom(*std::static_pointer_cast<VideoFrame>(
                                    std::shared_ptr<MediaFrame>(clone.release())));
            // else {
            //     enableAccel_ = false;
            //     accel_.reset();
            //     source->mainFilter.release();
            //     return false;
            // }
        }
    } else {
#endif
        int cell_width = source->w;
        int cell_height = source->h;
        int xoff = source->x.load();
        int yoff = source->y.load();

        int angle = input->getOrientation();
        const constexpr char filterIn[] = "mixin";
        if (angle != source->rotation || positionChanged) {
            // calculate width and height for cropping
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
            source->mainFilter = video::getTransposeFilterWithCrop(filterIn,
                                                                        angle,
                                                                        width,
                                                                        height,
                                                                        input->format());
            source->rotation = angle;
        }
        std::shared_ptr<VideoFrame> frame;
        if (source->mainFilter) {
            source->mainFilter->feedInput(input->pointer(), filterIn);
            frame = std::static_pointer_cast<VideoFrame>(
                std::shared_ptr<MediaFrame>(source->mainFilter->readOutput()));
        } else {
            frame = input;
        }
        if (frame)
            scaler_.scale_and_pad(*frame, output, xoff, yoff, cell_width, cell_height, true);

        if (source->postprocessFilter) {
            source->postprocessFilter->feedInput(output.pointer(), borderFilterName_);
            std::unique_ptr<MediaFrame> clone = source->postprocessFilter->readOutput();
            if (clone.get())
                output.copyFrom(*std::static_pointer_cast<VideoFrame>(
                    std::shared_ptr<MediaFrame>(clone.release())));
        }

#ifdef RING_ACCEL
    }
#endif

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


#ifdef RING_ACCEL
    std::lock_guard lock(accelMtx_);
    if(accel_) {
        if (!source->w || !source->h)
            return;

        std::string display = getCallDisplayName(source);
        source->mainFilter = std::unique_ptr<MediaFilter>(new MediaFilter());
        if(!initMainFilterHardware(*source->mainFilter,
                                   display,
                                   input->format(),
                                   source->x.load(),
                                   source->y.load(),
                                   source->w,
                                   source->h,
                                   input->getOrientation(),
                                   remove_black_borders_ && not source->isBig,
                                   isActive))
        {
            this->enableAccel_ = false;
            accel_.reset();
            source->mainFilter.release();
        }
    }
    else {
#endif
    // Update border filter
    std::string display = getCallDisplayName(source);
    source->postprocessFilter = std::unique_ptr<MediaFilter>(new MediaFilter());
    if (initBorderFilterSoftware(*source->postprocessFilter.get(),
                                 display,
                                 input->format(),
                                 source->x.load(),
                                 source->y.load(),
                                 source->w,
                                 source->h,
                                 isActive))
        return;

    // If textdraw failed - try without it...
    source->postprocessFilter.reset(new MediaFilter());
    if (!initBorderFilterSoftware(*source->postprocessFilter.get(),
                                  "",
                                  input->format(),
                                  source->x.load(),
                                  source->y.load(),
                                  source->w,
                                  source->h,
                                  isActive))
        source->postprocessFilter.release();
#ifdef RING_ACCEL
    }
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
            bool isVerticalAlign = source_aspect < grid_aspect_;
            {
                if(isVerticalAlign) { 
                    // vertical alignment
                    rows = 2 * height_ * grid_aspect_ / width_;
                    rows = std::min(rows, n);
                    columns = ((n + 1) / rows) + 1;
                    if(columns == 1)
                        isVerticalAlign = !isVerticalAlign;
                }
                else { 
                    // horizontal alignment
                    columns = 2 * width_ / (height_ * grid_aspect_);
                    columns = std::min(columns, n);
                    rows = ((n - 1) / columns) + 1;
                    if(rows == 1)
                        isVerticalAlign = !isVerticalAlign;
                }
            }

            if(isVerticalAlign) { 
                frameW = width_ / columns;
                frameH = frameW / grid_aspect_;
            }
            else { 
                frameH = height_ / rows;
                frameW = frameH * grid_aspect_;
            }

            frameH_off = (index / columns) * frameH;
            frameW_off = (index % columns) * frameW;

            // center participants
            frameH_off += (height_ - (rows * frameH)) / 2;
            if(n % columns != 0 && index >= (columns * (rows - 1))) // if we draw last and not full row
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
VideoMixer::initBorderFilterSoftware(MediaFilter& filter,
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
    ss << "[" << borderFilterName_ << "] ";
    ss << "drawbox=x=" << x - (border_size_) << ":y=" << y - (border_size_)
       << ":w=" << width + (border_size_ * 2) << ":h=" << height + (border_size_ * 2)
       << ":color=" << (active ? active_border_color_ : inactive_border_color_)
       << ":t=" << border_size_;

    if(!inputName.empty()) { 
        const int text_height = height / 15;
        constexpr int text_padding = 10;
        ss << ",drawtext=text='" << inputName << "'"
           << ":fontcolor=white:fontsize=" << text_height
           << ":x=" << x << "+(" << width << "-text_w)/2"
           << ":y=" << y << "+" << height - text_padding << "-text_h";
    }

    constexpr auto one = rational<int>(1);
    std::vector<MediaStream> msv;
    msv.emplace_back(borderFilterName_, format, one, width, height, 0, one);

    auto ret = filter.initialize(ss.str(), msv);
    if (ret < 0) {
        SIP_CORE_ERR() << "filter init fail";
        return false;
    }

    return true;
}

#ifdef RING_ACCEL
bool
VideoMixer::initMainFilterHardware(MediaFilter& filter,
                                   std::string inputName,
                                   int format,
                                   int x,
                                   int y,
                                   int w,
                                   int h,
                                   int dir,
                                   bool remove_borders,
                                   bool active)
{
    std::stringstream ss;
    ss << " [main][overlay]";
    ss << "sv_participant_opencl=x=" << x << ":y=" << y 
                                     << ":width=" << w << ":height=" << h
                                     << ":b_width=" << border_size_ 
                                     << ":b_color=" << (active ? active_border_color_ : inactive_border_color_);

    if(!inputName.empty()) { 
        const int text_height = h / 15;
        constexpr int text_padding = 10;
        ss << ":text='" << inputName << "'"
           << ":fontcolor=white:fontsize=" << text_height
           << ":text_x=(" << w << "-text_w)/2"
           << ":text_y=" << h - text_padding << "-text_h";
    }

    switch (dir) {
    case 0: break;
    case 90:
    case -270:
        ss << ":dir=2";
        break;
    case 180:
    case -180:
        ss << ":dir=6";
        break;
    case 270:
    case -90:
        ss << ":dir=1";
        break;
    default:
        SIP_CORE_WARN("Unsupported rotation value");
    }
    
    if (remove_borders)
        ss << ":no_black_fields=1";

    constexpr auto one = rational<int>(1);
    std::vector<MediaStream> msv;
    msv.emplace_back("main", AV_PIX_FMT_OPENCL, one, width_, height_, 0, one);
    accel_->linkFilter(msv.back(), width_, height_);
    msv.emplace_back("overlay", AV_PIX_FMT_OPENCL, one, w, h, 0, one);
    accel_->linkFilter(msv.back(), w, h);
    auto ret = filter.initialize(ss.str(), msv);
    if (ret < 0) {
        for (auto m : msv) {
            av_buffer_unref(&m.deviceRef);
            av_buffer_unref(&m.frameRef);
        }
        SIP_CORE_ERR() << "filter init fail";
        return false;
    }

    return true;
}

int
VideoMixer::getHWFrame(const std::shared_ptr<VideoFrame>& input, std::shared_ptr<VideoFrame>& output)
{
    std::lock_guard lock(accelMtx_);
#if !defined(__APPLE__) && defined(RING_ACCEL)
    try {
        auto desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(input->format()));
        bool isHardware = desc && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL);
        if (accel_ && accel_->isLinked() && isHardware) {
            // Fully accelerated pipeline, skip main memory
            output = input;
        } else if (isHardware) {
            // Hardware decoded frame, transfer back to main memory
            // Transfer to GPU if we have a hardware encoder
            // Hardware decoders decode to NV12, but sip_core's supported software encoders want YUV420P
            output = getUnlinkedHWFrame(*input.get());
        } else if (accel_) {
            // Software decoded frame with a hardware encoder, convert to accepted format first
            output = getHWFrameFromSWFrame(*input.get());
        } else {
            output = input;
        }
    } catch (const std::runtime_error& e) {
        SIP_CORE_ERR("Accel failure: %s", e.what());
        return -1;
    }
#else
        // macOS
        output = input;
#endif

        return 0;
}

std::shared_ptr<VideoFrame>
VideoMixer::getUnlinkedHWFrame(const VideoFrame& input)
{
    std::shared_ptr<VideoFrame> framePtr;
    if (!accel_) {
        std::lock_guard<std::mutex> lock(scaler_mutex_);
        framePtr = scaler_.convertFormat(input, format_);
    } else {
        framePtr = accel_->transfer(input);
    }
    return framePtr;
}

std::shared_ptr<VideoFrame>
VideoMixer::getHWFrameFromSWFrame(const VideoFrame& input)
{
    std::shared_ptr<VideoFrame> framePtr;
    auto pix = accel_->getSoftwareFormat();
    if (input.format() != pix) {
        std::lock_guard<std::mutex> lock(scaler_mutex_);
        framePtr = scaler_.convertFormat(input, pix);
        framePtr = accel_->transfer(*framePtr);
    } else {
        framePtr = accel_->transfer(input);
    }
    return framePtr;
}
#endif
    
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
#ifdef RING_ACCEL
    enableAccel_= params.useHardware;
#endif

    // cleanup the previous frame to have a nice copy in rendering method
    std::shared_ptr<VideoFrame> previous_p(obtainLastFrame());
    if (previous_p)
        libav_utils::fillWithBlack(previous_p->pointer());

#ifdef RING_ACCEL
    std::lock_guard lock_accel(accelMtx_);
    bool enabled = enableAccel_.load();
    enabled &= Manager::instance().videoPreferences.getDecodingAccelerated();
    enableAccel_.store(enabled);
    if(enableAccel_) {
        auto apiList = HardwareAccel::getCompatibleAccel(AV_CODEC_ID_NONE, width_, height_, CODEC_NONE);
        for (const auto& api : apiList) {
            accel_ = std::make_unique<video::HardwareAccel>(api);
            if (accel_->initAPI(false, nullptr) < 0) {
                accel_.reset();
                continue;
            }
            break;
        }
    }
#endif

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
