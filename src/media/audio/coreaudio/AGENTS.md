<!-- Parent: ../AGENTS.md -->

# src/media/audio/coreaudio/ — macOS / iOS backend (Core Audio)

Apple platform audio implementation of `AudioLayer`. Built on the AudioUnit framework (macOS uses the AUHAL unit + the HAL; iOS uses the RemoteIO unit + AVAudioSession).

## Subdirectories

| Dir     | Platform              | Notes                                                                                                |
|---------|-----------------------|------------------------------------------------------------------------------------------------------|
| `osx/`  | macOS                 | Uses `kAudioUnitSubType_HALOutput` + the HAL for device discovery/selection. Listens for hot-plug.   |
| `ios/`  | iOS / iPadOS / visionOS | Uses `kAudioUnitSubType_RemoteIO` + `AVAudioSession`. Categories/options for VoIP routing.           |

## Files

| File                             | Role                                                                                              |
|----------------------------------|---------------------------------------------------------------------------------------------------|
| `osx/audiodevice.h/cpp`          | macOS device discovery wrapper around the HAL (`AudioObjectGetPropertyData` plumbing).            |
| `osx/corelayer.h`, `osx/corelayer.mm` | macOS `AudioLayer` implementation. Objective-C++ for AVFoundation interop.                   |
| `ios/corelayer.h`, `ios/corelayer.mm` | iOS `AudioLayer` implementation. Uses `AVAudioSession` (mode `VoiceChat`, category `PlayAndRecord`). |

`.mm` is Objective-C++ — needed for direct Apple framework calls.

## Working in this directory

- **macOS device hot-plug**: handled via `AudioObjectAddPropertyListener` on the system devices property. When fired, `Manager::recoverAudioDevices()` is called to rebuild the layer and notify clients via `AudioSignal::DeviceEvent`.
- **iOS audio session**: route changes are observed via `AVAudioSession`'s `routeChangeNotification`. **Interruptions** (incoming PSTN call) are mandatory to handle — see `interruptionNotification` handling.
- **Sample rate**: Core Audio prefers 44.1 kHz on macOS and 48 kHz on iOS (or whatever the session negotiates). `Manager::hardwareAudioFormatChanged(format)` propagates the negotiated format to the mixer.
- **IO buffer size**: tuned for ~10ms on macOS, configurable via `kAudioUnitProperty_MaximumFramesPerSlice`. Smaller buffer = lower latency but more CPU.
- **iOS extension support**: `LIBSIP_CORE_FLAG_IOS_EXTENSION` is set by `Manager::isIOSExtension` and gates audio session activation (extensions have different lifecycle rules).

## Build

Linked automatically on Apple platforms by the top `CMakeLists.txt` when `APPLE` is set. Frameworks linked: `CoreAudio`, `AudioToolbox`, `AVFoundation` (macOS adds `CoreServices`).

## Dependencies

- **Internal**: `media/audio/audiolayer.h`, `media/audio/ringbuffer.h`.
- **External**: Apple frameworks only — no contrib deps.

<!-- MANUAL: -->
