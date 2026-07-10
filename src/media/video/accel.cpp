/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
 *  Author: Philippe Gorley <philippe.gorley@savoirfairelinux.com>
 *  Author: Pierre Lespagnol <pierre.lespagnol@savoirfairelinux.com>
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

#include <algorithm>
#include <mutex>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "media_buffer.h"
#include "string_utils.h"
#include "fileutils.h"
#include "logger.h"
#include "accel.h"

namespace sip_core {
namespace video {

struct HardwareAccel::HardwareAPI
{
    std::string name;
    AVHWDeviceType hwType;
    AVPixelFormat format;
    AVPixelFormat swFormat;
    std::vector<AVCodecID> supportedCodecs;
    std::list<std::pair<std::string, DeviceState>> possible_devices;
    bool dynBitrate;
};


std::vector<HardwareAccel::HardwareAPI> HardwareAccel::apiListDec_ = {
    {"dxva2",
     AV_HWDEVICE_TYPE_DXVA2,
     AV_PIX_FMT_DXVA2_VLD,
     AV_PIX_FMT_NV12,
     {AV_CODEC_ID_H264, AV_CODEC_ID_HEVC, AV_CODEC_ID_MJPEG, AV_CODEC_ID_VP8, AV_CODEC_ID_VP9},
     {{"default", DeviceState::NOT_TESTED}},
     false},
    {"d3d11va",
     AV_HWDEVICE_TYPE_D3D11VA,
     AV_PIX_FMT_D3D11,
     AV_PIX_FMT_NV12,
     {AV_CODEC_ID_H264, AV_CODEC_ID_HEVC, AV_CODEC_ID_MJPEG, AV_CODEC_ID_VP9},
     {{"default", DeviceState::NOT_TESTED}},
     false},
    {"vaapi",
     AV_HWDEVICE_TYPE_VAAPI,
     AV_PIX_FMT_VAAPI,
     AV_PIX_FMT_NV12,
     {AV_CODEC_ID_H264, AV_CODEC_ID_MPEG4, AV_CODEC_ID_VP8},
     {{"default", DeviceState::NOT_TESTED}, {"/dev/dri/renderD128", DeviceState::NOT_TESTED},
      {"/dev/dri/renderD129", DeviceState::NOT_TESTED}, {":0", DeviceState::NOT_TESTED}},
     false},
    {"qsv",
     AV_HWDEVICE_TYPE_QSV,
     AV_PIX_FMT_QSV,
     AV_PIX_FMT_NV12,
     {AV_CODEC_ID_H264, AV_CODEC_ID_HEVC, AV_CODEC_ID_MJPEG, AV_CODEC_ID_VP8, AV_CODEC_ID_VP9},
     {{"default", DeviceState::NOT_TESTED}},
     false},
    {"nvdec",
     AV_HWDEVICE_TYPE_CUDA,
     AV_PIX_FMT_CUDA,
     AV_PIX_FMT_NV12,
     {AV_CODEC_ID_H264, AV_CODEC_ID_HEVC, AV_CODEC_ID_VP8, AV_CODEC_ID_VP9, AV_CODEC_ID_MJPEG},
     {{"default", DeviceState::NOT_TESTED}, {"1", DeviceState::NOT_TESTED}, {"2", DeviceState::NOT_TESTED}},
     false},
    {"cuvid",
     AV_HWDEVICE_TYPE_CUDA,
     AV_PIX_FMT_CUDA,
     AV_PIX_FMT_NV12,
     {AV_CODEC_ID_H264, AV_CODEC_ID_VP8, AV_CODEC_ID_VP9},
     {{"default", DeviceState::NOT_TESTED}, {"1", DeviceState::NOT_TESTED}, {"2", DeviceState::NOT_TESTED}},
     false},
    {"videotoolbox",
     AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
     AV_PIX_FMT_VIDEOTOOLBOX,
     AV_PIX_FMT_NV12,
     {AV_CODEC_ID_H264, AV_CODEC_ID_HEVC, AV_CODEC_ID_MPEG4},
     {{"default", DeviceState::NOT_TESTED}},
     false},
    {"d3d12va",
     AV_HWDEVICE_TYPE_D3D12VA,
     AV_PIX_FMT_D3D12,
     AV_PIX_FMT_NV12,
     {AV_CODEC_ID_H264, AV_CODEC_ID_HEVC, AV_CODEC_ID_MJPEG, AV_CODEC_ID_VP9},
     {{"default", DeviceState::NOT_TESTED}},
     false},
};

std::vector<HardwareAccel::HardwareAPI> HardwareAccel::apiListEnc_ = {
    {"nvenc",
     AV_HWDEVICE_TYPE_CUDA,
     AV_PIX_FMT_CUDA,
     AV_PIX_FMT_NV12,
     {AV_CODEC_ID_H264, AV_CODEC_ID_HEVC},
     {{"default", DeviceState::NOT_TESTED}, {"1", DeviceState::NOT_TESTED}, {"2", DeviceState::NOT_TESTED}},
     true},
    {"qsv",
     AV_HWDEVICE_TYPE_QSV,
     AV_PIX_FMT_QSV,
     AV_PIX_FMT_NV12,
     {AV_CODEC_ID_H264, AV_CODEC_ID_HEVC, AV_CODEC_ID_MJPEG, AV_CODEC_ID_VP8},
    {{"default", DeviceState::NOT_TESTED}},
    false},
    {"vaapi",
     AV_HWDEVICE_TYPE_VAAPI,
     AV_PIX_FMT_VAAPI,
     AV_PIX_FMT_NV12,
     {AV_CODEC_ID_H264, AV_CODEC_ID_HEVC, AV_CODEC_ID_VP8},
     {{"default", DeviceState::NOT_TESTED}, {"/dev/dri/renderD128", DeviceState::NOT_TESTED},
      {"/dev/dri/renderD129", DeviceState::NOT_TESTED},
      {":0", DeviceState::NOT_TESTED}},
     false},
    {"videotoolbox",
     AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
     AV_PIX_FMT_VIDEOTOOLBOX,
     AV_PIX_FMT_NV12,
     {AV_CODEC_ID_H264, AV_CODEC_ID_HEVC},
     {{"default", DeviceState::NOT_TESTED}},
     false},
};

std::vector<HardwareAccel::HardwareAPI> HardwareAccel::apiListOpencl_ = {
    {"opencl",
     AV_HWDEVICE_TYPE_OPENCL,
     AV_PIX_FMT_OPENCL,
     AV_PIX_FMT_NV12,
     { AV_CODEC_ID_NONE },
     {{"default", DeviceState::NOT_TESTED},{"0.0", DeviceState::NOT_TESTED}},
     false}
};

HardwareAccel::HardwareAccel(AVCodecID id,
                             const std::string& name,
                             AVHWDeviceType hwType,
                             AVPixelFormat format,
                             AVPixelFormat swFormat,
                             CodecType type,
                             bool dynBitrate)
    : id_(id)
    , name_(name)
    , hwType_(hwType)
    , format_(format)
    , swFormat_(swFormat)
    , type_(type)
    , dynBitrate_(dynBitrate)
{}

HardwareAccel::~HardwareAccel()
{
    if (deviceCtx_)
        av_buffer_unref(&deviceCtx_);
    if (framesCtx_)
        av_buffer_unref(&framesCtx_);
    for (auto& pool : uploadPools_)
        av_buffer_unref(&pool.second);
}

static AVPixelFormat
getFormatCb(AVCodecContext* codecCtx, const AVPixelFormat* formats)
{
    // this cb is called only for decoders
    auto accel = static_cast<HardwareAccel*>(codecCtx->opaque);

    for (int i = 0; formats[i] != AV_PIX_FMT_NONE; ++i) {
        if (accel && formats[i] == accel->getFormat()) {
            // found hardware format for codec with api
            SIP_CORE_DBG() << "Found compatible hardware format for "
                       << avcodec_get_name(static_cast<AVCodecID>(accel->getCodecId()))
                       << " decoder with " << accel->getName();

            if (!codecCtx->hw_device_ctx) {
                SIP_CORE_ERR() << "Cannot initialize hardware frames without a valid hardware device";
                return AV_PIX_FMT_NONE;
            }

            // Let the decoder size the frames context (dimensions, alignment,
            // pool size incl. reference frames); fall back to a manual setup
            // for decoders that do not implement it.
            AVBufferRef* frame_ctx = nullptr;
            int ret = avcodec_get_hw_frames_parameters(codecCtx,
                                                       codecCtx->hw_device_ctx,
                                                       formats[i],
                                                       &frame_ctx);
            if (ret < 0) {
                frame_ctx = av_hwframe_ctx_alloc(codecCtx->hw_device_ctx);
                if (!frame_ctx)
                    return AV_PIX_FMT_NONE;

                auto ctx = reinterpret_cast<AVHWFramesContext*>(frame_ctx->data);
                ctx->format = formats[i];
                ctx->sw_format = accel->getSoftwareFormat();
                ctx->width = codecCtx->coded_width ? codecCtx->coded_width : codecCtx->width;
                ctx->height = codecCtx->coded_height ? codecCtx->coded_height : codecCtx->height;
                ctx->initial_pool_size = 20;
            } else {
                // Decoded frames now stay GPU-resident past the decode loop:
                // downstream consumers hold surfaces (engine last-frame +
                // in-flight present + glue latest-wins slot). Enlarge fixed
                // pools so held surfaces cannot starve the decoder; 0 means a
                // dynamic pool, which cannot starve.
                auto ctx = reinterpret_cast<AVHWFramesContext*>(frame_ctx->data);
                if (ctx->initial_pool_size > 0)
                    ctx->initial_pool_size += 4;
            }

            if ((ret = av_hwframe_ctx_init(frame_ctx)) < 0) {
                SIP_CORE_ERR("Failed to initialize hardware frame context: %s (%d)",
                        libav_utils::getError(ret).c_str(),
                        ret);
                av_buffer_unref(&frame_ctx);
                return AV_PIX_FMT_NONE;
            }

            // hardware tends to under-report supported levels
            codecCtx->hwaccel_flags |= AV_HWACCEL_FLAG_IGNORE_LEVEL;

            if (codecCtx->hw_frames_ctx)
                av_buffer_unref(&codecCtx->hw_frames_ctx);

            // transfer ownership of our only ref — no extra ref, no leak
            codecCtx->hw_frames_ctx = frame_ctx;
            return formats[i];
        }
    }
    return AV_PIX_FMT_NONE;
}

int
HardwareAccel::init_device(const char* name, const char* device, int flags)
{
    const AVHWDeviceContext* dev = nullptr;

    // Create device ctx
    int err;
    err = av_hwdevice_ctx_create(&deviceCtx_, hwType_, device, NULL, flags);
    if (err < 0) {
        SIP_CORE_DBG("Failed to create %s device: %d.\n", name, err);
        return 1;
    }

    // Verify that the device create correspond to api
    dev = (AVHWDeviceContext*) deviceCtx_->data;
    if (dev->type != hwType_) {
        SIP_CORE_DBG("Device created as type %d has type %d.", hwType_, dev->type);
        av_buffer_unref(&deviceCtx_);
        return -1;
    }
    SIP_CORE_DBG("Device type %s successfully created.", name);

    return 0;
}

int
HardwareAccel::init_device_type(std::string& dev)
{
    // The DeviceState lists are shared static state mutated from every
    // decoder/encoder/mixer thread that probes devices.
    static std::mutex deviceProbeMtx;
    std::lock_guard<std::mutex> probeLock(deviceProbeMtx);

    AVHWDeviceType check;
    const char* name;
    int err;

    name = av_hwdevice_get_type_name(hwType_);
    if (!name) {
        SIP_CORE_DBG("No name available for device type %d.", hwType_);
        return -1;
    }

    check = av_hwdevice_find_type_by_name(name);
    if (check != hwType_) {
        SIP_CORE_DBG("Type %d maps to name %s maps to type %d.", hwType_, name, check);
        return -1;
    }

    SIP_CORE_WARN("-- Starting %s init for %s with default device.",
              (type_ == CODEC_ENCODER) ? "encoding" : "decoding",
              name);
    if (possible_devices_->front().second != DeviceState::NOT_USABLE) {
        if (name_ == "qsv")
            err = init_device(name, "auto", 0);
        else
            err = init_device(name, nullptr, 0);
        if (err == 0) {
            SIP_CORE_DBG("-- Init passed for %s with default device.", name);
            possible_devices_->front().second = DeviceState::USABLE;
            dev = "default";
            return 0;
        } else {
            possible_devices_->front().second = DeviceState::NOT_USABLE;
            SIP_CORE_DBG("-- Init failed for %s with default device.", name);
        }
    }

    for (auto& device : *possible_devices_) {
        if (device.second == DeviceState::NOT_USABLE)
            continue;
        SIP_CORE_WARN("-- Init %s for %s with device %s.",
                  (type_ == CODEC_ENCODER) ? "encoding" : "decoding",
                  name,
                  device.first.c_str());
        err = init_device(name, device.first.c_str(), 0);
        if (err == 0) {
            SIP_CORE_DBG("-- Init passed for %s with device %s.", name, device.first.c_str());
            device.second = DeviceState::USABLE;
            dev = device.first;
            return 0;
        } else {
            device.second = DeviceState::NOT_USABLE;
            SIP_CORE_DBG("-- Init failed for %s with device %s.", name, device.first.c_str());
        }
    }
    return -1;
}

std::string
HardwareAccel::getCodecName() const
{
    return fmt::format("{}_{}", avcodec_get_name(id_), name_);
}

std::unique_ptr<VideoFrame>
HardwareAccel::transfer(const VideoFrame& frame)
{
    int ret = 0;
    if (type_ == CODEC_DECODER) {
        auto input = frame.pointer();
        if (input->format != format_) {
            SIP_CORE_ERR() << "Frame format mismatch: expected " << av_get_pix_fmt_name(format_)
                       << ", got "
                       << av_get_pix_fmt_name(static_cast<AVPixelFormat>(input->format));
            return nullptr;
        }

        return transferToMainMemory(frame, swFormat_);
    } else if (type_ == CODEC_ENCODER) {
        auto input = frame.pointer();
        if (input->format != swFormat_) {
            SIP_CORE_ERR() << "Frame format mismatch: expected " << av_get_pix_fmt_name(swFormat_)
                       << ", got "
                       << av_get_pix_fmt_name(static_cast<AVPixelFormat>(input->format));
            return nullptr;
        }

        auto framePtr = std::make_unique<VideoFrame>();
        auto hwFrame = framePtr->pointer();

        if ((ret = av_hwframe_get_buffer(framesCtx_, hwFrame, 0)) < 0) {
            SIP_CORE_ERR() << "Failed to allocate hardware buffer: "
                       << libav_utils::getError(ret).c_str();
            return nullptr;
        }

        if (!hwFrame->hw_frames_ctx) {
            SIP_CORE_ERR() << "Failed to allocate hardware buffer: Cannot allocate memory";
            return nullptr;
        }

        if ((ret = av_hwframe_transfer_data(hwFrame, input, 0)) < 0) {
            SIP_CORE_ERR() << "Failed to push frame to GPU: " << libav_utils::getError(ret).c_str();
            return nullptr;
        }

        hwFrame->pts = input->pts; // transfer does not copy timestamp
        return framePtr;
    } else { // CODEC_NONE for example for OpenCL filters
        auto input = frame.pointer();
        if (not input)
            throw std::runtime_error("Cannot transfer null frame");

        auto desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(input->format));
        if (!desc) {
            throw std::runtime_error("Cannot transfer frame with invalid format");
        }

        if (desc->flags & AV_PIX_FMT_FLAG_HWACCEL) {
            if (input->format != format_) {
                SIP_CORE_ERR() << "Frame format mismatch: expected " << av_get_pix_fmt_name(format_)
                        << ", got "
                        << av_get_pix_fmt_name(static_cast<AVPixelFormat>(input->format));
                return nullptr;
            }

            return transferToMainMemory(frame, swFormat_);
        }
        else {
            if (input->format != swFormat_) {
                SIP_CORE_ERR() << "Frame format mismatch: expected " << av_get_pix_fmt_name(swFormat_)
                        << ", got "
                        << av_get_pix_fmt_name(static_cast<AVPixelFormat>(input->format));
                return nullptr;
            }

            auto framePtr = std::make_unique<VideoFrame>();
            auto hwFrame = framePtr->pointer();

            if (!deviceCtx_) {
                SIP_CORE_ERR() << "Cannot initialize hardware frames without a valid hardware device";
                return nullptr;
            }

            // Reuse a cached per-size frames context: allocating and
            // initializing a fresh GPU surface pool for every uploaded frame
            // costs far more than the upload itself.
            auto& pool = uploadPools_[{input->width, input->height}];
            if (!pool) {
                AVBufferRef* framesCtx = av_hwframe_ctx_alloc(deviceCtx_);
                if (!framesCtx)
                    return nullptr;

                auto ctx = reinterpret_cast<AVHWFramesContext*>(framesCtx->data);
                ctx->format = format_;
                ctx->sw_format = swFormat_;
                ctx->width = input->width;
                ctx->height = input->height;
                ctx->initial_pool_size = 0; // dynamic: sizes vary with layout

                if ((ret = av_hwframe_ctx_init(framesCtx)) < 0) {
                    SIP_CORE_ERR("Failed to initialize hardware frame context: %s (%d)",
                            libav_utils::getError(ret).c_str(),
                            ret);
                    av_buffer_unref(&framesCtx);
                    uploadPools_.erase({input->width, input->height});
                    return nullptr;
                }
                pool = framesCtx;
            }

            if ((ret = av_hwframe_get_buffer(pool, hwFrame, 0)) < 0) {
                SIP_CORE_ERR() << "Failed to allocate hardware buffer: "
                        << libav_utils::getError(ret).c_str();
                return nullptr;
            }

            if (!hwFrame->hw_frames_ctx) {
                SIP_CORE_ERR() << "Failed to allocate hardware buffer: Cannot allocate memory";
                return nullptr;
            }

            if ((ret = av_hwframe_transfer_data(hwFrame, input, 0)) < 0) {
                SIP_CORE_ERR() << "Failed to push frame to GPU: " << libav_utils::getError(ret).c_str();
                return nullptr;
            }

            hwFrame->pts = input->pts; // transfer does not copy timestamp
            return framePtr;
        }
    }
}

void
HardwareAccel::setDetails(AVCodecContext* codecCtx)
{
    codecCtx->hw_device_ctx = av_buffer_ref(deviceCtx_);
    if (type_ == CODEC_DECODER) {
        codecCtx->get_format = &getFormatCb;
        // codecCtx->thread_safe_callbacks = 1;
    } else if (type_ == CODEC_ENCODER) {
        if (framesCtx_)
            // encoder doesn't need a device context, only a frame context
            codecCtx->hw_frames_ctx = av_buffer_ref(framesCtx_);
    }
}

bool
HardwareAccel::initFrame()
{
    int ret = 0;
    if (!deviceCtx_) {
        SIP_CORE_ERR() << "Cannot initialize hardware frames without a valid hardware device";
        return false;
    }

    framesCtx_ = av_hwframe_ctx_alloc(deviceCtx_);
    if (!framesCtx_)
        return false;

    auto ctx = reinterpret_cast<AVHWFramesContext*>(framesCtx_->data);
    ctx->format = format_;
    ctx->sw_format = swFormat_;
    ctx->width = width_;
    ctx->height = height_;
    ctx->initial_pool_size = 20; // TODO try other values

    if ((ret = av_hwframe_ctx_init(framesCtx_)) < 0) {
        SIP_CORE_ERR("Failed to initialize hardware frame context: %s (%d)",
                 libav_utils::getError(ret).c_str(),
                 ret);
        av_buffer_unref(&framesCtx_);
    }

    return ret >= 0;
}

bool
HardwareAccel::linkHardware(AVBufferRef* framesCtx)
{
    if (framesCtx) {
        // Force sw_format to match swFormat_. Frame is never transferred to main
        // memory when hardware is linked, so the sw_format doesn't matter.
        auto hw = reinterpret_cast<AVHWFramesContext*>(framesCtx->data);
        hw->sw_format = swFormat_;

        if (framesCtx_)
            av_buffer_unref(&framesCtx_);
        framesCtx_ = av_buffer_ref(framesCtx);
        if ((linked_ = (framesCtx_ != nullptr))) {
            SIP_CORE_DBG() << "Hardware transcoding pipeline successfully set up for"
                       << " encoder '" << getCodecName() << "'";
        }
        return linked_;
    } else {
        return false;
    }
}

void
HardwareAccel::linkFilter(MediaStream& ms, int width, int height)
{
    if (!deviceCtx_) {
        SIP_CORE_ERR() << "Cannot link filter without a valid hardware device";
        return;
    }

    AVBufferRef* framesCtx;
    if (width == width_ && height == height_ && framesCtx_) {
        ms.deviceRef = av_buffer_ref(deviceCtx_);
        ms.frameRef = av_buffer_ref(framesCtx_);
        return;
    }

    framesCtx = av_hwframe_ctx_alloc(deviceCtx_);
    if (!framesCtx)
        return;

    auto ctx = reinterpret_cast<AVHWFramesContext*>(framesCtx->data);
    ctx->format = format_;
    ctx->sw_format = swFormat_;
    ctx->width = width;
    ctx->height = height;
    ctx->initial_pool_size = 20; // TODO try other values

    int ret;
    if ((ret = av_hwframe_ctx_init(framesCtx)) < 0) {
        SIP_CORE_ERR("Failed to initialize hardware frame context: %s (%d)",
                 libav_utils::getError(ret).c_str(),
                 ret);
        av_buffer_unref(&framesCtx);
        // ms.frameRef stays null; the filter graph init fails cleanly and the
        // mixer falls back to software mixing.
        return;
    }

    ms.deviceRef = av_buffer_ref(deviceCtx_);
    // transfer ownership of the alloc ref — the caller releases ms.frameRef
    ms.frameRef = framesCtx;
}

bool
HardwareAccel::reserveFrame(AVFrame* frame)
{
    if(!framesCtx_ && !initFrame())
        return false;

    int ret;
    if ((ret = av_hwframe_get_buffer(framesCtx_, frame, 0)) < 0) {
        SIP_CORE_ERR() << "Failed to allocate hardware buffer: "
                    << libav_utils::getError(ret).c_str();
        return false;
    }

    return true;
}

namespace {

// Shared lazy-download cache attached to published hardware frames through
// AVFrame.opaque_ref. av_frame_ref()/av_frame_copy_props() propagate a new
// reference to the same underlying buffer and av_frame_unref() drops it, so
// every consumer of a published frame (sink clients, recorder, mixer, encoder
// relay) sees the same cache and the GPU->CPU transfer runs at most once per
// published frame and requested format. Destroyed with the last frame ref.
struct SharedDownloadCache
{
    static constexpr uint32_t MAGIC = 0x53444331; // 'SDC1'
    uint32_t magic {MAGIC};
    std::mutex mtx;
    // One entry per requested software format (NV12 for sinks/mixer, the
    // stream format for the recorder); bounded, frames are transient.
    std::vector<std::pair<AVPixelFormat, std::shared_ptr<VideoFrame>>> entries;
};

constexpr size_t DOWNLOAD_CACHE_MAX_FORMATS = 4;

void
freeDownloadCache(void* /*opaque*/, uint8_t* data)
{
    delete reinterpret_cast<SharedDownloadCache*>(data);
}

SharedDownloadCache*
getDownloadCache(const AVFrame* frame)
{
    if (!frame->opaque_ref)
        return nullptr;
    auto* cache = reinterpret_cast<SharedDownloadCache*>(frame->opaque_ref->data);
    return (cache && cache->magic == SharedDownloadCache::MAGIC) ? cache : nullptr;
}

} // namespace

void
HardwareAccel::attachDownloadCache(AVFrame* frame)
{
    if (!frame)
        return;
    auto desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
    if (!desc || !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL))
        return;
    auto cache = std::make_unique<SharedDownloadCache>();
    AVBufferRef* ref = av_buffer_create(reinterpret_cast<uint8_t*>(cache.get()),
                                        sizeof(SharedDownloadCache),
                                        freeDownloadCache,
                                        nullptr,
                                        AV_BUFFER_FLAG_READONLY);
    if (!ref)
        return; // consumers fall back to per-consumer downloads
    cache.release();
    av_buffer_unref(&frame->opaque_ref); // every publish gets a fresh cache
    frame->opaque_ref = ref;
}

std::shared_ptr<VideoFrame>
HardwareAccel::ensureSoftwareFrame(const std::shared_ptr<VideoFrame>& frame,
                                   AVPixelFormat desired)
{
    if (!frame || !frame->pointer())
        return {};
    auto input = frame->pointer();
    auto desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(input->format));
    if (!desc)
        return {};
    if (!(desc->flags & AV_PIX_FMT_FLAG_HWACCEL))
        return frame;

    auto download = [&]() -> std::shared_ptr<VideoFrame> {
        try {
            return transferToMainMemory(*frame, desired);
        } catch (const std::runtime_error& e) {
            SIP_CORE_ERR("ensureSoftwareFrame: GPU download failed: %s", e.what());
            return {};
        }
    };

    auto* cache = getDownloadCache(input);
    if (!cache)
        return download(); // unpublished frame: plain one-off download

    // The transfer runs under the cache mutex so racing siblings wait for the
    // first download instead of duplicating it — bounded by the single
    // transfer each of them used to pay individually.
    std::lock_guard<std::mutex> lk(cache->mtx);
    for (const auto& entry : cache->entries)
        if (entry.first == desired)
            return entry.second;
    auto sw = download();
    if (sw && cache->entries.size() < DOWNLOAD_CACHE_MAX_FORMATS)
        cache->entries.emplace_back(desired, sw);
    return sw;
}

std::unique_ptr<VideoFrame>
HardwareAccel::transferToMainMemory(const VideoFrame& frame, AVPixelFormat desiredFormat)
{
    auto input = frame.pointer();
    if (not input)
        throw std::runtime_error("Cannot transfer null frame");

    auto desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(input->format));
    if (!desc) {
        throw std::runtime_error("Cannot transfer frame with invalid format");
    }

    auto out = std::make_unique<VideoFrame>();
    if (not(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
        out->copyFrom(frame);
        return out;
    }

    auto output = out->pointer();
    output->format = desiredFormat;

    int ret = av_hwframe_transfer_data(output, input, 0);
    if (ret < 0) {
        throw std::runtime_error("Cannot transfer the frame from GPU");
    }

    output->pts = input->pts;
    if (AVFrameSideData* side_data = av_frame_get_side_data(input, AV_FRAME_DATA_DISPLAYMATRIX))
        av_frame_new_side_data_from_buf(output,
                                        AV_FRAME_DATA_DISPLAYMATRIX,
                                        av_buffer_ref(side_data->buf));
    return out;
}

int
HardwareAccel::initAPI(bool linkable, AVBufferRef* framesCtx)
{
    std::string device;
    auto ret = init_device_type(device);
    if (ret == 0) {
        bool link = false;
        if (linkable && framesCtx)
            link = linkHardware(framesCtx);
        if (type_ == CODEC_NONE) {
            // Filter-only accel (OpenCL mixing) is useless without a frames
            // context; report failure so the caller can try the next API.
            return initFrame() ? 0 : -1;
        }
        // we don't need frame context for videotoolbox and decoders
        if (hwType_ == AV_HWDEVICE_TYPE_VIDEOTOOLBOX ||
                type_ == CODEC_DECODER || link || initFrame()) {
            return 0;
        }
    }
    return -1;
}

std::list<HardwareAccel>
HardwareAccel::getCompatibleAccel(AVCodecID id, int width, int height, CodecType type)
{
    std::list<HardwareAccel> l;

    const auto& list = (type == CODEC_ENCODER) ? &apiListEnc_ :
                       (type == CODEC_DECODER) ? &apiListDec_ :
                       &apiListOpencl_;
    for (auto& api : *list) {
        const auto& it = std::find(api.supportedCodecs.begin(), api.supportedCodecs.end(), id);
        if (it != api.supportedCodecs.end()) {
            auto hwtype = AV_HWDEVICE_TYPE_NONE;
            while ((hwtype = av_hwdevice_iterate_types(hwtype)) != AV_HWDEVICE_TYPE_NONE) {
                if (hwtype == api.hwType) {
                    auto accel = HardwareAccel(id,
                                               api.name,
                                               api.hwType,
                                               api.format,
                                               api.swFormat,
                                               type,
                                               api.dynBitrate);
                    accel.height_ = height;
                    accel.width_ = width;
                    accel.possible_devices_ = &api.possible_devices;
                    l.emplace_back(std::move(accel));
                }
            }
        }
    }
    return l;
}

bool
HardwareAccel::isGPUAvailable()
{
    static const bool available = [] {
        auto apis = getCompatibleAccel(AV_CODEC_ID_H264, 1280, 720, CODEC_DECODER);
        for (auto& api : apis) {
            if (api.initAPI(false, nullptr) >= 0) {
                SIP_CORE_INFO("GPU probe: %s is usable", api.getName().c_str());
                return true;
            }
        }
        SIP_CORE_WARN("GPU probe: no usable hardware acceleration device found");
        return false;
    }();
    return available;
}

} // namespace video
} // namespace sip_core
