#include "media/video/video_input.h"
#include "media/video/video_source_utils.h"
#include "media/media_filter.h"
#include "sip/sdp.h"

#include <pjlib.h>
#include <pjmedia/sdp.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

using namespace sip_core;
using namespace sip_core::video;

namespace {

constexpr auto kWaitPoll = std::chrono::milliseconds(25);
constexpr auto kOpenTimeout = std::chrono::seconds(3);
constexpr auto kStopTimeout = std::chrono::seconds(2);

void
fail(const std::string& message)
{
    std::cerr << "TEST FAILURE: " << message << "\n";
    std::exit(1);
}

void
expect_true(bool condition, const std::string& message)
{
    if (!condition) {
        fail(message);
    }
}

bool
wait_for_condition(const std::function<bool()>& predicate, const std::chrono::milliseconds timeout)
{
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < timeout) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(kWaitPoll);
    }
    return predicate();
}

std::string
asset_path(const std::string& name)
{
    auto assetsDir = std::filesystem::path(__FILE__).parent_path() / "assets";
    auto path = assetsDir / name;
    if (!std::filesystem::exists(path)) {
        fail("Missing test asset: " + path.string());
    }
    return path.string();
}

MediaStream
make_audio_stream(const std::string& name)
{
    return MediaStream(name, AV_SAMPLE_FMT_S16, {1, 48000}, 48000, 2, 960);
}

void
test_audio_filter_compatibility()
{
    {
        auto input = make_audio_stream("input");
        MediaFilter single;
        auto ret = single.initialize(
            "[input]aformat=sample_fmts=s16:sample_rates=48000:channel_layouts=stereo",
            {input});
        expect_true(ret == 0, "Single-input audio filter graph should initialize");
        auto output = single.getOutputParams();
        expect_true(output.isValid() && !output.isVideo,
                    "Single-input audio filter graph should expose valid audio output");
    }

    {
        auto local = make_audio_stream("local");
        auto peer = make_audio_stream("peer");
        MediaFilter mixed;
        auto ret = mixed.initialize(
            "[local][peer]amix=inputs=2,aformat=sample_fmts=s16:sample_rates=48000:channel_layouts=stereo",
            {local, peer});
        expect_true(ret == 0, "Mixed-input audio filter graph should initialize");
        auto output = mixed.getOutputParams();
        expect_true(output.isValid() && !output.isVideo,
                    "Mixed-input audio filter graph should expose valid audio output");
    }
}

void
test_video_input_mute_restart()
{
    auto filePath = asset_path("test_16x12.mp4");
    VideoInput input(VideoInputMode::ManagedByDaemon, "");

    auto futureParams = input.switchInput("file://" + filePath);
    expect_true(futureParams.valid(), "switchInput should return a valid future");

    bool opened
        = wait_for_condition([&]() { return input.getWidth() == 16 && input.getHeight() == 12; },
                             kOpenTimeout);
    expect_true(opened, "VideoInput should open the test file");

    input.stopInput();

    bool stopped
        = wait_for_condition([&]() { return input.getWidth() == 0 && input.getHeight() == 0; },
                             kStopTimeout);
    expect_true(stopped, "VideoInput should stop when muted");

    input.startInput();

    bool restarted
        = wait_for_condition([&]() { return input.getWidth() == 16 && input.getHeight() == 12; },
                             kOpenTimeout);
    expect_true(restarted, "VideoInput should restart after unmute");

    input.stopInput();
}

void
test_video_input_switching()
{
    auto filePathA = asset_path("test_16x12.mp4");
    auto filePathB = asset_path("test_32x24.mp4");

    VideoInput input(VideoInputMode::ManagedByDaemon, "");

    auto futureA = input.switchInput("file://" + filePathA);
    expect_true(futureA.valid(), "switchInput for first device should return valid future");

    bool openedA
        = wait_for_condition([&]() { return input.getWidth() == 16 && input.getHeight() == 12; },
                             kOpenTimeout);
    expect_true(openedA, "VideoInput should open first device");
    expect_true(input.getConfig().input == filePathA,
                "VideoInput should report the first device path");

    auto futureB = input.switchInput("file://" + filePathB);
    expect_true(futureB.valid(), "switchInput for second device should return valid future");

    bool openedB
        = wait_for_condition([&]() { return input.getWidth() == 32 && input.getHeight() == 24; },
                             kOpenTimeout);
    expect_true(openedB, "VideoInput should switch to second device");
    expect_true(input.getConfig().input == filePathB,
                "VideoInput should report the second device path");

    input.stopInput();
}

void
test_video_source_validation_helpers()
{
    const std::vector<std::string> devices {"video=cam0", "video=cam01"};

    expect_true(containsExactDeviceId(devices, "video=cam0"),
                "containsExactDeviceId should match exact ids");
    expect_true(!containsExactDeviceId(devices, "video=cam"),
                "containsExactDeviceId must not match partial ids");
    expect_true(chooseDefaultDeviceId("video=cam0", devices) == "video=cam0",
                "chooseDefaultDeviceId should preserve a valid default");
    expect_true(chooseDefaultDeviceId("video=missing", devices) == "video=cam0",
                "chooseDefaultDeviceId should fall back to the first physical device");
    expect_true(chooseDefaultDeviceId("video=missing", {}).empty(),
                "chooseDefaultDeviceId should return empty when no physical devices remain");

    expect_true(isValidVideoSwitchSource("", devices),
                "Empty source should be accepted for local video disable");
    expect_true(isValidVideoSwitchSource("display://Desktop", devices),
                "Display capture source should be accepted");
    expect_true(isValidVideoSwitchSource("desktop://Desktop", devices),
                "Desktop alias source should be accepted");
    expect_true(isValidVideoSwitchSource("file://clip.mp4", devices),
                "File source should be accepted");
    expect_true(isValidVideoSwitchSource("camera://video=cam0", devices),
                "Known camera source should be accepted");
    expect_true(normalizeVideoSwitchSource("desktop://Desktop") == "display://Desktop",
                "Desktop alias should normalize to display prefix");
    expect_true(!isValidVideoSwitchSource("camera://", devices),
                "Camera source without device id must be rejected");
    expect_true(!isValidVideoSwitchSource("camera://video=cam", devices),
                "Camera source must use exact id matching");
    expect_true(!isValidVideoSwitchSource("camera://video=missing", devices),
                "Unknown camera source must be rejected");
}

void
test_invalid_camera_switch_fails_fast()
{
    VideoInput input(VideoInputMode::ManagedByDaemon, "");
    auto futureParams = input.switchInput("camera://video=missing");

    expect_true(futureParams.valid(), "Invalid camera switch should still return a future");
    expect_true(wait_for_condition(
                    [&]() {
                        return futureParams.wait_for(std::chrono::milliseconds(0))
                               == std::future_status::ready;
                    },
                    std::chrono::milliseconds(250)),
                "Invalid camera switch should resolve without hanging");

    auto params = futureParams.get();
    expect_true(params.input.empty(), "Invalid camera switch should resolve to empty device params");
    input.stopInput();
}

void
test_invalid_camera_switch_preserves_current_input()
{
    auto filePath = asset_path("test_16x12.mp4");
    VideoInput input(VideoInputMode::ManagedByDaemon, "");

    auto initialFuture = input.switchInput("file://" + filePath);
    expect_true(initialFuture.valid(), "Initial file switch should return a valid future");
    expect_true(wait_for_condition(
                    [&]() { return input.getWidth() == 16 && input.getHeight() == 12; },
                    kOpenTimeout),
                "VideoInput should open the initial source before invalid switch");

    auto failedFuture = input.switchInput("camera://video=missing");
    expect_true(failedFuture.valid(), "Rejected camera switch should still return a future");
    expect_true(wait_for_condition(
                    [&]() {
                        return failedFuture.wait_for(std::chrono::milliseconds(0))
                               == std::future_status::ready;
                    },
                    std::chrono::milliseconds(250)),
                "Rejected camera switch should resolve quickly");

    auto failedParams = failedFuture.get();
    expect_true(failedParams.input.empty(), "Rejected camera switch should resolve to empty params");
    expect_true(input.getName() == "file://" + filePath,
                "Rejected camera switch must keep the previous source resource");
    expect_true(input.getConfig().input == filePath,
                "Rejected camera switch must keep the previous device configuration");
    expect_true(wait_for_condition(
                    [&]() { return input.getWidth() == 16 && input.getHeight() == 12; },
                    std::chrono::milliseconds(250)),
                "Rejected camera switch must leave the current video input running");

    input.stopInput();
}

void
test_sdp_mute_mapping()
{
    const std::string recvOnlySdp = "v=0\r\n"
                                    "o=- 0 0 IN IP4 127.0.0.1\r\n"
                                    "s=-\r\n"
                                    "c=IN IP4 127.0.0.1\r\n"
                                    "t=0 0\r\n"
                                    "m=video 5004 RTP/AVP 96\r\n"
                                    "a=rtpmap:96 VP8/90000\r\n"
                                    "a=recvonly\r\n";

    const std::string sendRecvSdp = "v=0\r\n"
                                    "o=- 0 0 IN IP4 127.0.0.1\r\n"
                                    "s=-\r\n"
                                    "c=IN IP4 127.0.0.1\r\n"
                                    "t=0 0\r\n"
                                    "m=video 5004 RTP/AVP 96\r\n"
                                    "a=rtpmap:96 VP8/90000\r\n"
                                    "a=sendrecv\r\n";

    pj_status_t status = pj_init();
    if (status != PJ_SUCCESS) {
        fail("pj_init failed");
    }

    pj_caching_pool cp;
    pj_caching_pool_init(&cp, nullptr, 0);
    pj_pool_t* pool = pj_pool_create(&cp.factory, "sdp-test", 4096, 4096, nullptr);
    if (!pool) {
        pj_caching_pool_destroy(&cp);
        pj_shutdown();
        fail("Failed to create PJ pool");
    }

    pjmedia_sdp_session* session = nullptr;
    auto recvOnlySdpBuffer = recvOnlySdp;
    status = pjmedia_sdp_parse(pool, recvOnlySdpBuffer.data(), recvOnlySdpBuffer.size(), &session);
    if (status != PJ_SUCCESS || session == nullptr) {
        pj_pool_release(pool);
        pj_caching_pool_destroy(&cp);
        pj_shutdown();
        fail("Failed to parse SDP");
    }

    auto remoteList = Sdp::getMediaAttributeListFromSdp(session, false, true);
    expect_true(remoteList.size() == 1, "Remote media list should contain one entry");
    expect_true(remoteList[0].muted_, "Remote recvonly should be treated as muted");

    auto localList = Sdp::getMediaAttributeListFromSdp(session, false, false);
    expect_true(localList.size() == 1, "Local media list should contain one entry");
    expect_true(!localList[0].muted_, "Local media should not inherit remote recvonly mute");

    pjmedia_sdp_session* sendRecvSession = nullptr;
    auto sendRecvSdpBuffer = sendRecvSdp;
    status = pjmedia_sdp_parse(pool,
                               sendRecvSdpBuffer.data(),
                               sendRecvSdpBuffer.size(),
                               &sendRecvSession);
    if (status != PJ_SUCCESS || sendRecvSession == nullptr) {
        pj_pool_release(pool);
        pj_caching_pool_destroy(&cp);
        pj_shutdown();
        fail("Failed to parse sendrecv SDP");
    }

    auto sendRecvRemoteList = Sdp::getMediaAttributeListFromSdp(sendRecvSession, false, true);
    expect_true(sendRecvRemoteList.size() == 1,
                "Remote media list for sendrecv SDP should contain one entry");
    expect_true(!sendRecvRemoteList[0].muted_, "Remote sendrecv SDP must not be treated as muted");

    pj_pool_release(pool);
    pj_caching_pool_destroy(&cp);
    pj_shutdown();
}

} // namespace

int
main()
{
    test_audio_filter_compatibility();
    test_video_input_mute_restart();
    test_video_input_switching();
    test_video_source_validation_helpers();
    test_invalid_camera_switch_fails_fast();
    test_invalid_camera_switch_preserves_current_input();
    test_sdp_mute_mapping();

    std::cout << "All video input tests passed.\n";
    return 0;
}
