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

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

using namespace sip_core;

namespace {

/// Return a temp directory that is safe for FFmpeg's avio_open on Windows.
/// std::filesystem::temp_directory_path() may contain non-ASCII characters
/// (e.g. Cyrillic usernames) which avio_open cannot handle.
std::filesystem::path
safeTmpDir()
{
#ifdef _WIN32
    // Prefer a root-level temp directory to avoid non-ASCII user paths
    std::filesystem::path candidate("C:\\temp");
    if (std::filesystem::exists(candidate) || std::filesystem::create_directories(candidate))
        return candidate / "sip_core_test";
#endif
    return std::filesystem::temp_directory_path() / "sip_core_test";
}

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
    auto tmpDir = safeTmpDir();
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

    auto tmpDir = safeTmpDir();
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

    auto tmpDir = safeTmpDir();
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

    auto tmpDir = safeTmpDir();
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

/// Probe the audio duration of a file using FFmpeg demuxer.
/// Returns duration in milliseconds, or -1 on error.
int64_t
probeAudioDurationMs(const std::string& path)
{
    AVFormatContext* fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, path.c_str(), nullptr, nullptr) < 0)
        return -1;
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        avformat_close_input(&fmtCtx);
        return -1;
    }

    // Find the audio stream
    int audioIdx = -1;
    for (unsigned i = 0; i < fmtCtx->nb_streams; ++i) {
        if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audioIdx = static_cast<int>(i);
            break;
        }
    }
    if (audioIdx < 0) {
        avformat_close_input(&fmtCtx);
        return -1;
    }

    // Read all packets to find the maximum PTS (most reliable for short files)
    int64_t maxPts = 0;
    int64_t maxDuration = 0;
    AVStream* stream = fmtCtx->streams[audioIdx];
    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data = nullptr;
    pkt.size = 0;
    while (av_read_frame(fmtCtx, &pkt) >= 0) {
        if (pkt.stream_index == audioIdx) {
            int64_t endPts = pkt.pts + pkt.duration;
            if (endPts > maxPts)
                maxPts = endPts;
        }
        av_packet_unref(&pkt);
    }

    // Convert max PTS from stream time_base to milliseconds
    int64_t durationMs = av_rescale_q(maxPts, stream->time_base, {1, 1000});
    avformat_close_input(&fmtCtx);
    return durationMs;
}

// ---------------------------------------------------------------------------
// Test: Audio frames are NOT lost when stopRecording() is called immediately
// after feeding the last batch of frames.  This is the exact bug scenario:
// the recording thread must drain all buffered frames and the flush() must
// drain the filter pipeline before the file is finalized.
//
// Frames are fed with a tiny inter-frame delay (1 ms) to allow the filter
// graph's internal threads to process them (the graph uses nb_threads > 1).
// The critical part: NO extra sleep before stopRecording().
// ---------------------------------------------------------------------------
void
test_audio_frames_not_lost_on_stop()
{
    auto recorder = std::make_shared<MediaRecorder>();

    auto tmpDir = safeTmpDir();
    std::filesystem::create_directories(tmpDir);
    auto basePath = (tmpDir / "no_loss_test").string();

    recorder->setPath(basePath);
    recorder->audioOnly(true);

    int ret = recorder->startRecording();
    expect_true(ret == 0, "startRecording() should succeed");

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

    // Feed 50 frames = 1000 ms of audio at 48 kHz, 960 samples/frame (20 ms each).
    // A tiny delay between frames lets the multi-threaded filter graph process them
    // (mirrors real-world audio delivery at ~50 fps).
    constexpr int framesToSend = 50;
    constexpr int expectedDurationMs = framesToSend * 20; // 1000 ms
    for (int i = 0; i < framesToSend; ++i) {
        auto frame = makeSilentAudioFrame(960, static_cast<int64_t>(i) * 960);
        observer->update(nullptr, frame);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Stop IMMEDIATELY after the last frame — no extra sleep.
    // Before the fix the recording thread would discard buffered frames and
    // flush() would not drain the filter pipeline, losing the last seconds.
    recorder->stopRecording();

    auto outPath = basePath + ".ogg";
    expect_true(std::filesystem::exists(outPath),
                "Output file should exist: " + outPath);

    // Probe actual recorded duration
    auto actualDurationMs = probeAudioDurationMs(outPath);
    expect_true(actualDurationMs > 0,
                "Should be able to probe audio duration from output file");

    // The recorded audio must cover at least 70% of the expected duration.
    // Opus encoder delay (~26ms) and multi-threaded filter graph scheduling
    // account for the remaining gap.  Before the fix this would be ~0 ms
    // (or at most one Opus frame = 20 ms).
    int64_t minAcceptableMs = expectedDurationMs * 70 / 100; // 700 ms
    expect_true(actualDurationMs >= minAcceptableMs,
                "Recorded audio duration (" + std::to_string(actualDurationMs)
                    + " ms) should be >= 70% of expected (" + std::to_string(expectedDurationMs)
                    + " ms). Minimum acceptable: " + std::to_string(minAcceptableMs) + " ms");

    std::filesystem::remove_all(tmpDir);
}

// ---------------------------------------------------------------------------
// Test: The last batch of audio frames ("tail") is properly encoded when
// stopRecording() is called right after feeding them.  Compares output
// file sizes between a short and a long recording to ensure proportionality.
// ---------------------------------------------------------------------------
void
test_audio_tail_frames_encoded_after_stop()
{
    auto tmpDir = safeTmpDir();
    std::filesystem::create_directories(tmpDir);

    auto recordAudio = [&](const std::string& name, int framesToSend) -> uintmax_t {
        auto recorder = std::make_shared<MediaRecorder>();
        auto basePath = (tmpDir / name).string();

        recorder->setPath(basePath);
        recorder->audioOnly(true);

        recorder->startRecording();

        MediaStream ms;
        ms.name = "a:remote";
        ms.format = AV_SAMPLE_FMT_S16;
        ms.isVideo = false;
        ms.timeBase = {1, 48000};
        ms.sampleRate = 48000;
        ms.nbChannels = 2;
        ms.frameSize = 960;

        auto* observer = recorder->addStream(ms);
        for (int i = 0; i < framesToSend; ++i) {
            auto frame = makeSilentAudioFrame(960, static_cast<int64_t>(i) * 960);
            observer->update(nullptr, frame);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        // Stop immediately after last frame — no extra sleep
        recorder->stopRecording();

        auto outPath = basePath + ".ogg";
        if (!std::filesystem::exists(outPath))
            return 0;
        return std::filesystem::file_size(outPath);
    };

    // Record 25 frames (500 ms) and 100 frames (2000 ms)
    auto sizeShort = recordAudio("tail_short", 25);
    auto sizeLong = recordAudio("tail_long", 100);

    expect_true(sizeShort > 200,
                "Short recording should have encoded data (size=" + std::to_string(sizeShort) + ")");
    expect_true(sizeLong > 200,
                "Long recording should have encoded data (size=" + std::to_string(sizeLong) + ")");

    // The long recording (4x more audio) should produce a noticeably larger file.
    // Before the fix, both would be tiny (only header) or identical because the
    // tail frames were discarded.  We require the long file to be at least 1.5x
    // the short file.
    expect_true(sizeLong > sizeShort * 3 / 2,
                "Long recording (" + std::to_string(sizeLong)
                    + " bytes) should be at least 1.5x short recording ("
                    + std::to_string(sizeShort) + " bytes)");

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
    test_audio_frames_not_lost_on_stop();
    test_audio_tail_frames_encoded_after_stop();

    std::cout << "All media recorder tests passed.\n";

    Manager::instance().finish();
    return 0;
}
