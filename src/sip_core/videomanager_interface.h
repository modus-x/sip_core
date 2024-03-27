/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
 *  Author: Guillaume Roguez <guillaume.roguez@savoirfairelinux.com>
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

#ifndef DENABLE_VIDEOMANAGERI_H
#define DENABLE_VIDEOMANAGERI_H

#include "sip_core.h"
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif // HAVE_CONFIG_H

extern "C" {
struct AVFrame;
struct AVPacket;
void av_frame_free(AVFrame** frame);
void av_packet_free(AVPacket** frame);
}

#include "def.h"

#include <memory>
#include <vector>
#include <map>
#include <string>
#include <functional>
#include <chrono>
#include <cstdint>
#include <cstdlib>

#ifdef __APPLE__
#import "TargetConditionals.h"
#endif

#ifdef __ANDROID__
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#endif

namespace sip_core {
struct AudioFormat;
}

namespace libsip_core {

[[deprecated("Replaced by registerSignalHandlers")]] LIBSIP_CORE_PUBLIC void registerVideoHandlers(
    const std::map<std::string, std::shared_ptr<CallbackWrapperBase>>&);

struct LIBSIP_CORE_PUBLIC AVFrame_deleter
{
    void operator()(AVFrame* frame) const { av_frame_free(&frame); }
};

typedef std::unique_ptr<AVFrame, AVFrame_deleter> FrameBuffer;

struct LIBSIP_CORE_PUBLIC AVPacket_deleter
{
    void operator()(AVPacket* pkt) const { av_packet_free(&pkt); }
};

typedef std::unique_ptr<AVPacket, AVPacket_deleter> PacketBuffer;

class LIBSIP_CORE_PUBLIC MediaFrame
{
public:
    // Construct an empty MediaFrame
    MediaFrame();
    MediaFrame(const MediaFrame&) = delete;
    MediaFrame& operator=(const MediaFrame& o) = delete;
    MediaFrame(MediaFrame&& o) = delete;
    MediaFrame& operator=(MediaFrame&& o) = delete;

    virtual ~MediaFrame() = default;

    // Return a pointer on underlaying buffer
    const AVFrame* pointer() const noexcept { return frame_.get(); }
    AVFrame* pointer() noexcept { return frame_.get(); }
    AVPacket* packet() const noexcept { return packet_.get(); }

    // Fill this MediaFrame with data from o
    void copyFrom(const MediaFrame& o);
    void setPacket(PacketBuffer&& pkt);

    // Reset internal buffers (return to an empty MediaFrame)
    virtual void reset() noexcept;

    FrameBuffer getFrame() { return std::move(frame_); }

protected:
    FrameBuffer frame_;
    PacketBuffer packet_;
};

class LIBSIP_CORE_PUBLIC AudioFrame : public MediaFrame
{
public:
    AudioFrame()
        : MediaFrame()
    {}
    AudioFrame(const sip_core::AudioFormat& format, size_t nb_samples = 0);
    ~AudioFrame() {};
    void mix(const AudioFrame& o);
    float calcRMS() const;
    sip_core::AudioFormat getFormat() const;
    size_t getFrameSize() const;
    bool has_voice {false};

private:
    void setFormat(const sip_core::AudioFormat& format);
    void reserve(size_t nb_samples = 0);
};

class LIBSIP_CORE_PUBLIC VideoFrame : public MediaFrame
{
public:
    // Construct an empty VideoFrame
    VideoFrame()
        : MediaFrame()
    {}
    ~VideoFrame();

    // Reset internal buffers (return to an empty VideoFrame)
    void reset() noexcept override;

    // Fill this VideoFrame with data from o
    void copyFrom(const VideoFrame& o);

    // Return frame size in bytes
    std::size_t size() const noexcept;

    // Return pixel format
    int format() const noexcept;

    // Return frame width in pixels
    int width() const noexcept;

    // Return frame height in pixels
    int height() const noexcept;

    // Allocate internal pixel buffers following given specifications
    void reserve(int format, int width, int height);

    // Return orientation (in degrees) stored in the frame metadata, or 0 by default.
    int getOrientation() const;

    // Set internal pixel buffers on given memory buffer
    // This buffer must follow given specifications.
    void setFromMemory(uint8_t* data, int format, int width, int height) noexcept;
    void setFromMemory(uint8_t* data,
                       int format,
                       int width,
                       int height,
                       const std::function<void(uint8_t*)>& cb) noexcept;
    void setReleaseCb(const std::function<void(uint8_t*)>& cb) noexcept;

    void noise();

private:
    std::function<void(uint8_t*)> releaseBufferCb_ {};
    uint8_t* ptr_ {nullptr};
    bool allocated_ {false};
    void setGeometry(int format, int width, int height) noexcept;
};

struct LIBSIP_CORE_PUBLIC SinkTarget
{
    std::function<FrameBuffer()> pull;
    std::function<void(FrameBuffer)> push;
    int /* AVPixelFormat */ preferredFormat {-1 /* AV_PIX_FMT_NONE */};
};

using VideoCapabilities = std::map<std::string, std::map<std::string, std::vector<std::string>>>;

LIBSIP_CORE_PUBLIC std::vector<std::string> getDeviceList();
LIBSIP_CORE_PUBLIC VideoCapabilities getCapabilities(const std::string& deviceId);
LIBSIP_CORE_PUBLIC std::map<std::string, std::string> getSettings(const std::string& deviceId);
LIBSIP_CORE_PUBLIC void applySettings(const std::string& deviceId,
                                      const std::map<std::string, std::string>& settings);
LIBSIP_CORE_PUBLIC void setDefaultDevice(const std::string& deviceId);
LIBSIP_CORE_PUBLIC void setDeviceOrientation(const std::string& deviceId, int angle);
LIBSIP_CORE_PUBLIC std::map<std::string, std::string> getDeviceParams(const std::string& deviceId);
LIBSIP_CORE_PUBLIC std::string getDefaultDevice();
LIBSIP_CORE_PUBLIC void startAudioDevice();
LIBSIP_CORE_PUBLIC void stopAudioDevice();

LIBSIP_CORE_PUBLIC std::string openVideoInput(const std::string& path);
LIBSIP_CORE_PUBLIC bool closeVideoInput(const std::string& id);

LIBSIP_CORE_PUBLIC std::string createMediaPlayer(const std::string& path);
LIBSIP_CORE_PUBLIC bool closeMediaPlayer(const std::string& id);
LIBSIP_CORE_PUBLIC bool pausePlayer(const std::string& id, bool pause);
LIBSIP_CORE_PUBLIC bool mutePlayerAudio(const std::string& id, bool mute);
LIBSIP_CORE_PUBLIC bool playerSeekToTime(const std::string& id, int time);
int64_t getPlayerPosition(const std::string& id);

LIBSIP_CORE_PUBLIC bool registerSinkTarget(const std::string& sinkId, SinkTarget target);
#ifdef ENABLE_SHM
LIBSIP_CORE_PUBLIC void startShmSink(const std::string& sinkId, bool value);
#endif
LIBSIP_CORE_PUBLIC std::map<std::string, std::string> getRenderer(const std::string& callId);

LIBSIP_CORE_PUBLIC std::string startLocalMediaRecorder(const std::string& videoInputId,
                                                       const std::string& filepath);
LIBSIP_CORE_PUBLIC void stopLocalRecorder(const std::string& filepath);

#if defined(__ANDROID__) || (defined(TARGET_OS_IOS) && TARGET_OS_IOS)
LIBSIP_CORE_PUBLIC void addVideoDevice(
    const std::string& node, const std::vector<std::map<std::string, std::string>>& devInfo = {});
LIBSIP_CORE_PUBLIC void removeVideoDevice(const std::string& node);
LIBSIP_CORE_PUBLIC VideoFrame* getNewFrame(std::string_view id);
LIBSIP_CORE_PUBLIC void publishFrame(std::string_view id);
LIBSIP_CORE_PUBLIC void publishFrame(std::string_view id);
#endif

// methods which require android jni
#if defined(__ANDROID__)
// unused??
LIBSIP_CORE_PUBLIC void setVideoFrame(
    JNIEnv* jenv, jbyteArray frame, int frame_size, long target, int w, int h, int rotation);

LIBSIP_CORE_PUBLIC long acquireNativeWindow(JNIEnv* jenv, jobject javaSurface);
LIBSIP_CORE_PUBLIC void releaseNativeWindow(long windowId);
LIBSIP_CORE_PUBLIC void captureVideoFrame(
    JavaVM* javaVM, JNIEnv* jenv, const std::string& inputId, jobject javaImage, int rotation);
LIBSIP_CORE_PUBLIC void captureVideoPacket(const std::string& inputId,
                                           const ::std::shared_ptr< ::std::vector< uint8_t > >& buffer,
                                           int size,
                                           int offset,
                                           bool keyframe,
                                           long timestamp,
                                           int rotation);
LIBSIP_CORE_PUBLIC void setNativeWindowGeometry(long windowId, int width, int height);
LIBSIP_CORE_PUBLIC void unregisterVideoCallback(const std::string& sink, long windowId);
LIBSIP_CORE_PUBLIC bool registerVideoCallback(const std::string& sink, long windowId);
#endif

LIBSIP_CORE_PUBLIC bool getDecodingAccelerated();
LIBSIP_CORE_PUBLIC void setDecodingAccelerated(bool state);
LIBSIP_CORE_PUBLIC bool getEncodingAccelerated();
LIBSIP_CORE_PUBLIC void setEncodingAccelerated(bool state);

// player signal type definitions
struct LIBSIP_CORE_PUBLIC MediaPlayerSignal
{
    struct LIBSIP_CORE_PUBLIC FileOpened
    {
        constexpr static const char* name = "FileOpened";
        using cb_type = void(const std::string& /*playerId*/,
                             std::map<std::string, std::string> /*playerInfo*/);
    };
};

// Video signal type definitions
struct LIBSIP_CORE_PUBLIC VideoSignal
{
    struct LIBSIP_CORE_PUBLIC DeviceEvent
    {
        constexpr static const char* name = "DeviceEvent";
        using cb_type = void(void);
    };
    struct LIBSIP_CORE_PUBLIC DecodingStarted
    {
        constexpr static const char* name = "DecodingStarted";
        using cb_type = void(const std::string& /*id*/,
                             const std::string& /*shm_path*/,
                             int /*w*/,
                             int /*h*/,
                             bool /*is_mixer*/ id);
    };
    struct LIBSIP_CORE_PUBLIC DecodingStopped
    {
        constexpr static const char* name = "DecodingStopped";
        using cb_type = void(const std::string& /*id*/,
                             const std::string& /*shm_path*/,
                             bool /*is_mixer*/);
    };
#ifdef __ANDROID__
    struct LIBSIP_CORE_PUBLIC SetParameters
    {
        constexpr static const char* name = "SetParameters";
        using cb_type = void(const std::string& device,
                             const int format,
                             const int width,
                             const int height,
                             const int rate);
    };
    struct LIBSIP_CORE_PUBLIC GetCameraInfo
    {
        constexpr static const char* name = "GetCameraInfo";
        using cb_type = void(const std::string& device,
                             std::vector<int32_t>* formats,
                             std::vector<uint32_t>* sizes,
                             std::vector<uint32_t>* rates);
    };
    struct LIBSIP_CORE_PUBLIC RequestKeyFrame
    {
        constexpr static const char* name = "RequestKeyFrame";
        using cb_type = void(const std::string& /*device*/);
    };
    struct LIBSIP_CORE_PUBLIC SetBitrate
    {
        constexpr static const char* name = "SetBitrate";
        using cb_type = void(const std::string& /*device*/, const int bitrate);
    };
#endif
    struct LIBSIP_CORE_PUBLIC StartCapture
    {
        constexpr static const char* name = "StartCapture";
        using cb_type = void(const std::string& /*device*/);
    };
    struct LIBSIP_CORE_PUBLIC StopCapture
    {
        constexpr static const char* name = "StopCapture";
        using cb_type = void(const std::string& /*device*/);
    };
    struct LIBSIP_CORE_PUBLIC DeviceAdded
    {
        constexpr static const char* name = "DeviceAdded";
        using cb_type = void(const std::string& /*device*/);
    };
    struct LIBSIP_CORE_PUBLIC ParametersChanged
    {
        constexpr static const char* name = "ParametersChanged";
        using cb_type = void(const std::string& /*device*/);
    };
};

} // namespace libsip_core

#endif // DENABLE_VIDEOMANAGERI_H
