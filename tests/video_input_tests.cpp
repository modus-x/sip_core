#include "media/video/video_input.h"
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
test_sdp_mute_mapping()
{
    const std::string sdp = "v=0\r\n"
                            "o=- 0 0 IN IP4 127.0.0.1\r\n"
                            "s=-\r\n"
                            "c=IN IP4 127.0.0.1\r\n"
                            "t=0 0\r\n"
                            "m=video 5004 RTP/AVP 96\r\n"
                            "a=rtpmap:96 VP8/90000\r\n"
                            "a=recvonly\r\n";

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
    auto sdpBuffer = sdp;
    status = pjmedia_sdp_parse(pool, sdpBuffer.data(), sdpBuffer.size(), &session);
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

    pj_pool_release(pool);
    pj_caching_pool_destroy(&cp);
    pj_shutdown();
}

} // namespace

int
main()
{
    test_video_input_mute_restart();
    test_video_input_switching();
    test_sdp_mute_mapping();

    std::cout << "All video input tests passed.\n";
    return 0;
}
