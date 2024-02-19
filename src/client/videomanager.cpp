/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
 *  Author: Pierre-Luc Beaudoin <pierre-luc.beaudoin@savoirfairelinux.com>
 *  Author: Emmanuel Milou <emmanuel.milou@savoirfairelinux.com>
 *  Author: Guillaume Carmel-Archambault <guillaume.carmel-archambault@savoirfairelinux.com>
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

#include "videomanager_interface.h"
#include "videomanager.h"
#include "localrecorder.h"
#include "localrecordermanager.h"
#include "libav_utils.h"
#include "video/video_input.h"
#include "video/video_device_monitor.h"
#include "account.h"
#include "logger.h"
#include "manager.h"
#include "system_codec_container.h"
#ifdef ENABLE_VIDEO
#include "video/sinkclient.h"
#endif
#include "client/ring_signal.h"
#include "audio/ringbufferpool.h"
#include "sip_core/media_const.h"
#include "libav_utils.h"
#include "call_const.h"
#include "system_codec_container.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <new> // std::bad_alloc
#include <algorithm>
#include <cstdlib>
#include <cstring> // std::memset
#include <ciso646> // fix windows compiler bug

#ifdef __ANDROID__
#include <mutex>

#include <list>
#include <utility>

#endif

extern "C" {
#include <libavutil/display.h>

#ifdef __ANDROID__
#include <libavutil/pixdesc.h>
#include <libavutil/imgutils.h>
#include <libavcodec/avcodec.h>
#endif
}

namespace libsip_core {

// external binding by default
#ifdef __ANDROID__
std::map<ANativeWindow*, libsip_core::FrameBuffer> windows {};
std::mutex windows_mutex;

std::vector<uint8_t> workspace;
int rotAngle = 0;
AVBufferRef* rotMatrix = nullptr;
constexpr const char TAG[] = "videomanager.cpp";
#endif

MediaFrame::MediaFrame()
    : frame_ {av_frame_alloc()}

{
    if (not frame_)
        throw std::bad_alloc();
}

void
MediaFrame::copyFrom(const MediaFrame& o)
{
    reset();
    if (o.frame_) {
        av_frame_ref(frame_.get(), o.frame_.get());
        av_frame_copy_props(frame_.get(), o.frame_.get());
    }

    if (o.packet_) {
        packet_.reset(av_packet_alloc());
        av_packet_ref(packet_.get(), o.packet_.get());
    }
}

void
MediaFrame::reset() noexcept
{
    if (frame_)
        av_frame_unref(frame_.get());
    packet_.reset();
}

void
MediaFrame::setPacket(PacketBuffer&& pkt)
{
    packet_ = std::move(pkt);
}

AudioFrame::AudioFrame(const sip_core::AudioFormat& format, size_t nb_samples)
    : MediaFrame()
{
    setFormat(format);
    if (nb_samples)
        reserve(nb_samples);
}

void
AudioFrame::setFormat(const sip_core::AudioFormat& format)
{
    auto d = pointer();
    d->channels = format.nb_channels;
    d->channel_layout = av_get_default_channel_layout(format.nb_channels);
    d->sample_rate = format.sample_rate;
    d->format = format.sampleFormat;
}

sip_core::AudioFormat
AudioFrame::getFormat() const
{
    return {(unsigned) frame_->sample_rate,
            (unsigned) frame_->channels,
            (AVSampleFormat) frame_->format};
}

size_t
AudioFrame::getFrameSize() const
{
    return frame_->nb_samples;
}

void
AudioFrame::reserve(size_t nb_samples)
{
    if (nb_samples != 0) {
        auto d = pointer();
        d->nb_samples = nb_samples;
        int err;
        if ((err = av_frame_get_buffer(d, 0)) < 0) {
            throw std::bad_alloc();
        }
    }
}

void
AudioFrame::mix(const AudioFrame& frame)
{
    auto& f = *pointer();
    auto& fIn = *frame.pointer();
    if (f.channels != fIn.channels || f.format != fIn.format || f.sample_rate != fIn.sample_rate) {
        throw std::invalid_argument("Can't mix frames with different formats");
    }
    if (f.nb_samples == 0) {
        reserve(fIn.nb_samples);
        sip_core::libav_utils::fillWithSilence(&f);
    } else if (f.nb_samples != fIn.nb_samples) {
        throw std::invalid_argument("Can't mix frames with different length");
    }
    AVSampleFormat fmt = (AVSampleFormat) f.format;
    bool isPlanar = av_sample_fmt_is_planar(fmt);
    unsigned samplesPerChannel = isPlanar ? f.nb_samples : f.nb_samples * f.channels;
    unsigned channels = isPlanar ? f.channels : 1;
    if (fmt == AV_SAMPLE_FMT_S16 || fmt == AV_SAMPLE_FMT_S16P) {
        for (unsigned i = 0; i < channels; i++) {
            auto c = (int16_t*) f.extended_data[i];
            auto cIn = (int16_t*) fIn.extended_data[i];
            for (unsigned s = 0; s < samplesPerChannel; s++) {
                c[s] = std::clamp((int32_t) c[s] + (int32_t) cIn[s],
                                  (int32_t) std::numeric_limits<int16_t>::min(),
                                  (int32_t) std::numeric_limits<int16_t>::max());
            }
        }
    } else if (fmt == AV_SAMPLE_FMT_FLT || fmt == AV_SAMPLE_FMT_FLTP) {
        for (unsigned i = 0; i < channels; i++) {
            auto c = (float*) f.extended_data[i];
            auto cIn = (float*) fIn.extended_data[i];
            for (unsigned s = 0; s < samplesPerChannel; s++) {
                c[s] += cIn[s];
            }
        }
    } else {
        throw std::invalid_argument(std::string("Unsupported format for mixing: ")
                                    + av_get_sample_fmt_name(fmt));
    }
}

float
AudioFrame::calcRMS() const
{
    double rms = 0.0;
    auto fmt = static_cast<AVSampleFormat>(frame_->format);
    bool planar = av_sample_fmt_is_planar(fmt);
    int perChannel = planar ? frame_->nb_samples : frame_->nb_samples * frame_->channels;
    int channels = planar ? frame_->channels : 1;
    if (fmt == AV_SAMPLE_FMT_S16 || fmt == AV_SAMPLE_FMT_S16P) {
        for (int c = 0; c < channels; ++c) {
            auto buf = reinterpret_cast<int16_t*>(frame_->extended_data[c]);
            for (int i = 0; i < perChannel; ++i) {
                auto sample = buf[i] * 0.000030517578125f;
                rms += sample * sample;
            }
        }
    } else if (fmt == AV_SAMPLE_FMT_FLT || fmt == AV_SAMPLE_FMT_FLTP) {
        for (int c = 0; c < channels; ++c) {
            auto buf = reinterpret_cast<float*>(frame_->extended_data[c]);
            for (int i = 0; i < perChannel; ++i) {
                rms += buf[i] * buf[i];
            }
        }
    } else {
        // Should not happen
        SIP_CORE_ERR() << "Unsupported format for getting volume level: "
                       << av_get_sample_fmt_name(fmt);
        return 0.0;
    }
    // divide by the number of multi-byte samples
    return sqrt(rms / (frame_->nb_samples * frame_->channels));
}

#ifdef ENABLE_VIDEO

VideoFrame::~VideoFrame()
{
    if (releaseBufferCb_)
        releaseBufferCb_(ptr_);
}

void
VideoFrame::reset() noexcept
{
    MediaFrame::reset();
    allocated_ = false;
    releaseBufferCb_ = {};
}

void
VideoFrame::copyFrom(const VideoFrame& o)
{
    MediaFrame::copyFrom(o);
    ptr_ = o.ptr_;
    allocated_ = o.allocated_;
}

size_t
VideoFrame::size() const noexcept
{
    return av_image_get_buffer_size((AVPixelFormat) frame_->format,
                                    frame_->width,
                                    frame_->height,
                                    1);
}

int
VideoFrame::format() const noexcept
{
    return frame_->format;
}

int
VideoFrame::width() const noexcept
{
    return frame_->width;
}

int
VideoFrame::height() const noexcept
{
    return frame_->height;
}

void
VideoFrame::setGeometry(int format, int width, int height) noexcept
{
    frame_->format = format;
    frame_->width = width;
    frame_->height = height;
}

void
VideoFrame::reserve(int format, int width, int height)
{
    auto libav_frame = frame_.get();

    if (allocated_) {
        // nothing to do if same properties
        if (width == libav_frame->width and height == libav_frame->height
            and format == libav_frame->format)
            av_frame_unref(libav_frame);
    }

    setGeometry(format, width, height);
    if (av_frame_get_buffer(libav_frame, 32))
        throw std::bad_alloc();
    allocated_ = true;
    releaseBufferCb_ = {};
}

void
VideoFrame::setFromMemory(uint8_t* ptr, int format, int width, int height) noexcept
{
    reset();
    setGeometry(format, width, height);
    if (not ptr)
        return;
    av_image_fill_arrays(frame_->data,
                         frame_->linesize,
                         (uint8_t*) ptr,
                         (AVPixelFormat) frame_->format,
                         width,
                         height,
                         1);
}

void
VideoFrame::setFromMemory(uint8_t* ptr,
                          int format,
                          int width,
                          int height,
                          const std::function<void(uint8_t*)>& cb) noexcept
{
    setFromMemory(ptr, format, width, height);
    if (cb) {
        releaseBufferCb_ = cb;
        ptr_ = ptr;
    }
}

void
VideoFrame::setReleaseCb(const std::function<void(uint8_t*)>& cb) noexcept
{
    if (cb) {
        releaseBufferCb_ = cb;
    }
}

void
VideoFrame::noise()
{
    auto f = frame_.get();
    if (f->data[0] == nullptr)
        return;
    for (std::size_t i = 0; i < size(); ++i) {
        f->data[0][i] = std::rand() & 255;
    }
}

int
VideoFrame::getOrientation() const
{
    int32_t* matrix {nullptr};
    if (auto p = packet()) {
        matrix = reinterpret_cast<int32_t*>(
            av_packet_get_side_data(p, AV_PKT_DATA_DISPLAYMATRIX, nullptr));
    } else if (auto p = pointer()) {
        if (AVFrameSideData* side_data = av_frame_get_side_data(p, AV_FRAME_DATA_DISPLAYMATRIX)) {
            matrix = reinterpret_cast<int32_t*>(side_data->data);
        }
    }
    if (matrix) {
        double angle = av_display_rotation_get(matrix);
        return std::isnan(angle) ? 0 : -(int) angle;
    }
    return 0;
}

VideoFrame*
getNewFrame(std::string_view id)
{
    if (auto input = sip_core::Manager::instance().getVideoManager().getVideoInput(id))
        return &input->getNewFrame();
    SIP_CORE_WARN("getNewFrame: can't find input %.*s", (int) id.size(), id.data());
    return nullptr;
}

void
publishFrame(std::string_view id)
{
    if (auto input = sip_core::Manager::instance().getVideoManager().getVideoInput(id))
        return input->publishFrame();
    SIP_CORE_WARN("publishFrame: can't find input %.*s", (int) id.size(), id.data());
}

void
registerVideoHandlers(const std::map<std::string, std::shared_ptr<CallbackWrapperBase>>& handlers)
{
    registerSignalHandlers(handlers);
}

std::vector<std::string>
getDeviceList()
{
    return sip_core::Manager::instance().getVideoManager().videoDeviceMonitor.getDeviceList();
}

VideoCapabilities
getCapabilities(const std::string& deviceId)
{
    return sip_core::Manager::instance().getVideoManager().videoDeviceMonitor.getCapabilities(
        deviceId);
}

std::string
getDefaultDevice()
{
    return sip_core::Manager::instance().getVideoManager().videoDeviceMonitor.getDefaultDevice();
}

void
setDefaultDevice(const std::string& deviceId)
{
    SIP_CORE_DBG("Setting default device to %s", deviceId.c_str());
    if (sip_core::Manager::instance().getVideoManager().videoDeviceMonitor.setDefaultDevice(
            deviceId))
        sip_core::Manager::instance().saveConfig();
}

void
setDeviceOrientation(const std::string& deviceId, int angle)
{
    sip_core::Manager::instance().getVideoManager().setDeviceOrientation(deviceId, angle);
}

std::map<std::string, std::string>
getDeviceParams(const std::string& deviceId)
{
    auto params = sip_core::Manager::instance().getVideoManager().videoDeviceMonitor.getDeviceParams(
        deviceId);
    std::stringstream rate;
    rate << params.framerate;
    return {{"format", params.format},
            {"width", std::to_string(params.width)},
            {"height", std::to_string(params.height)},
            {"rate", rate.str()}};
}

std::map<std::string, std::string>
getSettings(const std::string& deviceId)
{
    return sip_core::Manager::instance()
        .getVideoManager()
        .videoDeviceMonitor.getSettings(deviceId)
        .to_map();
}

void
applySettings(const std::string& deviceId, const std::map<std::string, std::string>& settings)
{
    sip_core::Manager::instance().getVideoManager().videoDeviceMonitor.applySettings(deviceId,
                                                                                     settings);
    sip_core::Manager::instance().saveConfig();
}

std::string
openVideoInput(const std::string& path)
{
    auto& vm = sip_core::Manager::instance().getVideoManager();

    auto id = path.empty() ? vm.videoDeviceMonitor.getMRLForDefaultDevice() : path;
    auto& input = vm.clientVideoInputs[id];
    if (not input) {
        input = sip_core::getVideoInput(id);
    }
    return id;
}

bool
closeVideoInput(const std::string& id)
{
    return sip_core::Manager::instance().getVideoManager().clientVideoInputs.erase(id) > 0;
}
#endif

void
startAudioDevice()
{
    auto newPreview = sip_core::getAudioInput(sip_core::RingBufferPool::DEFAULT_ID);
    sip_core::Manager::instance().getVideoManager().audioPreview = newPreview;
    newPreview->switchInput("");
}

void
stopAudioDevice()
{
    sip_core::Manager::instance().getVideoManager().audioPreview.reset();
}

std::string
startLocalMediaRecorder(const std::string& videoInputId, const std::string& filepath)
{
    auto rec = std::make_unique<sip_core::LocalRecorder>(videoInputId);
    rec->setPath(filepath);

    // retrieve final path (containing file extension)
    auto path = rec->getPath();

    auto& recordManager = sip_core::LocalRecorderManager::instance();

    try {
        recordManager.insertRecorder(path, std::move(rec));
    } catch (const std::invalid_argument&) {
        return "";
    }

    auto ret = recordManager.getRecorderByPath(path)->startRecording();
    if (!ret) {
        recordManager.removeRecorderByPath(filepath);
        return "";
    }

    return path;
}

void
stopLocalRecorder(const std::string& filepath)
{
    sip_core::LocalRecorder* rec = sip_core::LocalRecorderManager::instance().getRecorderByPath(
        filepath);
    if (!rec) {
        SIP_CORE_WARN("Can't stop non existing local recorder.");
        return;
    }

    rec->stopRecording();
    sip_core::LocalRecorderManager::instance().removeRecorderByPath(filepath);
}

bool
registerSinkTarget(const std::string& sinkId, SinkTarget target)
{
#ifdef ENABLE_VIDEO
    if (auto sink = sip_core::Manager::instance().getSinkClient(sinkId)) {
        sink->registerTarget(std::move(target));
        return true;
    } else
        SIP_CORE_WARN("No sink found for id '%s'", sinkId.c_str());
#endif
    return false;
}

#ifdef ENABLE_SHM
void
startShmSink(const std::string& sinkId, bool value)
{
#ifdef ENABLE_VIDEO
    if (auto sink = sip_core::Manager::instance().getSinkClient(sinkId))
        sink->enableShm(value);
    else
        SIP_CORE_WARN("No sink found for id '%s'", sinkId.c_str());
#endif
}
#endif

std::map<std::string, std::string>
getRenderer(const std::string& callId)
{
#ifdef ENABLE_VIDEO
    if (auto sink = sip_core::Manager::instance().getSinkClient(callId))
        return {
            {libsip_core::Media::Details::CALL_ID, callId},
            {libsip_core::Media::Details::SHM_PATH, sink->openedName()},
            {libsip_core::Media::Details::WIDTH, std::to_string(sink->getWidth())},
            {libsip_core::Media::Details::HEIGHT, std::to_string(sink->getHeight())},
        };
    else
#endif
        return {
            {libsip_core::Media::Details::CALL_ID, callId},
            {libsip_core::Media::Details::SHM_PATH, ""},
            {libsip_core::Media::Details::WIDTH, "0"},
            {libsip_core::Media::Details::HEIGHT, "0"},
        };
}

std::string
createMediaPlayer(const std::string& path)
{
    return sip_core::createMediaPlayer(path);
}

bool
pausePlayer(const std::string& id, bool pause)
{
    return sip_core::pausePlayer(id, pause);
}

bool
closeMediaPlayer(const std::string& id)
{
    return sip_core::closeMediaPlayer(id);
}

bool
mutePlayerAudio(const std::string& id, bool mute)
{
    return sip_core::mutePlayerAudio(id, mute);
}

bool
playerSeekToTime(const std::string& id, int time)
{
    return sip_core::playerSeekToTime(id, time);
}

int64_t
getPlayerPosition(const std::string& id)
{
    return sip_core::getPlayerPosition(id);
}

bool
getDecodingAccelerated()
{
#ifdef RING_ACCEL
    return sip_core::Manager::instance().videoPreferences.getDecodingAccelerated();
#else
    return false;
#endif
}

void
setDecodingAccelerated(bool state)
{
#ifdef RING_ACCEL
    SIP_CORE_DBG("%s hardware acceleration", (state ? "Enabling" : "Disabling"));
    if (sip_core::Manager::instance().videoPreferences.setDecodingAccelerated(state))
        sip_core::Manager::instance().saveConfig();
#endif
}

bool
getEncodingAccelerated()
{
#ifdef RING_ACCEL
    return sip_core::Manager::instance().videoPreferences.getEncodingAccelerated();
#else
    return false;
#endif
}

void
setEncodingAccelerated(bool state)
{
#ifdef RING_ACCEL
    SIP_CORE_DBG("%s hardware acceleration", (state ? "Enabling" : "Disabling"));
    if (sip_core::Manager::instance().videoPreferences.setEncodingAccelerated(state))
        sip_core::Manager::instance().saveConfig();
    else
        return;
#endif
    for (const auto& acc : sip_core::Manager::instance().getAllAccounts()) {
        if (state)
            acc->setCodecActive(AV_CODEC_ID_HEVC);
        else
            acc->setCodecInactive(AV_CODEC_ID_HEVC);
        // Update and sort codecs
        acc->setActiveCodecs(acc->getActiveCodecs());
        sip_core::Manager::instance().saveConfig(acc);
    }
}

#if defined(__ANDROID__) || defined(RING_UWP) || (defined(TARGET_OS_IOS) && TARGET_OS_IOS)
void
addVideoDevice(const std::string& node,
               const std::vector<std::map<std::string, std::string>>& devInfo)
{
    sip_core::Manager::instance().getVideoManager().videoDeviceMonitor.addDevice(node, devInfo);
}

#if defined(__ANDROID__)

void
releaseBuffer(ANativeWindow* window, libsip_core::FrameBuffer frame)
{
    std::unique_lock<std::mutex> guard(windows_mutex);
    try {
        windows.at(window) = std::move(frame);
    } catch (...) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "Can't move frame: no window");
    }
}

void
AndroidDisplayCb(ANativeWindow* window, libsip_core::FrameBuffer frame)
{
    ANativeWindow_unlockAndPost(window);
    releaseBuffer(window, std::move(frame));
}

int
AndroidFormatToAVFormat(int androidformat)
{
    switch (androidformat) {
    case 17: // ImageFormat.NV21
        return AV_PIX_FMT_NV21;
    case 35: // ImageFormat.YUV_420_888
        return AV_PIX_FMT_YUV420P;
    case 39: // ImageFormat.YUV_422_888
        return AV_PIX_FMT_YUV422P;
    case 41: // ImageFormat.FLEX_RGB_888
        return AV_PIX_FMT_GBRP;
    case 42: // ImageFormat.FLEX_RGBA_8888
        return AV_PIX_FMT_GBRAP;
    default:
        return AV_PIX_FMT_NONE;
    }
}

libsip_core::FrameBuffer
sinkTargetPullCallback(ANativeWindow* window)
{
    try {
        libsip_core::FrameBuffer frame;
        {
            std::lock_guard<std::mutex> guard(windows_mutex);
            frame = std::move(windows.at(window));
        }
        if (frame) {
            ANativeWindow_Buffer buffer;
            if (ANativeWindow_lock(window, &buffer, nullptr) == 0) {
                frame->format = AV_PIX_FMT_RGBA;
                frame->width = buffer.width;
                frame->height = buffer.height;
                frame->data[0] = (uint8_t*) buffer.bits;
                frame->linesize[0] = buffer.stride * 4;
                return frame;
            } else {
                __android_log_print(ANDROID_LOG_WARN, TAG, "Can't lock window");
                releaseBuffer(window, std::move(frame));
            }
        }
    } catch (...) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "Exception in pull callback");
    }
    return {};
}

void
rotateNV21(uint8_t* yinput,
           uint8_t* uvinput,
           unsigned ystride,
           unsigned uvstride,
           unsigned width,
           unsigned height,
           int rotation,
           uint8_t* youtput,
           uint8_t* uvoutput)
{
    if (rotation == 0) {
        std::copy_n(yinput, ystride * height, youtput);
        std::copy_n(uvinput, uvstride * height, uvoutput);
        return;
    }
    if (rotation % 90 != 0 || rotation < 0 || rotation > 270) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "%u %u %d", width, height, rotation);
        return;
    }
    bool swap = rotation % 180 != 0;
    bool xflip = rotation % 270 != 0;
    bool yflip = rotation >= 180;
    unsigned wOut = swap ? height : width;
    unsigned hOut = swap ? width : height;

    for (unsigned j = 0; j < height; j++) {
        for (unsigned i = 0; i < width; i++) {
            unsigned yIn = j * ystride + i;
            unsigned uIn = (j >> 1) * uvstride + (i & ~1);
            unsigned vIn = uIn + 1;
            unsigned iSwapped = swap ? j : i;
            unsigned jSwapped = swap ? i : j;
            unsigned iOut = xflip ? wOut - iSwapped - 1 : iSwapped;
            unsigned jOut = yflip ? hOut - jSwapped - 1 : jSwapped;
            unsigned yOut = jOut * wOut + iOut;
            unsigned uOut = (jOut >> 1) * wOut + (iOut & ~1);
            unsigned vOut = uOut + 1;
            youtput[yOut] = yinput[yIn];
            uvoutput[uOut] = uvinput[uIn];
            uvoutput[vOut] = uvinput[vIn];
        }
    }
    return;
}

void
setRotation(int angle)
{
    if (angle == rotAngle)
        return;
    AVBufferRef* localFrameDataBuffer = angle == 0 ? nullptr : av_buffer_alloc(sizeof(int32_t) * 9);
    if (localFrameDataBuffer)
        av_display_rotation_set(reinterpret_cast<int32_t*>(localFrameDataBuffer->data), angle);

    std::swap(rotMatrix, localFrameDataBuffer);
    rotAngle = angle;

    av_buffer_unref(&localFrameDataBuffer);
}

void
captureVideoPacket(JNIEnv* jenv,
                   const std::string& input,
                   jobject buffer,
                   int size,
                   int offset,
                   bool keyframe,
                   long timestamp,
                   int rotation)
{
    try {
        auto frame = libsip_core::getNewFrame(input);
        if (not frame)
            return;
        auto packet = libsip_core::PacketBuffer(av_packet_alloc());
        if (keyframe)
            packet->flags = AV_PKT_FLAG_KEY;
        setRotation(rotation);
        if (rotMatrix) {
            auto buf = av_packet_new_side_data(packet.get(),
                                               AV_PKT_DATA_DISPLAYMATRIX,
                                               rotMatrix->size);
            std::copy_n(rotMatrix->data, rotMatrix->size, buf);
        }
        auto data = (uint8_t*) jenv->GetDirectBufferAddress(buffer);
        packet->data = data + offset;
        packet->size = size;
        packet->pts = timestamp;
        frame->setPacket(std::move(packet));
        libsip_core::publishFrame(input);
    } catch (const std::exception& e) {
        __android_log_print(ANDROID_LOG_ERROR,
                            TAG,
                            "Exception capturing video packet: %s",
                            e.what());
    }
}

void
setNativeWindowGeometry(long windowId, int width, int height)
{
    ANativeWindow* window = (ANativeWindow*) ((intptr_t) windowId);
    ANativeWindow_setBuffersGeometry(window, width, height, WINDOW_FORMAT_RGBX_8888);
}

long
acquireNativeWindow(JNIEnv* jenv, jobject javaSurface)
{
    return (long) ANativeWindow_fromSurface(jenv, javaSurface);
}

void
setVideoFrame(JNIEnv* jenv, jbyteArray frame, int frame_size, long target, int w, int h, int rotation)
{
    uint8_t* f_target = (uint8_t*) ((intptr_t) target);
    if (rotation == 0)
        jenv->GetByteArrayRegion(frame, 0, frame_size, (jbyte*) f_target);
    else {
        workspace.resize(frame_size);
        jenv->GetByteArrayRegion(frame, 0, frame_size, (jbyte*) workspace.data());
        auto planeSize = w * h;
        rotateNV21(workspace.data(),
                   workspace.data() + planeSize,
                   w,
                   w,
                   w,
                   h,
                   rotation,
                   f_target,
                   f_target + planeSize);
    }
}

void
releaseNativeWindow(long windowId)
{
    ANativeWindow* window = (ANativeWindow*) ((intptr_t) windowId);
    ANativeWindow_release(window);
}

void
unregisterVideoCallback(const std::string& sink, long windowId)
{
    libsip_core::registerSinkTarget(sink, libsip_core::SinkTarget {});
    ANativeWindow* nativeWindow = (ANativeWindow*) ((intptr_t) windowId);

    std::lock_guard<std::mutex> guard(windows_mutex);
    windows.erase(nativeWindow);
}

bool
registerVideoCallback(const std::string& sink, long windowId)
{
    ANativeWindow* nativeWindow = (ANativeWindow*) ((intptr_t) windowId);
    auto f_display_cb = std::bind(&AndroidDisplayCb, nativeWindow, std::placeholders::_1);
    auto p_display_cb = std::bind(&sinkTargetPullCallback, nativeWindow);

    {
        std::lock_guard<std::mutex> guard(windows_mutex);
        windows.emplace(nativeWindow, libsip_core::FrameBuffer {av_frame_alloc()});
    }
    return libsip_core::registerSinkTarget(sink,
                                           libsip_core::SinkTarget {.pull = p_display_cb,
                                                                    .push = f_display_cb});
}

void
captureVideoFrame(JavaVM* javaVM, JNIEnv* jenv, const std::string& input, jobject image, int rotation)
{
    static jclass imageClass = jenv->GetObjectClass(image);
    static jmethodID imageGetFormat = jenv->GetMethodID(imageClass, "getFormat", "()I");
    static jmethodID imageGetWidth = jenv->GetMethodID(imageClass, "getWidth", "()I");
    static jmethodID imageGetHeight = jenv->GetMethodID(imageClass, "getHeight", "()I");
    static jmethodID imageGetCropRect = jenv->GetMethodID(imageClass,
                                                          "getCropRect",
                                                          "()Landroid/graphics/Rect;");
    static jmethodID imageGetPlanes = jenv->GetMethodID(imageClass,
                                                        "getPlanes",
                                                        "()[Landroid/media/Image$Plane;");
    static jmethodID imageClose = jenv->GetMethodID(imageClass, "close", "()V");

    try {
        auto frame = libsip_core::getNewFrame(input);
        if (not frame) {
            jenv->CallVoidMethod(image, imageClose);
            return;
        }

        auto avframe = frame->pointer();

        avframe->format = AndroidFormatToAVFormat(jenv->CallIntMethod(image, imageGetFormat));
        avframe->width = jenv->CallIntMethod(image, imageGetWidth);
        avframe->height = jenv->CallIntMethod(image, imageGetHeight);
        jobject crop = jenv->CallObjectMethod(image, imageGetCropRect);
        if (crop) {
            static jclass rectClass = jenv->GetObjectClass(crop);
            static jfieldID rectTopField = jenv->GetFieldID(rectClass, "top", "I");
            static jfieldID rectLeftField = jenv->GetFieldID(rectClass, "left", "I");
            static jfieldID rectBottomField = jenv->GetFieldID(rectClass, "bottom", "I");
            static jfieldID rectRightField = jenv->GetFieldID(rectClass, "right", "I");
            avframe->crop_top = jenv->GetIntField(crop, rectTopField);
            avframe->crop_left = jenv->GetIntField(crop, rectLeftField);
            avframe->crop_bottom = avframe->height - jenv->GetIntField(crop, rectBottomField);
            avframe->crop_right = avframe->width - jenv->GetIntField(crop, rectRightField);
        }

        jobjectArray planes = (jobjectArray) jenv->CallObjectMethod(image, imageGetPlanes);
        static jclass planeClass = jenv->GetObjectClass(jenv->GetObjectArrayElement(planes, 0));
        static jmethodID planeGetBuffer = jenv->GetMethodID(planeClass,
                                                            "getBuffer",
                                                            "()Ljava/nio/ByteBuffer;");
        static jmethodID planeGetRowStride = jenv->GetMethodID(planeClass, "getRowStride", "()I");
        static jmethodID planeGetPixelStride = jenv->GetMethodID(planeClass,
                                                                 "getPixelStride",
                                                                 "()I");

        jsize planeCount = jenv->GetArrayLength(planes);
        if (avframe->format == AV_PIX_FMT_YUV420P) {
            jobject yplane = jenv->GetObjectArrayElement(planes, 0);
            jobject uplane = jenv->GetObjectArrayElement(planes, 1);
            jobject vplane = jenv->GetObjectArrayElement(planes, 2);
            auto ydata = (uint8_t*) jenv->GetDirectBufferAddress(
                jenv->CallObjectMethod(yplane, planeGetBuffer));
            auto udata = (uint8_t*) jenv->GetDirectBufferAddress(
                jenv->CallObjectMethod(uplane, planeGetBuffer));
            auto vdata = (uint8_t*) jenv->GetDirectBufferAddress(
                jenv->CallObjectMethod(vplane, planeGetBuffer));
            auto ystride = jenv->CallIntMethod(yplane, planeGetRowStride);
            auto uvstride = jenv->CallIntMethod(uplane, planeGetRowStride);
            auto uvpixstride = jenv->CallIntMethod(uplane, planeGetPixelStride);

            if (uvpixstride == 1) {
                avframe->data[0] = ydata;
                avframe->linesize[0] = ystride;
                avframe->data[1] = udata;
                avframe->linesize[1] = uvstride;
                avframe->data[2] = vdata;
                avframe->linesize[2] = uvstride;
            } else if (uvpixstride == 2) {
                // False YUV420, actually NV12 or NV21
                auto uvdata = std::min(udata, vdata);
                avframe->format = uvdata == udata ? AV_PIX_FMT_NV12 : AV_PIX_FMT_NV21;
                avframe->data[0] = ydata;
                avframe->linesize[0] = ystride;
                avframe->data[1] = uvdata;
                avframe->linesize[1] = uvstride;
            }
        } else {
            for (int i = 0; i < planeCount; i++) {
                jobject plane = jenv->GetObjectArrayElement(planes, i);
                // jint pxStride = jenv->CallIntMethod(plane, planeGetPixelStride);
                avframe->data[i] = (uint8_t*) jenv->GetDirectBufferAddress(
                    jenv->CallObjectMethod(plane, planeGetBuffer));
                avframe->linesize[i] = jenv->CallIntMethod(plane, planeGetRowStride);
            }
        }

        setRotation(rotation);
        if (rotMatrix)
            av_frame_new_side_data_from_buf(avframe,
                                            AV_FRAME_DATA_DISPLAYMATRIX,
                                            av_buffer_ref(rotMatrix));

        image = jenv->NewGlobalRef(image);
        frame->setReleaseCb([javaVM, jenv, image](uint8_t*) mutable {
            bool justAttached = false;
            int envStat = javaVM->GetEnv((void**) &jenv, JNI_VERSION_1_6);
            if (envStat == JNI_EDETACHED) {
                justAttached = true;
                if (javaVM->AttachCurrentThread(&jenv, nullptr) != 0)
                    return;
            } else if (envStat == JNI_EVERSION) {
                return;
            }
            jenv->CallVoidMethod(image, imageClose);
            jenv->DeleteGlobalRef(image);
            if (justAttached)
                javaVM->DetachCurrentThread();
        });
        libsip_core::publishFrame(input);
    } catch (const std::exception& e) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Exception capturing video frame: %s", e.what());
    }
}

#endif

void
removeVideoDevice(const std::string& node)
{
    sip_core::Manager::instance().getVideoManager().videoDeviceMonitor.removeDevice(node);
}
#endif

} // namespace libsip_core

namespace sip_core {

#ifdef ENABLE_VIDEO
video::VideoDeviceMonitor&
getVideoDeviceMonitor()
{
    return Manager::instance().getVideoManager().videoDeviceMonitor;
}

std::shared_ptr<video::VideoInput>
getVideoInput(const std::string& id, video::VideoInputMode inputMode)
{
    auto& vmgr = Manager::instance().getVideoManager();
    std::lock_guard<std::mutex> lk(vmgr.videoMutex);
    auto it = vmgr.videoInputs.find(id);
    if (it != vmgr.videoInputs.end()) {
        if (auto input = it->second.lock()) {
            return input;
        }
    }

    auto input = std::make_shared<video::VideoInput>(inputMode, id);
    vmgr.videoInputs[id] = input;
    return input;
}

void
VideoManager::setDeviceOrientation(const std::string& deviceId, int angle)
{
    videoDeviceMonitor.setDeviceOrientation(deviceId, angle);
}
#endif

std::shared_ptr<AudioInput>
getAudioInput(const std::string& id)
{
    auto& vmgr = Manager::instance().getVideoManager();
    std::lock_guard<std::mutex> lk(vmgr.audioMutex);

    // erase expired audio inputs
    for (auto it = vmgr.audioInputs.cbegin(); it != vmgr.audioInputs.cend();) {
        if (it->second.expired())
            it = vmgr.audioInputs.erase(it);
        else
            ++it;
    }

    auto it = vmgr.audioInputs.find(id);
    if (it != vmgr.audioInputs.end()) {
        if (auto input = it->second.lock()) {
            return input;
        }
    }

    auto input = std::make_shared<AudioInput>(id);
    vmgr.audioInputs[id] = input;
    return input;
}

bool
VideoManager::hasRunningPlayers()
{
    auto& vmgr = Manager::instance().getVideoManager();
    return !vmgr.mediaPlayers.empty();
}

std::shared_ptr<MediaPlayer>
getMediaPlayer(const std::string& id)
{
    auto& vmgr = Manager::instance().getVideoManager();
    auto it = vmgr.mediaPlayers.find(id);
    if (it != vmgr.mediaPlayers.end()) {
        return it->second;
    }
    return {};
}

std::string
createMediaPlayer(const std::string& path)
{
    auto player = std::make_shared<MediaPlayer>(path);
    if (!player->isInputValid()) {
        return "";
    }
    auto playerId = player.get()->getId();
    Manager::instance().getVideoManager().mediaPlayers[playerId] = player;
    return playerId;
}

bool
pausePlayer(const std::string& id, bool pause)
{
    if (auto player = getMediaPlayer(id)) {
        player->pause(pause);
        return true;
    }
    return false;
}

bool
closeMediaPlayer(const std::string& id)
{
    return Manager::instance().getVideoManager().mediaPlayers.erase(id) > 0;
}

bool
mutePlayerAudio(const std::string& id, bool mute)
{
    if (auto player = getMediaPlayer(id)) {
        player->muteAudio(mute);
        return true;
    }
    return false;
}

bool
playerSeekToTime(const std::string& id, int time)
{
    if (auto player = getMediaPlayer(id))
        return player->seekToTime(time);
    return false;
}

int64_t
getPlayerPosition(const std::string& id)
{
    if (auto player = getMediaPlayer(id))
        return player->getPlayerPosition();
    return -1;
}

} // namespace sip_core
