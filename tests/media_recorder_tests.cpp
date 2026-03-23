/*
 *  Tests for MediaRecorder — specifically the "lazy-attach" pattern where
 *  streams are added AFTER startRecording() has been called.  This is the
 *  exact scenario that occurs when isAlwaysRecording is enabled.
 */

#include "libav_deps.h" // MUST BE INCLUDED FIRST
#include "media/media_recorder.h"
#include "media/media_stream.h"
#include "media/media_buffer.h"
#include "manager.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <thread>
#include <chrono>
#include <filesystem>

using namespace sip_core;

namespace {

void
fail(const std::string& message)
{
    std::cerr << "TEST FAILURE: " << message << "\n";
    std::exit(1);
}

void
expect_true(bool condition, const std::string& message)
{
    if (!condition)
        fail(message);
}

/// Create a silent S16 stereo 48 kHz audio frame with the given number of samples.
std::shared_ptr<MediaFrame>
makeSilentAudioFrame(int nbSamples, int64_t pts)
{
    auto frame = std::make_shared<MediaFrame>();
    auto* f = frame->pointer();
    f->format = AV_SAMPLE_FMT_S16;
    f->sample_rate = 48000;
    f->nb_samples = nbSamples;
    av_channel_layout_default(&f->ch_layout, 2);
    if (av_frame_get_buffer(f, 0) < 0)
        fail("av_frame_get_buffer failed");
    // fill with silence
    std::memset(f->data[0], 0, f->linesize[0]);
    f->pts = pts;
    return frame;
}

// ---------------------------------------------------------------------------
// Test: streams added *after* startRecording() still receive frames
// ---------------------------------------------------------------------------
void
test_lazy_attach_records_frames()
{
    auto recorder = std::make_shared<MediaRecorder>();

    // Use a temp path for the output file
    auto tmpDir = std::filesystem::temp_directory_path() / "sip_core_test";
    std::filesystem::create_directories(tmpDir);
    auto basePath = (tmpDir / "lazy_attach_test").string();

    recorder->setPath(basePath);
    recorder->audioOnly(true);

    // 1. Start recording FIRST — streams_ is empty at this point
    int ret = recorder->startRecording();
    expect_true(ret == 0, "startRecording() should succeed");
    expect_true(recorder->isRecording(), "recorder should be active after startRecording()");

    // 2. Add a stream AFTER recording has started (lazy-attach)
    MediaStream ms;
    ms.name = "a:remote";
    ms.format = AV_SAMPLE_FMT_S16;
    ms.isVideo = false;
    ms.timeBase = {1, 48000};
    ms.sampleRate = 48000;
    ms.nbChannels = 2;
    ms.frameSize = 960; // 20 ms at 48 kHz

    auto* observer = recorder->addStream(ms);
    expect_true(observer != nullptr, "addStream() after startRecording() should return an observer");

    // 3. Feed a few frames through the observer
    constexpr int framesToSend = 25; // ~500 ms of audio
    for (int i = 0; i < framesToSend; ++i) {
        auto frame = makeSilentAudioFrame(960, static_cast<int64_t>(i) * 960);
        observer->update(nullptr, frame);
    }

    // Give the recording thread a moment to encode
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 4. Stop and verify
    recorder->stopRecording();

    auto outPath = basePath + ".ogg";
    expect_true(std::filesystem::exists(outPath),
                "Output file should exist: " + outPath);
    auto fileSize = std::filesystem::file_size(outPath);
    expect_true(fileSize > 200,
                "Output file should contain encoded data (size=" + std::to_string(fileSize) + ")");

    // Cleanup
    std::filesystem::remove_all(tmpDir);
}

// ---------------------------------------------------------------------------
// Test: addStream before startRecording still works (baseline sanity check)
// ---------------------------------------------------------------------------
void
test_normal_attach_records_frames()
{
    auto recorder = std::make_shared<MediaRecorder>();

    auto tmpDir = std::filesystem::temp_directory_path() / "sip_core_test";
    std::filesystem::create_directories(tmpDir);
    auto basePath = (tmpDir / "normal_attach_test").string();

    recorder->setPath(basePath);
    recorder->audioOnly(true);

    // 1. Add stream BEFORE recording
    MediaStream ms;
    ms.name = "a:remote";
    ms.format = AV_SAMPLE_FMT_S16;
    ms.isVideo = false;
    ms.timeBase = {1, 48000};
    ms.sampleRate = 48000;
    ms.nbChannels = 2;
    ms.frameSize = 960;

    auto* observer = recorder->addStream(ms);
    expect_true(observer != nullptr, "addStream() should succeed");

    // 2. Start recording
    int ret = recorder->startRecording();
    expect_true(ret == 0, "startRecording() should succeed");

    // 3. Feed frames
    constexpr int framesToSend = 25;
    for (int i = 0; i < framesToSend; ++i) {
        auto frame = makeSilentAudioFrame(960, static_cast<int64_t>(i) * 960);
        observer->update(nullptr, frame);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    recorder->stopRecording();

    auto outPath = basePath + ".ogg";
    expect_true(std::filesystem::exists(outPath),
                "Output file should exist: " + outPath);
    auto fileSize = std::filesystem::file_size(outPath);
    expect_true(fileSize > 200,
                "Output file should contain encoded data (size=" + std::to_string(fileSize) + ")");

    std::filesystem::remove_all(tmpDir);
}

} // anonymous namespace

int
main()
{
    // Minimal Manager init required by MediaEncoder/system codec container
    Manager::instance().init("", std::nullopt);

    test_lazy_attach_records_frames();
    test_normal_attach_records_frames();

    std::cout << "All media recorder tests passed.\n";

    Manager::instance().finish();
    return 0;
}
