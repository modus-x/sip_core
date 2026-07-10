<!-- Parent: ../AGENTS.md -->

# src/media/audio/opensl/ — Android backend (OpenSL ES)

Android `AudioLayer` built on **OpenSL ES** (the only audio API guaranteed across all Android NDK versions; AAudio is preferred on Android 8+ but not used here).

## Files

| File                          | Role                                                                                            |
|-------------------------------|-------------------------------------------------------------------------------------------------|
| `opensllayer.h/cpp`           | `OpenSLLayer` — derives from `AudioLayer`. Creates the OpenSL engine, output mix, and player/recorder objects. |
| `audio_player.h/cpp`          | Playback path. Uses an OpenSL `SLPlayItf` + buffer queue.                                       |
| `audio_recorder.h/cpp`        | Capture path. Uses `SLRecordItf` + buffer queue.                                                |
| `audio_common.h`              | Shared OpenSL types/macros (`SLresult` check helpers).                                          |
| `buf_manager.h`               | Tiny buffer pool — enqueue/dequeue PCM blocks.                                                  |

## Behavior

- **Hardware format**: Android reports preferred sample rate + buffer size via `AudioManager`. The daemon calls `ConfigurationSignal::GetHardwareAudioFormat` on the host (Android only) to get those values — the host implements the JNI bridge.
- **Voice mode**: stream type is `SL_ANDROID_STREAM_VOICE` so the OS routes audio through the earpiece / handset speakers and uses the comms volume profile.
- **AEC**: the daemon-side WebRTC AEC is used because OpenSL's built-in AEC is unreliable across vendors. You can switch it off in `audio:` config if your device's hardware AEC is good enough.
- **No hot-plug** — Android handles routing transparently. The daemon does react to `DeviceEvent` when the host signals a route change.

## Build

Linked when `ANDROID` is true. Requires the NDK; pulls in `-lOpenSLES`. No contrib build.

## Gotchas

- OpenSL has no concept of latency reporting; you have to estimate. The daemon assumes the default 10–40ms.
- Buffer queues are fixed-size; underruns cause silence (no error). Watch for them in the receive thread logs.
- iOS extension flag (`LIBSIP_CORE_FLAG_IOS_EXTENSION`) is **not** relevant here despite the parallel naming — Android has no equivalent.

## Dependencies

- **Internal**: `media/audio/audiolayer.h`.
- **External**: Android NDK (`-lOpenSLES`). No contrib build.

<!-- MANUAL: -->
