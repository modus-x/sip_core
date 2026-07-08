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

#pragma once

#include "noncopyable.h"
#include "video_base.h"
#include "video_scaler.h"
#include "video_input.h"
#include "threadloop.h"
#include "media_stream.h"
#include "media_filter.h"

#include <list>
#include <chrono>
#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <vector>
#include <tuple>

namespace sip_core {
namespace video {

class SinkClient;

struct StreamInfo
{
    std::string callId;
    std::string streamId;
};

struct SourceInfo
{
    Observable<std::shared_ptr<MediaFrame>>* source;
    int x;
    int y;
    int w;
    int h;
    bool hasVideo;
    std::string callId;
    std::string streamId;
};
using OnSourcesUpdatedCb = std::function<void(std::vector<SourceInfo>&&)>;

enum class Layout { GRID, ONE_BIG_WITH_SMALL, ONE_BIG };

struct AudioOnlySource
{
    std::string callId;
    std::string streamId;
    std::string overlayLabel;
};

class VideoMixer : public VideoGenerator, public VideoFramePassiveReader
{
    using VideoToStream = std::map<Observable<std::shared_ptr<MediaFrame>>*, StreamInfo>;
    using AudioOnlySourceKey = std::pair<std::string, std::string>;
    using AudioOnlySources = std::map<AudioOnlySourceKey, AudioOnlySource>;

public:
    struct Parameters
    {
        int width;
        int height;
        AVPixelFormat format {AV_PIX_FMT_YUV422P};
        double grid_aspect {16.0 / 9.0}; // rectangular tiles; = 0 to match mixer aspect
        // Tight, Teams-like spacing: every tile is inset by (padding+border_size)
        // on each side (see VideoMixer::calc_position), so the inter-tile gap is
        // 2*(padding+border_size) and the outer margin is (padding+border_size).
        // These match the Flutter SPLITTED gallery tiles (1px padding + 3px
        // active-speaker border) so the composited/MIXED stream reads the same as
        // the split view. border_size also sets the baked active-speaker frame
        // thickness (drawbox t=). Raise them for a looser grid / bolder frame.
        int padding {1};
        int border_size {3};
        std::string active_border_color {"CornflowerBlue@1"}; // ffmpeg compatible colors only
        std::string inactive_border_color {"Blue@0"};         // invisible: only the active speaker is framed
        bool remove_black_borders {true};
        int voice_inactive_hold_ms {500};
    };

    VideoMixer(const std::string& id, const std::string& localInput = {}, bool attachHost = true);
    ~VideoMixer();

    void setParameters(const Parameters& params);

    int getWidth() const override;
    int getHeight() const override;
    AVPixelFormat getPixelFormat() const override;

    // as VideoFramePassiveReader (OBSERVER)
    void update(Observable<std::shared_ptr<MediaFrame>>* ob,
                const std::shared_ptr<MediaFrame>& v) override;
    void attached(Observable<std::shared_ptr<MediaFrame>>* ob) override;
    void detached(Observable<std::shared_ptr<MediaFrame>>* ob) override;

    /**
     * Set all inputs at once
     * @param inputs        New inputs
     * @note previous inputs will be stopped, new inputs won't be automatically turned on.
     * until these inputs are not attached, black frames will be sent
     */
    void switchInputs(const std::vector<std::string>& inputs, bool muted = false);

    /**
     * Stop all inputs
     */
    void stopInputs();

    void startInputs();

    void muteInputs(bool mute);

    void setActiveStream(const std::string& id);
    void resetActiveStream()
    {
        std::unique_lock lock(rwMutex_);
        activeStream_ = {};
        updateLayout();
    }

    void setVoiceActivity(const std::string& streamId, bool state);
    void setVoiceActivity(const std::map<std::string, bool>& states);
    void setVoiceActivity(const std::map<std::string, bool>&& states);
    void setVoiceInactiveHoldMs(int holdMs);

    bool hasActive()
    {
        std::shared_lock lock(rwMutex_);
        return !activeStream_.empty();
    }

    bool verifyActive(const std::string& id)
    {
        std::shared_lock lock(rwMutex_);
        return activeStream_ == id;
    }

    bool moveSource(size_t from_index, size_t to_index);

    void setVideoLayout(Layout newLayout);

    Layout getVideoLayout()
    {
        std::shared_lock lk(rwMutex_);
        return currentLayout_;
    }

    void setOnSourcesUpdated(OnSourcesUpdatedCb&& cb)
    {
        std::unique_lock lk(rwMutex_);
        onSourcesUpdated_ = std::move(cb);
    }

    MediaStream getStream(const std::string& name) const;

    std::shared_ptr<VideoFrameActiveWriter> getVideoLocal() const
    {
        if (!localInputs_.empty())
            return *localInputs_.begin();
        return {};
    }

    void updateLayout(const char* reason = "updateLayout()");

    std::shared_ptr<SinkClient>& getSink() { return sink_; }

    // Definitions live in video_mixer.cpp because they touch
    // audioOnlyRenderSources_, whose value type std::unique_ptr<VideoMixerSource>
    // requires the complete (private) VideoMixerSource definition for its
    // destructor.
    void addAudioOnlySource(const std::string& callId,
                            const std::string& streamId,
                            const std::string& overlayLabel = {});

    void removeAudioOnlySource(const std::string& callId, const std::string& streamId);

    void attachVideo(Observable<std::shared_ptr<MediaFrame>>* frame,
                     const std::string& callId,
                     const std::string& streamId);
    void detachVideo(Observable<std::shared_ptr<MediaFrame>>* frame);

    StreamInfo streamInfo(Observable<std::shared_ptr<MediaFrame>>* frame) const
    {
        std::lock_guard<std::mutex> lk(videoToStreamInfoMtx_);
        auto it = videoToStreamInfo_.find(frame);
        if (it == videoToStreamInfo_.end())
            return {};
        return it->second;
    }

protected:
    VideoToStream getVideoToStreamInfo() const;
    std::map<std::string, bool> getVoiceActivity();

private:
    NON_COPYABLE(VideoMixer);
    struct VideoMixerSource;
    using gripRect = std::tuple<int, int, int, int>;

    bool render_frame(VideoFrame& output,
                      const std::shared_ptr<VideoFrame>& input,
                      std::unique_ptr<VideoMixerSource>& source,
                      bool positionChanged);

    void calc_position(std::unique_ptr<VideoMixerSource>& source,
                       const std::shared_ptr<VideoFrame>& input,
                       int index,
                       bool isActive,
                       const std::string& callId = {});

    gripRect calc_position_rel(std::unique_ptr<VideoMixerSource>& source,
                               const std::shared_ptr<VideoFrame>& input,
                               int index,
                               bool isActive);

    gripRect calc_position_fixed(std::unique_ptr<VideoMixerSource>& source,
                                 const std::shared_ptr<VideoFrame>& input,
                                 int index,
                                 bool isActive);

    bool initBorderFilter(MediaFilter& filter,
                          std::string inputName,
                          int format,
                          int x,
                          int y,
                          int width,
                          int height,
                          bool active,
                          bool withText);

    int addLayoutUpdate(const char* reason);
    void consumeLayoutUpdates(int count, const char* reason);
    void applyVoiceActivityStateLocked(const std::string& streamId,
                                       bool state,
                                       std::chrono::steady_clock::time_point now,
                                       bool& layoutChanged);
    void removeStaleVoiceStatesLocked(const std::map<std::string, bool>& states,
                                      bool& layoutChanged);
    bool expireVoiceHoldsLocked(std::chrono::steady_clock::time_point now);
    static int clampVoiceInactiveHoldMs(int holdMs);

    void startSink();
    void stopSink();

    void process();
    void processSource(std::unique_ptr<VideoMixer::VideoMixerSource>& source,
                       const std::shared_ptr<VideoFrame> frame,
                       int& i,
                       const std::string& streamId,
                       bool isVoiceActive,
                       const std::string& callId = {});

    // Process any pending observer detaches in a safe context
    void processPendingDetaches();

    // Enqueue an observable to be detached from sources_ without blocking
    void enqueueDetach(Observable<std::shared_ptr<MediaFrame>>* ob);

    std::string getCallDisplayName(const std::unique_ptr<VideoMixer::VideoMixerSource>& source,
                                   const std::string& fallbackCallId = {});

    const std::string id_;
    int width_ = 0;
    int height_ = 0;
    AVPixelFormat format_ = AV_PIX_FMT_YUV422P;
    double grid_aspect_ {16.0 / 9.0};
    int padding_ {1}; // kept in sync with Parameters::padding (Teams-tight gaps)
    const std::string borderFilterName_ = "border";
    int border_size_ {3}; // kept in sync with Parameters::border_size
    std::string active_border_color_ {"CornflowerBlue@1"}; // ffmpeg declared colors only
    std::string inactive_border_color_ {"Blue@0"};         // invisible: only the active speaker is framed
    bool remove_black_borders_ {true};
    std::shared_mutex rwMutex_;

    std::shared_ptr<SinkClient> sink_;

    std::chrono::time_point<std::chrono::steady_clock> nextProcess_;
    std::mutex localInputsMtx_;
    std::vector<std::shared_ptr<VideoInput>> localInputs_ {};
    /// When true, sources created by attached() start in muted state.
    /// Set by switchInputs(muted=true) around the startInputs() call.
    std::atomic<bool> nextLocalSourceMuted_ {false};
    void stopInput(const std::shared_ptr<VideoFrameActiveWriter>& input);

    VideoScaler scaler_;

    ThreadLoop loop_; // as to be last member

    Layout currentLayout_ {Layout::GRID};
    std::list<std::unique_ptr<VideoMixerSource>> sources_;

    // We need to convert call to frame
    mutable std::mutex videoToStreamInfoMtx_ {};
    VideoToStream videoToStreamInfo_ {};

    // Queue of observables pending removal to avoid locking in callbacks
    std::mutex pendingDetachMtx_ {};
    std::vector<Observable<std::shared_ptr<MediaFrame>>*> pendingDetaches_ {};

    // pair streamId -> raw voice activity state
    std::map<std::string, bool> voiceActivityRaw_;
    // pair streamId -> effective display activity state
    std::map<std::string, bool> voiceActivityDisplay_;
    // pair streamId -> inactive deadline while applying hold
    std::map<std::string, std::chrono::steady_clock::time_point> voiceInactiveDeadlines_;
    int voiceInactiveHoldMs_ {500};

    AudioOnlySources audioOnlySources_;
    // Persistent render-side state for each audio-only placeholder so that
    // VideoMixer::process() can cache geometry/border filter across frames
    // and only recompute when the layout actually changes (mirrors the
    // needsUpdate gating used for video sources).
    std::map<AudioOnlySourceKey, std::unique_ptr<VideoMixerSource>> audioOnlyRenderSources_;
    std::string activeStream_ {};

    // Stable source ordering: maps streamId -> first-seen insertion index.
    // Survives detach/reattach cycles so grid positions stay consistent.
    std::unordered_map<std::string, int> stableOrder_;
    int nextStableIndex_ {0};

    std::atomic_int layoutUpdated_ {0};
    OnSourcesUpdatedCb onSourcesUpdated_ {};

    int64_t startTime_;
    int64_t lastTimestamp_;

    // Display-name cache populated once per frame in process() *before* rwMutex_
    // to avoid calling Manager::getCallFromCallID() under the shared lock.
    // Keyed by callId. Only accessed from the mixer thread.
    std::unordered_map<std::string, std::string> displayNameCache_;
};

} // namespace video
} // namespace sip_core
