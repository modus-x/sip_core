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
#ifdef RING_ACCEL
#include "accel.h"
#endif

#include <list>
#include <chrono>
#include <memory>
#include <shared_mutex>
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

class VideoMixer : public VideoGenerator, public VideoFramePassiveReader
{
    using VideoToStream = std::map<Observable<std::shared_ptr<MediaFrame>>*, StreamInfo>;
public:
    struct Parameters
    {
        int width;
        int height;
        AVPixelFormat format {AV_PIX_FMT_YUV422P};
        double grid_aspect {1.}; // = 0 to match mixer aspect
        int padding {5};
        int border_size {8};
        std::string active_border_color {"CornflowerBlue@1"}; // ffmpeg compatible colors only
        std::string inactive_border_color {"Blue@1"};         // ffmpeg compatible colors only
        bool remove_black_borders {true};
    #ifdef RING_ACCEL
        bool useHardware { true };
    #endif
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
    void switchInputs(const std::vector<std::string>& inputs);

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

    void addAudioOnlySource(const std::string& callId, const std::string& streamId)
    {
        std::unique_lock lock(rwMutex_);
        audioOnlySources_.insert({callId, streamId});
        updateLayout();
    }

    void removeAudioOnlySource(const std::string& callId, const std::string& streamId)
    {
        std::unique_lock lock(rwMutex_);
        if (audioOnlySources_.erase({callId, streamId})) {
            updateLayout();
        }
    }

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
                       bool isActive);

    gripRect calc_position_rel(std::unique_ptr<VideoMixerSource>& source,
                       const std::shared_ptr<VideoFrame>& input,
                       int index,
                       bool isActive);
    
    gripRect calc_position_fixed(std::unique_ptr<VideoMixerSource>& source,
                       const std::shared_ptr<VideoFrame>& input,
                       int index,
                       bool isActive);

    bool initBorderFilterSoftware(MediaFilter& filter,
                                  std::string inputName,
                                  int format,
                                  int x,
                                  int y,
                                  int width,
                                  int height,
                                  bool active);
#ifdef RING_ACCEL
    bool initMainFilterHardware(MediaFilter& filter,
                               std::string inputName,
                               int format,
                               int x,
                               int y,
                               int w,
                               int h,
                               int dir,
                               bool remove_borders,
                               bool active);

    int getHWFrame(const std::shared_ptr<VideoFrame>& input, std::shared_ptr<VideoFrame>& output);
    std::shared_ptr<VideoFrame> getUnlinkedHWFrame(const VideoFrame& input);
    std::shared_ptr<VideoFrame> getHWFrameFromSWFrame(const VideoFrame& input);
    video::HardwareAccel* initHWAccel();
#endif

    int addLayoutUpdate(const char* reason);
    void consumeLayoutUpdates(int count, const char* reason);

    void startSink();
    void stopSink();

    void process();
    void processSource(std::unique_ptr<VideoMixer::VideoMixerSource>& source,
                       const std::shared_ptr<VideoFrame> frame,
                       int& i,
                       const std::string& streamId,
                       bool isVoiceActive);

    // Process any pending observer detaches in a safe context
    void processPendingDetaches();

    // Enqueue an observable to be detached from sources_ without blocking
    void enqueueDetach(Observable<std::shared_ptr<MediaFrame>>* ob);

    std::string getCallDisplayName(const std::unique_ptr<VideoMixer::VideoMixerSource>& source);

    const std::string id_;
    int width_ = 0;
    int height_ = 0;
    AVPixelFormat format_ = AV_PIX_FMT_YUV422P;
    double grid_aspect_ {1.};
    int padding_ {5};
    const std::string borderFilterName_ = "border";
    int border_size_ {8};
    std::string active_border_color_ {"CornflowerBlue@1"}; // ffmpeg declared colors only
    std::string inactive_border_color_ {"Blue@1"};         // ffmpeg declared colors only
    bool remove_black_borders_ {true};
    std::shared_mutex rwMutex_;

    std::shared_ptr<SinkClient> sink_;

    std::chrono::time_point<std::chrono::steady_clock> nextProcess_;
    std::mutex localInputsMtx_;
    std::vector<std::shared_ptr<VideoInput>> localInputs_ {};
    void stopInput(const std::shared_ptr<VideoFrameActiveWriter>& input);

    std::mutex scaler_mutex_;
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

    // pair streamId -> activity state
    std::map<std::string, bool> voiceActivity_;

    // pair callId -> streamId
    // in case of local participant, it will be empty
    std::set<std::pair<std::string, std::string>> audioOnlySources_;
    std::string activeStream_ {};

    std::atomic_int layoutUpdated_ {0};
    OnSourcesUpdatedCb onSourcesUpdated_ {};

    int64_t startTime_;
    int64_t lastTimestamp_;

#ifdef RING_ACCEL
    std::atomic_bool enableAccel_ = true;
    bool fallback_ = false;
    const std::string hardwareScaleAndPadFilterName_ = "SnP";
    std::unique_ptr<video::HardwareAccel> accel_ = nullptr;
    std::mutex accelMtx_;
#endif
};

} // namespace video
} // namespace sip_core
