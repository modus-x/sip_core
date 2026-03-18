/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
 *  Author: Guillaume Roguez <Guillaume.Roguez@savoirfairelinux.com>
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "rational.h"
#include "media_stream.h"
#include "media_device.h"
#include "media_io_handle.h"
#include "socket_pair.h"

extern "C" {
struct AVCodecContext;
struct AVStream;
struct AVDictionary;
struct AVFormatContext;
struct AVCodec;
enum AVMediaType;
}

namespace libsip_core {
class AudioFrame;
}

namespace sip_core {

using namespace std::chrono;

using AudioFrame = libsip_core::AudioFrame;
#ifdef ENABLE_VIDEO
using VideoFrame = libsip_core::VideoFrame;
#endif
struct AudioFormat;
class RingBuffer;
class Resampler;
class MediaIOHandle;
class MediaDecoder;

enum class DecodeStatus {
    Success,
    FrameFinished,
    EndOfFile,
    ReadBufferOverflow,
    ReadError,
    DecodeError,
    RestartRequired,
    FallBack
};

class MediaDecoderBase
{
public:
    virtual ~MediaDecoderBase() = default;
    virtual void emulateRate() = 0;

    virtual int openInput(const DeviceParams&) = 0;
    virtual void setInterruptCallback(int (*cb)(void*), void* opaque) = 0;
    virtual void setIOContext(MediaIOHandle* ioctx) = 0;
    virtual void enableLateFrameDrop(std::chrono::microseconds threshold) = 0;

    virtual int setup(AVMediaType type) = 0;
    virtual int setupAudio() = 0;
    virtual int setupVideo() = 0;

    virtual DecodeStatus decode() = 0;
    virtual DecodeStatus flush() = 0;

    virtual int getWidth() const = 0;
    virtual int getHeight() const = 0;
    virtual std::string getDecoderName() const = 0;

    virtual rational<double> getFps() const = 0;
    virtual AVPixelFormat getPixelFormat() const = 0;

    virtual void updateStartTime(int64_t startTime) = 0;

    virtual void emitFrame(bool isAudio) = 0;
    virtual void flushBuffers() = 0;
    virtual void setSeekTime(int64_t time) = 0;
#ifdef RING_ACCEL
    virtual void enableAccel(bool enableAccel) = 0;
#endif

    virtual MediaStream getStream(std::string name = "") const = 0;

    virtual void setResolutionChangedCallback(std::function<void(int, int)> cb) = 0;

    virtual void setFEC(bool enable) = 0;

    virtual void setContextCallback(const std::function<void()>& cb) = 0;

    static int get_rtp_packet_type(MediaIOHandle* ioctx, uint64_t timeout_ms)
    {
        if(!ioctx || !ioctx->getContext() || !ioctx->getContext()->opaque)
            return -1;
        
        auto sp = static_cast<SocketPair*>(ioctx->getContext()->opaque);

        auto start = std::chrono::steady_clock::now();
        auto ms = std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() - start < ms)
        {
            uint8_t buffer[256];
            auto len = sp->readData(buffer, sizeof(buffer));
            if(len < 0)
                return -1;
            else if(len == 0)
                continue;

            if(len < 14)
                return -1;

            if (len < 12 ||
                (buffer[0] >> 6) != 2)
                return -1;
            
            return buffer[1] & 0x7f;
        }
        return -1;
    }
};

} // namespace sip_core
