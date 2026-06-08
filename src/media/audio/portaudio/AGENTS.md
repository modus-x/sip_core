<!-- Parent: ../AGENTS.md -->

# src/media/audio/portaudio/ — Windows backend (PortAudio)

Default Windows `AudioLayer`. PortAudio brokers WASAPI (preferred), WDM-KS, or DirectSound depending on what the system exposes.

## Files

| File                                | Role                                                                                          |
|-------------------------------------|-----------------------------------------------------------------------------------------------|
| `portaudiolayer.h/cpp`              | `PortAudioLayer` — derives from `AudioLayer`. Opens capture + playback streams, polls the device list, handles default-device changes via `Pa_GetDefaultInput/OutputDevice`. |
| `CMakeLists.txt`                    | Builds the source set when `MSVC` is true. PortAudio itself is built by `contrib/src/portaudio`. |

## Behavior

- **Device list** comes from `Pa_GetDeviceCount` + `Pa_GetDeviceInfo`. Names are UTF-8 (PortAudio handles the conversion from WCHAR via the host API layer).
- **Callback mode** is used — PortAudio drives the encode/decode loop. The callback must not block; it pushes frames into the ring buffer.
- **Sample rate / channels** are queried per device. The mixer adapts via `Manager::hardwareAudioFormatChanged`.

## Build

PortAudio is built from `contrib/src/portaudio/` via `package.json` (Windows/MSVC build). The library lands in `contrib/<triplet>/lib/`.

`CMakeLists.txt` here is included from the parent only on MSVC builds (`if(MSVC)` in `src/media/audio/CMakeLists.txt`).

## Gotchas

- WASAPI exclusive mode is **not** used (so other apps can play audio simultaneously) — be aware this caps the minimum buffer size.
- Device IDs are PA-internal indices; they shift across system restarts. Match by name when persisting selections, not by ID.
- Windows hot-plug requires watching `WM_DEVICECHANGE` — not currently wired (re-enumerate on a timer instead).

## Dependencies

- **Internal**: `media/audio/audiolayer.h`.
- **External**: `portaudio` (built in contrib).

<!-- MANUAL: -->
