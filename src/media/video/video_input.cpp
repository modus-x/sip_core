/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
 *  Author: Tristan Matthews <tristan.matthews@savoirfairelinux.com>
 *  Author: Vivien Didelot <vivien.didelot@savoirfairelinux.com>
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "video_input.h"

#include "media_decoder.h"
#include "media_const.h"
#include "manager.h"
#include "client/videomanager.h"
#include "client/ring_signal.h"
#include "sinkclient.h"
#include "logger.h"
#include "media/media_buffer.h"
#include "video_source_utils.h"

#include <libavformat/avio.h>

#include <chrono>
#include <string>
#include <sstream>
#include <cassert>
#include <cctype>
#ifdef _MSC_VER
#include <io.h> // for access
#else
#include <unistd.h>
#endif
extern "C" {
#include <libavutil/display.h>
}

#if defined(_WIN32) && defined(USE_DSHOW_SCREEN_CAPTURE)
#include <windows.h>
#include <Shlobj.h>
#include "Shlwapi.h"
#endif

#ifdef __APPLE__
#include <CoreGraphics/CoreGraphics.h>
#endif

namespace sip_core {
namespace video {

static constexpr unsigned default_grab_width = 640;
static constexpr unsigned default_grab_height = 480;
static constexpr auto kCameraStartupTimeout = std::chrono::seconds(10);

static int64_t
steadyClockNowUs() noexcept
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

#ifdef __APPLE__
// Calculate scaled dimensions that fit within maxWidth x maxHeight while preserving aspect ratio
static std::pair<unsigned, unsigned>
calculateScaledResolution(unsigned srcWidth,
                          unsigned srcHeight,
                          unsigned maxWidth,
                          unsigned maxHeight)
{
    if (srcWidth <= maxWidth && srcHeight <= maxHeight) {
        return {srcWidth, srcHeight};
    }

    float srcAspect = static_cast<float>(srcWidth) / srcHeight;
    float maxAspect = static_cast<float>(maxWidth) / maxHeight;

    unsigned targetWidth, targetHeight;
    if (srcAspect > maxAspect) {
        // Width is the limiting factor
        targetWidth = maxWidth;
        targetHeight = static_cast<unsigned>(maxWidth / srcAspect);
    } else {
        // Height is the limiting factor
        targetHeight = maxHeight;
        targetWidth = static_cast<unsigned>(maxHeight * srcAspect);
    }

    // Round to 8-pixel boundary for encoder compatibility
    targetWidth = (targetWidth >> 3) << 3;
    targetHeight = (targetHeight >> 3) << 3;

    return {targetWidth, targetHeight};
}
#endif

VideoInput::VideoInput(VideoInputMode inputMode, const std::string& id_)
    : VideoGenerator::VideoGenerator()
    , loop_(std::bind(&VideoInput::setup, this),
            std::bind(&VideoInput::process, this),
            std::bind(&VideoInput::cleanup, this))
{
    inputMode_ = inputMode;
    if (inputMode_ == VideoInputMode::Undefined) {
#if (defined(__ANDROID__) || defined(RING_UWP) || (defined(TARGET_OS_IOS) && TARGET_OS_IOS))
        inputMode_ = VideoInputMode::ManagedByClient;
#else
        inputMode_ = VideoInputMode::ManagedByDaemon;
#endif
    }
#ifdef __ANDROID__
    sink_ = Manager::instance().createSinkClient(id_);
#else
    if (inputMode_ == VideoInputMode::ManagedByDaemon) {
        sink_ = Manager::instance().createSinkClient(id_);
    }
#endif
    switchInput(id_);
}

VideoInput::~VideoInput()
{
    stopInput();
}

void
VideoInput::notifyCaptureStarted()
{
    if (decOpts_.input.empty()) {
        captureStartPending_.store(false);
        return;
    }

    captureStartPending_.store(false);
    if (!captureStarted_.exchange(true))
        emitSignal<libsip_core::VideoSignal::StartCapture>(decOpts_.input);
}

void
VideoInput::notifyCaptureStopped(bool force)
{
    const auto input = decOpts_.input;
    const bool pending = captureStartPending_.exchange(false);
    const bool started = captureStarted_.exchange(false);

    if ((force || pending || started) && !input.empty())
        emitSignal<libsip_core::VideoSignal::StopCapture>(input);
}

void
VideoInput::notifySetupFailed(bool stopCapture)
{
    if (stopCapture)
        notifyCaptureStopped(true);
    if (onFailedSetup_)
        onFailedSetup_(MEDIA_VIDEO);
}

void
VideoInput::startLoop()
{
    if (videoManagedByClient()) {
        switchDevice();
        return;
    }
    if (!loop_.isRunning()) {
        loop_.start();
    }
}

void
VideoInput::switchDevice()
{
    if (switchPending_.exchange(false)) {
        SIP_CORE_DBG("Switching input to '%s'", decOpts_.input.c_str());
        if (decOpts_.input.empty()) {
            capturing_ = false;
            return;
        }

        capturing_ = true;
    }
}

int
VideoInput::getWidth() const
{
    if (videoManagedByClient()) {
        return decOpts_.width;
    }
    return decoder_ ? decoder_->getWidth() : 0;
}

int
VideoInput::getHeight() const
{
    if (videoManagedByClient()) {
        return decOpts_.height;
    }
    return decoder_ ? decoder_->getHeight() : 0;
}

AVPixelFormat
VideoInput::getPixelFormat() const
{
    if (!videoManagedByClient()) {
        return decoder_->getPixelFormat();
    }
    return (AVPixelFormat) std::stoi(decOpts_.format);
}

void
VideoInput::setRotation(int angle)
{
    std::shared_ptr<AVBufferRef> displayMatrix {av_buffer_alloc(sizeof(int32_t) * 9),
                                                [](AVBufferRef* buf) {
                                                    av_buffer_unref(&buf);
                                                }};
    if (displayMatrix) {
        av_display_rotation_set(reinterpret_cast<int32_t*>(displayMatrix->data), angle);
        displayMatrix_ = std::move(displayMatrix);
    }
}

bool
VideoInput::setup()
{
    if (not attach(sink_.get())) {
        SIP_CORE_ERR("attach sink failed");
        return false;
    }

    if (!sink_->start())
        SIP_CORE_ERR("start sink failed");

    SIP_CORE_DBG("VideoInput ready to capture");

    return true;
}

void
VideoInput::process()
{
    if (playingFile_) {
        if (paused_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            return;
        }
        decoder_->emitFrame(false);
        return;
    }
    // ALWAYS called on first VideoInput creation
    if (switchPending_)
        createDecoder();

    if (not captureFrame()) {
        loop_.stop();
        return;
    }
}

void
VideoInput::setSeekTime(int64_t time)
{
    if (decoder_) {
        decoder_->setSeekTime(time);
    }
}

void
VideoInput::cleanup()
{
    deleteDecoder(); // do it first to let a chance to last frame to be displayed
    notifyCaptureStopped();
    stopSink();
    SIP_CORE_DBG("VideoInput closed");
}

bool
VideoInput::captureFrame()
{
    // Return true if capture could continue, false if must be stop
    if (not decoder_)
        return false;

    switch (decoder_->decode()) {
    case MediaDemuxer::Status::EndOfFile:
        // Before attempting to recreate decoder, check if device is still available
        // For camera devices, verify the device hasn't been disconnected
        if (decOpts_.format == "video4linux2" || decOpts_.format == "dshow") {
            if (!sip_core::getVideoDeviceMonitor().deviceExists(decOpts_.unique_id)) {
                SIP_CORE_WARN("Device \"%s\" disconnected during capture, stopping",
                              decOpts_.unique_id.c_str());
                sip_core::getVideoDeviceMonitor().removeDeviceViaInput(decOpts_.unique_id.empty()
                                                                           ? decOpts_.input
                                                                           : decOpts_.unique_id);
                return false;
            }
        }
        createDecoder();
        return static_cast<bool>(decoder_);
    case MediaDemuxer::Status::ReadError:
        SIP_CORE_ERR() << "Failed to decode frame";
        // For repeated read errors, check if device still exists
        if (decOpts_.format == "video4linux2" || decOpts_.format == "dshow") {
            if (!sip_core::getVideoDeviceMonitor().deviceExists(decOpts_.unique_id)) {
                SIP_CORE_WARN("Device \"%s\" disconnected (read error), stopping",
                              decOpts_.unique_id.c_str());
                sip_core::getVideoDeviceMonitor().removeDeviceViaInput(decOpts_.unique_id.empty()
                                                                           ? decOpts_.input
                                                                           : decOpts_.unique_id);
                return false;
            }
        }
        // try again to decode
        return true;
    default:
        return true;
    }
}
void
VideoInput::flushBuffers()
{
    if (decoder_) {
        decoder_->flushBuffers();
    }
}

void
VideoInput::configureFilePlayback(const std::string&,
                                  std::shared_ptr<MediaDemuxer>& demuxer,
                                  int index)
{
    deleteDecoder();
    clearOptions();

    auto decoder = std::make_unique<MediaDecoder>(demuxer,
                                                  index,
                                                  [this](std::shared_ptr<MediaFrame>&& frame) {
                                                      publishFrame(
                                                          std::static_pointer_cast<VideoFrame>(
                                                              frame));
                                                  });
    decoder->setInterruptCallback(
        [](void* data) -> int { return static_cast<VideoInput*>(data)->shouldInterruptDecoderIo(); },
        this);
    decoder->emulateRate();

    decoder_ = std::move(decoder);
    playingFile_ = true;
    loop_.start();

    /* Signal the client about readable sink */
    sink_->setFrameSize(decoder_->getWidth(), decoder_->getHeight());
}

#ifdef WIN32
BOOL CALLBACK
EnumWindowsProcMy(HWND hwnd, LPARAM lParam)
{
    std::pair<DWORD, std::string>* dataPair = reinterpret_cast<std::pair<DWORD, std::string>*>(
        lParam);
    DWORD lpdwProcessId;
    if (auto parent = GetWindow(hwnd, GW_OWNER))
        GetWindowThreadProcessId(parent, &lpdwProcessId);
    else
        GetWindowThreadProcessId(hwnd, &lpdwProcessId);
    int len = GetWindowTextLength(hwnd) + 1;
    std::vector<wchar_t> buf(len);
    GetWindowText(hwnd, &buf[0], len);

    if (lpdwProcessId == dataPair->first) {
        if (!IsWindowVisible(hwnd))
            return TRUE;
        dataPair->second = to_string(&buf[0]);
        return FALSE;
    }
    return TRUE;
}
#endif

void
VideoInput::setRecorderCallback(const std::function<void(const MediaStream& ms)>& cb)
{
    recorderCallback_ = cb;
    if (decoder_)
        decoder_->setContextCallback([this]() {
            if (recorderCallback_)
                recorderCallback_(getInfo());
        });
}

void
VideoInput::createDecoder()
{
    deleteDecoder();

    switchPending_ = false;
    startupAbortReason_.store(StartupAbortReason::None);
    clearStartupDeadline();

    // we cannot create decoder without depOpts_!
    if (decOpts_.input.empty()) {
        foundDecOpts(decOpts_);
        return;
    }

    // create decoder without demuxer, height, width, only with callback
    auto decoder = std::make_unique<MediaDecoder>(
        // this callback will notify listeners
        [this](const std::shared_ptr<MediaFrame>& frame) mutable {
            publishFrame(std::static_pointer_cast<VideoFrame>(frame));
        });

    if (emulateRate_)
        decoder->emulateRate();

    if (decOpts_.format == "video4linux2") {
        decoder->enableLateFrameDrop(std::chrono::milliseconds(200));
    }

    decoder->setInterruptCallback(
        [](void* data) -> int { return static_cast<VideoInput*>(data)->shouldInterruptDecoderIo(); },
        this);

    const bool useStartupTimeout = (decOpts_.format == "video4linux2" || decOpts_.format == "dshow");
    if (useStartupTimeout) {
        setStartupDeadline(std::chrono::steady_clock::now() + kCameraStartupTimeout);
    }

    bool ready = false, restartSink = false;
    if (decOpts_.format == "x11grab" && !decOpts_.is_area) {
        decOpts_.width = 0;
        decOpts_.height = 0;
    }

    int tries = 0;
    int busyTries = 0;
    constexpr int maxBusyTries = 50; // 50 * 100ms = 5 seconds max for EBUSY

    while (!ready && !isStopped_) {
        if (useStartupTimeout && isStartupDeadlineExceeded()) {
            startupAbortReason_.store(StartupAbortReason::Timeout);
            SIP_CORE_ERR("Timeout while starting camera input \"%s\" after %lld ms",
                         decOpts_.input.c_str(),
                         static_cast<long long>(
                             std::chrono::duration_cast<std::chrono::milliseconds>(
                                 kCameraStartupTimeout)
                                 .count()));
            foundDecOpts(decOpts_);
            clearStartupDeadline();
            if (decOpts_.format == "video4linux2" || decOpts_.format == "dshow") {
                sip_core::getVideoDeviceMonitor().removeDeviceViaInput(decOpts_.unique_id.empty()
                                                                           ? decOpts_.input
                                                                           : decOpts_.unique_id);
            }
            notifySetupFailed();
            return;
        }

        // For camera devices, check if the device still exists before retrying
        if (decOpts_.format == "video4linux2" || decOpts_.format == "dshow") {
            if (!sip_core::getVideoDeviceMonitor().deviceExists(decOpts_.unique_id)) {
                SIP_CORE_WARN("Device \"%s\" disconnected, stopping input",
                              decOpts_.unique_id.c_str());
                foundDecOpts(decOpts_);
                clearStartupDeadline();
                sip_core::getVideoDeviceMonitor().removeDeviceViaInput(decOpts_.unique_id.empty()
                                                                           ? decOpts_.input
                                                                           : decOpts_.unique_id);
                notifySetupFailed();
                return;
            }
        }

        // Retry to open the video till the input is opened
        auto ret = decoder->openInput(decOpts_);
        ready = ret >= 0;
        if (ret < 0 && -ret != EBUSY) {
            tries += 1;
            if (tries <= 10) {
                SIP_CORE_ERR("Could not open input \"%s\" with status %i, trying again",
                             decOpts_.input.c_str(),
                             ret);
            } else {
                foundDecOpts(decOpts_);
                clearStartupDeadline();
                if (decOpts_.format == "video4linux2" || decOpts_.format == "dshow") {
                    sip_core::getVideoDeviceMonitor().removeDeviceViaInput(decOpts_.unique_id.empty()
                                                                               ? decOpts_.input
                                                                               : decOpts_.unique_id);
                }
                notifySetupFailed();
                return;
            }
        } else if (-ret == EBUSY) {
            // If the device is busy, this means that it can be used by another call.
            // If this is the case, cleanup() can occur and this will erase shmPath_
            // So, be sure to regenerate a correct shmPath for clients.
            restartSink = true;
            busyTries += 1;
            if (busyTries > maxBusyTries) {
                SIP_CORE_ERR("Device \"%s\" busy for too long, giving up", decOpts_.input.c_str());
                foundDecOpts(decOpts_);
                clearStartupDeadline();
                notifySetupFailed();
                return;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (isStopped_) {
        startupAbortReason_.store(StartupAbortReason::StopRequested);
        clearStartupDeadline();
        notifyCaptureStopped();
        return;
    }

    if (restartSink) {
        sink_->start();
    }

    // by this time our successfully opened input is producing some output
    // in this function we wait for demux stream, get its id, set callback for demuxer (pass
    // compressed frames to decoder), then setup decoder context (copy paramaters from found stream)
    if (decoder->setupVideo() < 0) {
        auto abortReason = startupAbortReason_.load();
        if (abortReason == StartupAbortReason::Timeout) {
            SIP_CORE_ERR("Camera startup timed out while probing stream info for \"%s\"",
                         decOpts_.input.c_str());
        } else if (abortReason == StartupAbortReason::StopRequested || isStopped_) {
            SIP_CORE_DBG("Camera startup interrupted by stop request for \"%s\"",
                         decOpts_.input.c_str());
        } else {
            SIP_CORE_ERR("decoder IO startup failed");
        }
        foundDecOpts(decOpts_);
        clearStartupDeadline();
        if (decOpts_.format == "video4linux2" || decOpts_.format == "dshow") {
            sip_core::getVideoDeviceMonitor().removeDeviceViaInput(decOpts_.unique_id.empty()
                                                                       ? decOpts_.input
                                                                       : decOpts_.unique_id);
        }
        notifySetupFailed();
        return;
    }
    clearStartupDeadline();

    auto ret = decoder->decode(); // Populate AVCodecContext fields
    if (ret == MediaDemuxer::Status::ReadError) {
        SIP_CORE_INFO() << "Decoder error";
        foundDecOpts(decOpts_);
        notifySetupFailed();
        return;
    }

    decOpts_.width = ((decoder->getWidth() >> 3) << 3);
    decOpts_.height = ((decoder->getHeight() >> 3) << 3);
    decOpts_.framerate = decoder->getFps();
    AVPixelFormat fmt = decoder->getPixelFormat();
    if (fmt != AV_PIX_FMT_NONE) {
        decOpts_.pixel_format = av_get_pix_fmt_name(fmt);
    } else {
        SIP_CORE_WARN("Could not determine pixel format, using default");
        decOpts_.pixel_format = av_get_pix_fmt_name(AV_PIX_FMT_YUV420P);
    }

    SIP_CORE_DBG("created decoder with video params : size=%dX%d, fps=%lf pix=%s",
                 decOpts_.width,
                 decOpts_.height,
                 decOpts_.framerate.real(),
                 decOpts_.pixel_format.c_str());
    decoder_ = std::move(decoder);

    foundDecOpts(decOpts_);

    /* Signal the client about readable sink */
    sink_->setFrameSize(decoder_->getWidth(), decoder_->getHeight());
    if (onSuccessfulSetup_)
        onSuccessfulSetup_(MEDIA_VIDEO, 0);
    notifyCaptureStarted();

    decoder_->setContextCallback([this]() {
        if (recorderCallback_)
            recorderCallback_(getInfo());
    });
}

void
VideoInput::deleteDecoder()
{
    if (not decoder_)
        return;
    flushFrames();
    decoder_.reset();
}

void
VideoInput::stopInput()
{
    notifyCaptureStopped();

    isStopped_ = true;
    startupAbortReason_.store(StartupAbortReason::StopRequested);
    if (videoManagedByClient()) {
        capturing_ = false;
        clearStartupDeadline();
        return;
    }
    loop_.join();
    clearStartupDeadline();

    clearOptions();
}

void
VideoInput::startInput()
{
    if (decOpts_.input.empty() && !currentResource_.empty() && !switchPending_.load()) {
        // Restart using the last known resource when options were cleared.
        switchInput(currentResource_);
        return;
    }

    isStopped_ = false;
    captureStartPending_.store(!decOpts_.input.empty());

    startLoop();
    if (videoManagedByClient())
        notifyCaptureStarted();
}

void
VideoInput::clearOptions()
{
    decOpts_ = {};
    emulateRate_ = false;
    captureStartPending_.store(false);
}

bool
VideoInput::isCapturing() const noexcept
{
    if (videoManagedByClient()) {
        return capturing_;
    }
    return loop_.isRunning();
}

bool
VideoInput::shouldInterruptDecoderIo() noexcept
{
    if (!isCapturing() || isStopped_) {
        auto expected = StartupAbortReason::None;
        startupAbortReason_.compare_exchange_strong(expected, StartupAbortReason::StopRequested);
        return true;
    }

    if (isStartupDeadlineExceeded()) {
        startupAbortReason_.store(StartupAbortReason::Timeout);
        return true;
    }

    return false;
}

bool
VideoInput::isStartupDeadlineExceeded() const noexcept
{
    const auto deadlineUs = startupDeadlineUs_.load();
    return deadlineUs > 0 && steadyClockNowUs() >= deadlineUs;
}

void
VideoInput::setStartupDeadline(std::chrono::steady_clock::time_point deadline) noexcept
{
    startupDeadlineUs_.store(
        std::chrono::duration_cast<std::chrono::microseconds>(deadline.time_since_epoch()).count());
}

void
VideoInput::clearStartupDeadline() noexcept
{
    startupDeadlineUs_.store(0);
}

bool
VideoInput::initCamera(const std::string& device)
{
    decOpts_ = sip_core::getVideoDeviceMonitor().getDeviceParams(device);
    if (decOpts_.input.empty()) {
        SIP_CORE_WARN("No video device parameters found for \"%s\"", device.c_str());
        return false;
    }
#if defined(_WIN32) && defined(USE_DSHOW_SCREEN_CAPTURE)
    if (decOpts_.name == "screen-capture-recorder") {
        // screen-capture-recorder plugin can appear
        // in the list of available cameras.
        // Initialize it explicitly in this case.
        initScreenCaptureRecorder(device);
    }
#endif
    return true;
}

static constexpr unsigned
round2pow(unsigned i, unsigned n)
{
    return (i >> n) << n;
}

#if !defined(_WIN32)
bool
VideoInput::initX11(const std::string& display)
{
    // Patterns
    // full screen sharing : :1+0,0 2560x1440 - SCREEN 1, POSITION 0X0, RESOLUTION 2560X1440
    // area sharing : :1+882,211 1532x779 - SCREEN 1, POSITION 882x211, RESOLUTION 1532x779
    // window sharing : :+1,0 0x0 window-id:0x0340021e - POSITION 0X0
    size_t space = display.find(' ');
    std::string windowIdStr = "window-id:";
    size_t winIdPos = display.find(windowIdStr);

    DeviceParams p = sip_core::getVideoDeviceMonitor().getDeviceParams(DEVICE_DESKTOP);
    if (winIdPos != std::string::npos) {
        p.window_id = display.substr(winIdPos + windowIdStr.size()); // "0x0340021e";
        p.is_area = 0;
    }
    if (space != std::string::npos) {
        p.input = display.substr(0, space);
        if (p.window_id.empty()) {
            SIP_CORE_INFO() << "p.window_id.empty()";
            auto splits = sip_core::split_string_to_unsigned(display.substr(space + 1), 'x');
            // round to 8 pixel block
            p.width = round2pow(splits[0], 3);
            p.height = round2pow(splits[1], 3);
            p.is_area = 1;
        }
    } else {
        p.input = display;
        p.width = 0;
        p.height = 0;
        p.is_area = 1;
    }

    auto dec = std::make_unique<MediaDecoder>();
    if (dec->openInput(p) < 0 || dec->setupVideo() < 0)
        return initCamera(sip_core::getVideoDeviceMonitor().getDefaultDevice());

    clearOptions();
    decOpts_ = p;
    decOpts_.width = round2pow(dec->getStream().width, 3);
    decOpts_.height = round2pow(dec->getStream().height, 3);

    return true;
}
#endif

#ifdef __APPLE__
bool
VideoInput::initAVFoundation(const std::string& display)
{
    size_t space = display.find(' ');

    clearOptions();
    decOpts_.format = "avfoundation";
    decOpts_.pixel_format = "bgr0";
    decOpts_.name = "Capture screen 0";
    decOpts_.input = "Capture screen 0";
    decOpts_.framerate = sip_core::getVideoDeviceMonitor().getDeviceParams(DEVICE_DESKTOP).framerate;

    // Get actual screen dimensions using CoreGraphics
    CGDirectDisplayID mainDisplay = CGMainDisplayID();
    size_t screenWidth = CGDisplayPixelsWide(mainDisplay);
    size_t screenHeight = CGDisplayPixelsHigh(mainDisplay);

    // Parse user-provided dimensions if available (overrides detected screen size)
    if (space != std::string::npos) {
        std::istringstream iss(display.substr(space + 1));
        char sep;
        unsigned w, h;
        iss >> w >> sep >> h;
        if (w > 0 && h > 0) {
            screenWidth = w;
            screenHeight = h;
        }
    }

    // Cap at 1920x1080 maximum to prevent lag on large displays
    constexpr unsigned MAX_CAPTURE_WIDTH = 1920;
    constexpr unsigned MAX_CAPTURE_HEIGHT = 1080;

    auto [targetWidth, targetHeight] = calculateScaledResolution(static_cast<unsigned>(screenWidth),
                                                                 static_cast<unsigned>(screenHeight),
                                                                 MAX_CAPTURE_WIDTH,
                                                                 MAX_CAPTURE_HEIGHT);

    decOpts_.width = targetWidth;
    decOpts_.height = targetHeight;

    SIP_CORE_DBG("initAVFoundation: screen %zux%zu -> target %ux%u",
                 screenWidth,
                 screenHeight,
                 targetWidth,
                 targetHeight);

    return true;
}
#endif

bool
VideoInput::initWindowsCapture(const std::string& params)
{
    clearOptions();
    decOpts_ = sip_core::getVideoDeviceMonitor().getDeviceParams(DEVICE_DESKTOP);

    const std::string sourceStr = "source:";
    const size_t sourcePos = params.find(sourceStr);
    if (sourcePos != std::string::npos
        && (sourcePos == 0 || std::isspace(static_cast<unsigned char>(params[sourcePos - 1])))) {
        const size_t sourceStart = sourcePos + sourceStr.size();
        size_t sourceEnd = params.find_first_of(" ,\t\r\n", sourceStart);
        if (sourceEnd == std::string::npos)
            sourceEnd = params.size();

        auto source = params.substr(sourceStart, sourceEnd - sourceStart);
        // Some UIs include decorations like "source:1- SCREEN ..."; keep only the actual id.
        while (!source.empty() && !std::isalnum(static_cast<unsigned char>(source.back())))
            source.pop_back();
        decOpts_.window_id = std::move(source); // e.g. "0x0340021e" or "0"
    }

    auto parseSize = [&](unsigned& outW, unsigned& outH) -> bool {
        // Find a "<digits>x<digits>" pattern (avoid matching "0x..." hex prefixes).
        for (size_t i = 0; i < params.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(params[i])))
                continue;
            if (i > 0
                && (params[i - 1] == '+'
                    || !std::isspace(static_cast<unsigned char>(params[i - 1]))))
                continue;
            size_t j = i;
            while (j < params.size() && std::isdigit(static_cast<unsigned char>(params[j])))
                ++j;
            if (j >= params.size() || params[j] != 'x')
                continue;
            const size_t xPos = j;
            ++j;
            if (j >= params.size() || !std::isdigit(static_cast<unsigned char>(params[j])))
                continue;
            size_t k = j;
            while (k < params.size() && std::isdigit(static_cast<unsigned char>(params[k])))
                ++k;

            try {
                outW = static_cast<unsigned>(std::stoul(params.substr(i, xPos - i)));
                outH = static_cast<unsigned>(std::stoul(params.substr(j, k - j)));
                return true;
            } catch (...) {
                return false;
            }
        }
        return false;
    };

    auto parseOffset = [&](int& outX, int& outY) -> bool {
        const size_t plusPos = params.find('+');
        if (plusPos == std::string::npos)
            return false;
        size_t i = plusPos + 1;
        if (i >= params.size() || !std::isdigit(static_cast<unsigned char>(params[i])))
            return false;
        size_t j = i;
        while (j < params.size() && std::isdigit(static_cast<unsigned char>(params[j])))
            ++j;
        if (j >= params.size() || params[j] != 'x')
            return false;
        const size_t xPos = j;
        ++j;
        if (j >= params.size() || !std::isdigit(static_cast<unsigned char>(params[j])))
            return false;
        size_t k = j;
        while (k < params.size() && std::isdigit(static_cast<unsigned char>(params[k])))
            ++k;
        try {
            outX = static_cast<int>(std::stol(params.substr(i, xPos - i)));
            outY = static_cast<int>(std::stol(params.substr(j, k - j)));
            return true;
        } catch (...) {
            return false;
        }
    };

    unsigned w {}, h {};
    if (parseSize(w, h)) {
        decOpts_.width = round2pow(w, 3);
        decOpts_.height = round2pow(h, 3);

        int offX {}, offY {};
        if (parseOffset(offX, offY)) {
            decOpts_.offset_x = offX;
            decOpts_.offset_y = offY;
        }
    }

    auto dec = std::make_unique<MediaDecoder>();

    if (dec->openInput(decOpts_) < 0 || dec->setupVideo() < 0)
        return initCamera(sip_core::getVideoDeviceMonitor().getDefaultDevice());

    // Always reflect the actual opened stream properties.
    decOpts_.width = round2pow(dec->getStream().width, 3);
    decOpts_.height = round2pow(dec->getStream().height, 3);

    return true;
}

#if defined(_WIN32) && defined(USE_DSHOW_SCREEN_CAPTURE)
bool
VideoInput::initScreenCaptureRecorder(const std::string& params)
{
    // Paterns
    // capture area : 1920x1080 - SCREEN 0, POSITION 0X0, RESOLUTION 1920x1080
    // capture area with offset : 1920x1080 +28x28 - SCREEN 0, POSITION 28x28, RESOLUTION 1920x1080
    // capture non default screen : 1920x1080 +28x28  source:1- SCREEN 1, POSITION 28x28, RESOLUTION
    // 1920x1080 capture window : source:0x0340021e

    clearOptions();
    decOpts_ = sip_core::getVideoDeviceMonitor().getDeviceParams(DEVICE_DESKTOP);

    TCHAR appDataPath[MAX_PATH];
    if (FAILED(SHGetFolderPath(NULL, CSIDL_APPDATA, NULL, SHGFP_TYPE_CURRENT, appDataPath))) {
        return false;
    }
    PathAppend(appDataPath, TEXT("ScreenCaptureRecorder.ini"));
    WritePrivateProfileString(TEXT("all_settings"),
                              NULL,
                              NULL,
                              appDataPath); // clear all section content

    auto writeIntSetting = [&](const wchar_t* key, int value) {
        wchar_t buf[16];
        swprintf(buf, 16, L"%d", value);
        WritePrivateProfileString(TEXT("all_settings"), key, buf, appDataPath);
    };

    const std::string sourceStr = "source:";
    const size_t sourcePos = params.find(sourceStr);
    if (sourcePos != std::string::npos
        && (sourcePos == 0 || std::isspace(static_cast<unsigned char>(params[sourcePos - 1])))) {
        const size_t sourceStart = sourcePos + sourceStr.size();
        size_t sourceEnd = params.find_first_of(" ,\t\r\n", sourceStart);
        if (sourceEnd == std::string::npos)
            sourceEnd = params.size();

        auto source = params.substr(sourceStart, sourceEnd - sourceStart);
        while (!source.empty() && !std::isalnum(static_cast<unsigned char>(source.back())))
            source.pop_back();

        if (not source.empty()) {
            std::wstring wsSource(source.begin(), source.end());
            if (source.rfind("0x", 0) == 0) { // starts with
                WritePrivateProfileString(TEXT("all_settings"),
                                          TEXT("hwnd_to_track"),
                                          wsSource.c_str(),
                                          appDataPath);
            } else {
                WritePrivateProfileString(TEXT("all_settings"),
                                          TEXT(
                                              "capture_particular_display_number_starting_at_zero"),
                                          wsSource.c_str(),
                                          appDataPath);
            }
        }
    }

    auto parseSize = [&](unsigned& outW, unsigned& outH) -> bool {
        // Find a "<digits>x<digits>" pattern (avoid matching "0x..." hex prefixes).
        for (size_t i = 0; i < params.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(params[i])))
                continue;
            if (i > 0
                && (params[i - 1] == '+'
                    || !std::isspace(static_cast<unsigned char>(params[i - 1]))))
                continue;
            size_t j = i;
            while (j < params.size() && std::isdigit(static_cast<unsigned char>(params[j])))
                ++j;
            if (j >= params.size() || params[j] != 'x')
                continue;
            const size_t xPos = j;
            ++j;
            if (j >= params.size() || !std::isdigit(static_cast<unsigned char>(params[j])))
                continue;
            size_t k = j;
            while (k < params.size() && std::isdigit(static_cast<unsigned char>(params[k])))
                ++k;
            try {
                outW = static_cast<unsigned>(std::stoul(params.substr(i, xPos - i)));
                outH = static_cast<unsigned>(std::stoul(params.substr(j, k - j)));
                return true;
            } catch (...) {
                return false;
            }
        }
        return false;
    };

    auto parseOffset = [&](int& outX, int& outY) -> bool {
        const size_t plusPos = params.find('+');
        if (plusPos == std::string::npos)
            return false;
        size_t i = plusPos + 1;
        if (i >= params.size() || !std::isdigit(static_cast<unsigned char>(params[i])))
            return false;
        size_t j = i;
        while (j < params.size() && std::isdigit(static_cast<unsigned char>(params[j])))
            ++j;
        if (j >= params.size() || params[j] != 'x')
            return false;
        const size_t xPos = j;
        ++j;
        if (j >= params.size() || !std::isdigit(static_cast<unsigned char>(params[j])))
            return false;
        size_t k = j;
        while (k < params.size() && std::isdigit(static_cast<unsigned char>(params[k])))
            ++k;
        try {
            outX = static_cast<int>(std::stol(params.substr(i, xPos - i)));
            outY = static_cast<int>(std::stol(params.substr(j, k - j)));
            return true;
        } catch (...) {
            return false;
        }
    };

    unsigned w {}, h {};
    if (parseSize(w, h)) {
        decOpts_.width = round2pow(w, 3);
        decOpts_.height = round2pow(h, 3);

        int offX {}, offY {};
        if (parseOffset(offX, offY)) {
            decOpts_.offset_x = offX;
            decOpts_.offset_y = offY;
        }
    } else {
        auto dec = std::make_unique<MediaDecoder>();

        if (dec->openInput(decOpts_) < 0 || dec->setupVideo() < 0)
            return initCamera(sip_core::getVideoDeviceMonitor().getDefaultDevice());

        decOpts_.width = round2pow(dec->getStream().width, 3);
        decOpts_.height = round2pow(dec->getStream().height, 3);
    }

    // Ensure a stable output size even if a tracked window is resized.
    writeIntSetting(TEXT("capture_height"), static_cast<int>(decOpts_.height));
    writeIntSetting(TEXT("capture_width"), static_cast<int>(decOpts_.width));
    writeIntSetting(TEXT("start_x"), decOpts_.offset_x);
    writeIntSetting(TEXT("start_y"), decOpts_.offset_y);

    return true;
}
#endif

bool
VideoInput::initFile(std::string path)
{
    size_t dot = path.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : path.substr(dot + 1);

    /* File exists? */
    if (access(path.c_str(), R_OK) != 0) {
        SIP_CORE_ERR("file '%s' unavailable\n", path.c_str());
        return false;
    }

    // check if file has video, fall back to default device if none
    // FIXME the way this is done is hackish, but it can't be done in createDecoder because that
    // would break the promise returned in switchInput
    DeviceParams p;
    p.input = path;
    p.name = path;
    auto dec = std::make_unique<MediaDecoder>();
    if (dec->openInput(p) < 0 || dec->setupVideo() < 0) {
        return initCamera(sip_core::getVideoDeviceMonitor().getDefaultDevice());
    }

    clearOptions();
    emulateRate_ = true;
    decOpts_.input = path;
    decOpts_.name = path;
    decOpts_.loop = "1";

    // Force 1fps for static image
    if (ext == "jpeg" || ext == "jpg" || ext == "png") {
        decOpts_.format = "image2";
        decOpts_.framerate = 1;
    } else {
        SIP_CORE_WARN("Guessing file type for %s", path.c_str());
    }

    return false;
}

void
VideoInput::restart()
{
    if (loop_.isStopping()) {
        switchInput(currentResource_);
    }
}

std::shared_future<DeviceParams>
VideoInput::switchInput(const std::string& resource)
{
    const auto normalizedResource = normalizeVideoSwitchSource(resource);
    SIP_CORE_DBG("MRL: '%s'", normalizedResource.c_str());

    decOptsFound_ = false;

    // FUTURE of promise can be listened!
    std::promise<DeviceParams> p;
    foundDecOpts_.swap(p);

    // Switch off video input?
    if (normalizedResource.empty()) {
        currentResource_ = normalizedResource;
        // some default params
        foundDecOpts(DeviceParams {});
        futureDecOpts_ = foundDecOpts_.get_future().share();
        stopInput();
        clearOptions();
        return futureDecOpts_;
    }

    // Supported MRL schemes
    static const std::string sep = libsip_core::Media::VideoProtocolPrefix::SEPARATOR;

    const auto pos = normalizedResource.find(sep);
    if (pos == std::string::npos)
        return {};

    const auto prefix = normalizedResource.substr(0, pos);
    if ((pos + sep.size()) >= normalizedResource.size())
        return {};

    const auto suffix = normalizedResource.substr(pos + sep.size());

    // if already is true -> skip
    if (switchPending_.exchange(true)) {
        SIP_CORE_ERR("Video switch already requested");
        return {};
    }

    const auto previousResource = currentResource_;
    const auto previousDecOpts = decOpts_;
    const auto previousEmulateRate = emulateRate_;

    currentResource_ = normalizedResource;

    bool ready = false;
    bool recognized = false;

    if (prefix == libsip_core::Media::VideoProtocolPrefix::CAMERA) {
        recognized = true;
        /* Video4Linux2 */
        ready = initCamera(suffix);
        if (!ready) {
            currentResource_ = previousResource;
            decOpts_ = previousDecOpts;
            emulateRate_ = previousEmulateRate;
            switchPending_ = false;
            foundDecOpts(DeviceParams {});
            futureDecOpts_ = foundDecOpts_.get_future().share();
            notifySetupFailed(false);
            return futureDecOpts_;
        }
    } else if (prefix == libsip_core::Media::VideoProtocolPrefix::DISPLAY) {
        recognized = true;
        /* X11 display name */
#ifdef __APPLE__
        ready = initAVFoundation(suffix);
#elif defined(_WIN32) && defined(USE_DSHOW_SCREEN_CAPTURE)
        ready = initScreenCaptureRecorder(suffix);
#elif defined(_WIN32)
        ready = initWindowsCapture(suffix);
#else
        ready = initX11(suffix);
#endif
    } else if (prefix == libsip_core::Media::VideoProtocolPrefix::FILE) {
        recognized = true;
        /* Pathname */
        ready = initFile(suffix);
    }

    if (!recognized) {
        currentResource_ = previousResource;
        decOpts_ = previousDecOpts;
        emulateRate_ = previousEmulateRate;
        switchPending_ = false;
        return {};
    }

    const auto nextDecOpts = decOpts_;
    const auto nextEmulateRate = emulateRate_;

    // Keep the requested resource visible during shutdown. Restart paths can
    // observe currentResource_ while stopInput() tears down the previous source.
    currentResource_ = normalizedResource;
    decOpts_ = previousDecOpts;
    emulateRate_ = previousEmulateRate;

    if (!isStopped_) {
        stopInput();
        sink_ = Manager::instance().createSinkClient(normalizedResource);
    }

    currentResource_ = normalizedResource;
    decOpts_ = nextDecOpts;
    emulateRate_ = nextEmulateRate;

    if (ready) {
        foundDecOpts(decOpts_);
    }

    futureDecOpts_ = foundDecOpts_.get_future().share();

    startInput();

    return futureDecOpts_;
}

MediaStream
VideoInput::getInfo() const
{
    if (!videoManagedByClient()) {
        if (decoder_)
            return decoder_->getStream("v:local");
    }
    auto opts = futureDecOpts_.get();
    rational<int> fr(opts.framerate.numerator(), opts.framerate.denominator());
    return MediaStream("v:local",
                       av_get_pix_fmt(opts.pixel_format.c_str()),
                       1 / fr,
                       opts.width,
                       opts.height,
                       0,
                       fr);
}

void
VideoInput::foundDecOpts(const DeviceParams& params)
{
    if (not decOptsFound_) {
        decOptsFound_ = true;
        foundDecOpts_.set_value(params);
    }
}

void
VideoInput::setSink(const std::string& sinkId)
{
    sink_ = Manager::instance().createSinkClient(sinkId);
}

void
VideoInput::setFrameSize(const int width, const int height)
{
    /* Signal the client about readable sink */
    sink_->setFrameSize(width, height);
}

void
VideoInput::setupSink()
{
    setup();
}

void
VideoInput::stopSink()
{
    detach(sink_.get());
    sink_->stop();
}

void
VideoInput::updateStartTime(int64_t startTime)
{
    if (decoder_) {
        decoder_->updateStartTime(startTime);
    }
}

} // namespace video
} // namespace sip_core
