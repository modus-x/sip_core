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

/// Create a silent audio frame with configurable parameters.
std::shared_ptr<MediaFrame>
makeSilentAudioFrame(int nbSamples, int64_t pts, int sampleRate = 48000, int channels = 2)
{
    auto frame = std::make_shared<MediaFrame>();
    auto* f = frame->pointer();
    f->format = AV_SAMPLE_FMT_S16;
    f->sample_rate = sampleRate;
    f->nb_samples = nbSamples;
    av_channel_layout_default(&f->ch_layout, channels);
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

// ---------------------------------------------------------------------------
// Test: PCMA/PCMU-like stream (8 kHz, mono, 160 samples) records correctly
// ---------------------------------------------------------------------------
void
test_pcmu_codec_records_frames()
{
    auto recorder = std::make_shared<MediaRecorder>();

    auto tmpDir = std::filesystem::temp_directory_path() / "sip_core_test";
    std::filesystem::create_directories(tmpDir);
    auto basePath = (tmpDir / "pcmu_test").string();

    recorder->setPath(basePath);
    recorder->audioOnly(true);

    int ret = recorder->startRecording();
    expect_true(ret == 0, "startRecording() should succeed");

    // Simulate a PCMA/PCMU decoded stream: 8 kHz, mono, 160 samples (20 ms)
    MediaStream ms;
    ms.name = "a:remote";
    ms.format = AV_SAMPLE_FMT_S16;
    ms.isVideo = false;
    ms.timeBase = {1, 8000};
    ms.sampleRate = 8000;
    ms.nbChannels = 1;
    ms.frameSize = 160;

    auto* observer = recorder->addStream(ms);
    expect_true(observer != nullptr, "addStream() should succeed for 8 kHz mono stream");

    // Feed ~1 second of 20 ms frames (50 frames)
    constexpr int framesToSend = 50;
    for (int i = 0; i < framesToSend; ++i) {
        auto frame = makeSilentAudioFrame(160, static_cast<int64_t>(i) * 160, 8000, 1);
        observer->update(nullptr, frame);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    recorder->stopRecording();

    auto outPath = basePath + ".ogg";
    expect_true(std::filesystem::exists(outPath),
                "Output file should exist: " + outPath);
    auto fileSize = std::filesystem::file_size(outPath);
    // With PCMU frames the old code produced ~369 bytes (header only).
    // With the fix we expect real encoded audio data.
    expect_true(fileSize > 500,
                "PCMU-like stream: output file should contain encoded data (size="
                    + std::to_string(fileSize) + ")");

    std::filesystem::remove_all(tmpDir);
}

// ---------------------------------------------------------------------------
// Test: G.729-like stream (8 kHz, mono, 80 samples / 10 ms) records correctly.
// G.729 frames are half the size of PCMA/PCMU, so the buffersink must
// accumulate two resampled frames (2×480 = 960) before Opus can encode.
// ---------------------------------------------------------------------------
void
test_g729_codec_records_frames()
{
    auto recorder = std::make_shared<MediaRecorder>();

    auto tmpDir = std::filesystem::temp_directory_path() / "sip_core_test";
    std::filesystem::create_directories(tmpDir);
    auto basePath = (tmpDir / "g729_test").string();

    recorder->setPath(basePath);
    recorder->audioOnly(true);

    int ret = recorder->startRecording();
    expect_true(ret == 0, "startRecording() should succeed");

    // Simulate a G.729 decoded stream: 8 kHz, mono, 80 samples (10 ms)
    MediaStream ms;
    ms.name = "a:remote";
    ms.format = AV_SAMPLE_FMT_S16;
    ms.isVideo = false;
    ms.timeBase = {1, 8000};
    ms.sampleRate = 8000;
    ms.nbChannels = 1;
    ms.frameSize = 80; // G.729 native: 10 ms at 8 kHz

    auto* observer = recorder->addStream(ms);
    expect_true(observer != nullptr, "addStream() should succeed for G.729-like stream");

    // Feed ~1 second of 10 ms frames (100 frames)
    constexpr int framesToSend = 100;
    for (int i = 0; i < framesToSend; ++i) {
        auto frame = makeSilentAudioFrame(80, static_cast<int64_t>(i) * 80, 8000, 1);
        observer->update(nullptr, frame);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    recorder->stopRecording();

    auto outPath = basePath + ".ogg";
    expect_true(std::filesystem::exists(outPath),
                "Output file should exist: " + outPath);
    auto fileSize = std::filesystem::file_size(outPath);
    expect_true(fileSize > 500,
                "G.729-like stream: output file should contain encoded data (size="
                    + std::to_string(fileSize) + ")");

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
    test_pcmu_codec_records_frames();
    test_g729_codec_records_frames();

    std::cout << "All media recorder tests passed.\n";

    Manager::instance().finish();
    return 0;
}
