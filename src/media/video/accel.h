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

#pragma once

#include "libav_deps.h"
#include "media_codec.h"
#include "media_stream.h"

#include <memory>
#include <string>
#include <vector>
#include <list>
#include <map>
#include <utility>

extern "C" {
#include <libavutil/hwcontext.h>
}

namespace sip_core {
namespace video {


enum class DeviceState {
    NOT_TESTED,
    USABLE,
    NOT_USABLE
};

/**
 * @brief Provides an abstraction layer to the hardware acceleration APIs in FFmpeg.
 */
class HardwareAccel
{
public:
    /**
     * @brief Transfers hardware frame to main memory.
     *
     * Transfers a hardware decoded frame back to main memory. Should be called after
     * the frame is decoded using avcodec_send_packet/avcodec_receive_frame.
     *
     * If @frame is software, this is a no-op.
     *
     * @param frame Refrerence to the decoded hardware frame.
     * @param desiredFormat Software pixel format that the hardware outputs.
     * @returns Software frame.
     */
    static std::unique_ptr<VideoFrame> transferToMainMemory(const VideoFrame& frame,
                                                            AVPixelFormat desiredFormat);

    /**
     * @brief Attaches a shared lazy-download cache to a published hardware frame.
     *
     * Must be called by the producer before the frame fans out to consumers
     * (VideoGenerator::publishFrame does this). av_frame_ref propagates a new
     * reference to the same cache buffer via AVFrame.opaque_ref, so every
     * consumer of the published frame shares one cache. No-op for software
     * frames or on allocation failure (consumers then download individually).
     */
    static void attachDownloadCache(AVFrame* frame);

    /**
     * @brief Returns a software copy of @frame, downloading at most once.
     *
     * If @frame is already software it is returned unchanged. If it is a
     * hardware frame, the GPU->CPU transfer runs once per published frame and
     * format; sibling consumers (cropped conference sinks, recorder, mixer,
     * encoder relay) reuse the memoized download through the cache attached
     * by attachDownloadCache. Returns nullptr on download failure (logged) —
     * callers must drop the frame, never block or retry.
     *
     * The returned frame is shared between consumers: treat it as immutable.
     * Take a private ref (e.g. MediaFrame::copyFrom) before touching mutable
     * per-ref state such as pts or the crop fields.
     */
    static std::shared_ptr<VideoFrame> ensureSoftwareFrame(
        const std::shared_ptr<VideoFrame>& frame, AVPixelFormat desired = AV_PIX_FMT_NV12);

    /**
     * @brief Constructs a HardwareAccel object
     *
     * Made public so std::unique_ptr can access it. Should not be called.
     */
    HardwareAccel(AVCodecID id,
                  const std::string& name,
                  AVHWDeviceType hwType,
                  AVPixelFormat format,
                  AVPixelFormat swFormat,
                  CodecType type,
                  bool dynBitrate);

    /**
     * @brief Dereferences hardware contexts.
     */
    ~HardwareAccel();

    /**
     * @brief Codec that is being accelerated.
     */
    AVCodecID getCodecId() const { return id_; };

    /**
     * @brief Name of the hardware layer/API being used.
     */
    const std::string& getName() const { return name_; };

    /**
     * @brief Hardware format.
     */
    AVPixelFormat getFormat() const { return format_; };

    /**
     * @brief Software format.
     *
     * For encoding it is the format expected by the hardware. For decoding
     * it is the format output by the hardware.
     */
    AVPixelFormat getSoftwareFormat() const { return swFormat_; }

    /**
     * @brief Gets the name of the codec.
     *
     * Decoding: avcodec_get_name(id_)
     * Encoding: avcodec_get_name(id_) + '_' + name_
     */
    std::string getCodecName() const;

    /**
     * @brief If hardware decoder can feed hardware encoder directly.
     *
     * Returns whether or not the decoder is linked to an encoder or vice-versa. Being linked
     * means an encoder can directly use the decoder's hardware frame, without first
     * transferring it to main memory.
     */
    bool isLinked() const { return linked_; }

    /**
     * @brief Set some extra details in the codec context.
     *
     * Should be called after a successful
     * setup (setupDecoder or setupEncoder).
     * For decoding, sets the hw_device_ctx and get_format callback. If the decoder has
     * a frames context, mark as linked.
     * For encoding, sets hw_device_ctx and hw_frames_ctx, and may set some hardware
     * codec options.
     */
    void setDetails(AVCodecContext* codecCtx);

    /**
     * @brief Transfers a frame to/from the GPU memory.
     *
     * Transfers a hardware decoded frame back to main memory. Should be called after
     * the frame is decoded using avcodec_send_packet/avcodec_receive_frame or before
     * the frame is encoded using avcodec_send_frame/avcodec_receive_packet.
     *
     * @param frame Hardware frame when decoding, software frame when encoding.
     * @returns Software frame when decoding, hardware frame when encoding.
     */
    std::unique_ptr<VideoFrame> transfer(const VideoFrame& frame);

    /**
     * @brief Links this HardwareAccel's frames context with the passed in context.
     *
     * This serves to skip transferring a decoded frame back to main memory before encoding.
     */
    bool linkHardware(AVBufferRef* framesCtx);

    /**
     * @brief Links this HardwareAccel's frames context and device context
     *  with the passed in filter's MediaStream.
     *
     * This serves to skip transferring a decoded frame back to main memory before encoding.
     */
    void linkFilter(MediaStream& ms, int width, int height);

    /**
     * @brief Links given VideoFrame context with current HardwareAccel.
     *  
     *
     * This serves to be able to allocate new VideoFrame with given hw format.
     */
    bool reserveFrame(AVFrame* frame);

    static std::list<HardwareAccel> getCompatibleAccel(AVCodecID id,
                                                       int width,
                                                       int height,
                                                       CodecType type);

    /**
     * @brief Whether any hardware acceleration device can actually be opened
     * on this host. Probed once per process and cached. Reports false inside a
     * Windows Remote Desktop session (no usable GPU — see isRemoteSession).
     */
    static bool isGPUAvailable();

    /**
     * @brief Whether the process is running inside a Windows Remote Desktop
     * (Terminal Services) session. Always false on non-Windows. Under RDP the
     * physical GPU is not exposed (Microsoft Basic Render Driver), so hardware
     * decode/encode/OpenCL-mixing silently produce black or fall back to
     * software — callers use this to prefer the software path.
     */
    static bool isRemoteSession();

    /**
     * @brief The ACTUAL hardware-vs-software path of the live video pipeline.
     *
     * Unlike isGPUAvailable() (a one-shot *capability* probe), these report what
     * the most recently opened VIDEO decoder/encoder actually did:
     *  - HARDWARE when a HardwareAccel was successfully attached (accel_ != null),
     *  - SOFTWARE when it fell back to a CPU codec — e.g. NVENC/NVDEC failed to
     *    open, or the card has no encoder silicon at all (NVIDIA GM108 / 840M),
     *  - UNKNOWN before any video codec has opened this process.
     * The conference render-mode badge polls these (static, so they work from
     * the separate videoMixer-window engine on Windows/Linux) so it can never
     * claim "GPU" while frames are really going through libx264 / software
     * decode. Process-global, last-writer-wins, lock-free.
     */
    enum class AccelState { UNKNOWN, SOFTWARE, HARDWARE };
    static void setActiveDecodeState(AccelState s);
    static void setActiveEncodeState(AccelState s);
    static AccelState activeDecodeState();
    static AccelState activeEncodeState();

    /**
     * @brief Logs the host's video adapter(s) — names + vendor/device IDs — to
     * sip_core.log, once per process. Best-effort and never fatal: Linux reads
     * DRM sysfs nodes + /proc/driver/nvidia; Windows enumerates DXGI adapters;
     * macOS reads IOKit IOAccelerator names. Purely diagnostic — it explains
     * WHICH card a host has even when hardware acceleration then fails to open
     * on it (the "GPU but really CPU" case).
     */
    static void logSystemVideoAdapters();

    int initAPI(bool linkable, AVBufferRef* framesCtx);
    bool dynBitrate() { return dynBitrate_; }

private:
    bool initDevice(const std::string& device);
    bool initFrame();

    AVCodecID id_ {AV_CODEC_ID_NONE};
    std::string name_;
    AVHWDeviceType hwType_ {AV_HWDEVICE_TYPE_NONE};
    AVPixelFormat format_ {AV_PIX_FMT_NONE};
    AVPixelFormat swFormat_ {AV_PIX_FMT_NONE};
    CodecType type_ {CODEC_NONE};
    bool linked_ {false};
    int width_ {0};
    int height_ {0};
    bool dynBitrate_ {false};

    AVBufferRef* deviceCtx_ {nullptr};
    AVBufferRef* framesCtx_ {nullptr};
    // Cached per-size hardware upload pools for CODEC_NONE transfers
    // (conference mixer path); released in the destructor.
    std::map<std::pair<int, int>, AVBufferRef*> uploadPools_;

    struct HardwareAPI;
    static std::vector<HardwareAPI> apiListDec_;
    static std::vector<HardwareAPI> apiListEnc_;
    static std::vector<HardwareAPI> apiListOpencl_;

    int init_device(const char* name, const char* device, int flags);
    int init_device_type(std::string& dev);

    std::list<std::pair<std::string, DeviceState>>* possible_devices_;
};

} // namespace video
} // namespace sip_core
