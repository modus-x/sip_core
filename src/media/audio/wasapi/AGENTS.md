<!-- Parent: ../AGENTS.md -->

# src/media/audio/wasapi/ — Windows backend (native WASAPI)

Default Windows `AudioLayer`. Talks to the Windows Core Audio API (WASAPI) directly —
no PortAudio. Minimum target OS: **Windows 8** (`IAudioClient` / `IAudioClient2`; no
`IAudioClient3` dependency). Replaced the old `portaudio/` backend.

## Files

| File                 | Role                                                                                          |
|----------------------|-----------------------------------------------------------------------------------------------|
| `wasapilayer.h/cpp`  | `WasapiLayer : public AudioLayer`. Device enumeration, shared-mode event-driven capture + render streams, MMCSS-boosted audio threads, the `WindowsAudioDeviceMonitor` + `IMMNotificationClient` device-change monitor, and RDP handling. |
| `wasapi_convert.h`   | Pure, header-only, no Windows deps: float32/PCM ↔ int16 sample conversion and the `"{{Default}} - <name>"` device-list formatting. Unit-tested by `tests/test_wasapi_convert.cpp`. |
| `CMakeLists.txt`     | Adds the source set on MSVC (via `src/media/audio/CMakeLists.txt`'s `if(MSVC)` block).         |

## Behavior

- **Mode:** `AUDCLNT_SHAREMODE_SHARED` only. Exclusive mode is never used — the RDP
  "Remote Audio" endpoint only supports shared, and shared lets other apps use audio too.
- **Format:** takes `GetMixFormat()` verbatim (shared mode; almost always 32-bit float),
  converts to/from `AV_SAMPLE_FMT_S16` in the stream loops. The ring buffer / mixer do the
  rate + channel resampling — see `wasapi-audio-backend-plan.md` §1.2. On Windows the frame
  fed to `putRecorded` / pulled from `getPlayback` **must** be S16 (`AudioLayer::adjustVolume`
  reinterpret-casts to `int16_t*` under `_WIN32`).
- **Event-driven:** `AUDCLNT_STREAMFLAGS_EVENTCALLBACK` + `SetEventHandle`, one dedicated
  MMCSS ("Pro Audio") thread per direction, `WaitForSingleObject` with a watchdog timeout so
  a late/missed RDP event never hangs the loop.
- **Device names:** `EnumAudioEndpoints(DEVICE_STATE_ACTIVE)` + `PKEY_Device_FriendlyName`
  (UTF-16 → UTF-8). Index 0 is the synthetic `"{{Default}} - <name>"` alias built from the
  `eCommunications`-role default endpoint (fallback `eConsole`). Selections persist by NAME,
  never by index.
- **Device-change / RDP resilience:** `WindowsAudioDeviceMonitor` (WM_DEVICECHANGE) +
  `DefaultDeviceListener` (`IMMNotificationClient`) feed a 250 ms-debounced
  `Manager::recoverAudioDevices` (rebuilds the layer). `AUDCLNT_E_DEVICE_INVALIDATED` in a
  stream loop triggers the same recovery — this is what catches an RDP session drop mid-call.

## Config compatibility

Reuses the existing `portaudio` YAML submap (`devicePlayback` / `deviceRecord` /
`deviceRingtone`) and the `AudioPreference::getPortAudioDevice*` accessors verbatim, so
existing Windows consumer configs keep working with **zero migration**. The audio-manager
identifier string is `"wasapi"` (`WASAPI_API_STR`); an old `audioApi: portaudio` value is
inert on Windows (single compiled backend).

## Gotchas

- Buffer duration is larger over RDP (~200 ms) to absorb network jitter; local uses
  `max(3×devicePeriod, 30 ms)`. RDP is detected via `GetSystemMetrics(SM_REMOTESESSION)`.
- Links Windows SDK import libs only: `ole32`, `Mmdevapi`, `Avrt` (MMCSS), `Ksuser`, `uuid`.
  No contrib dependency — nothing to build under `contrib/`.

## Dependencies

- **Internal:** `media/audio/audiolayer.h`, `media/audio/resampler.*` (via the pipeline),
  `manager.h` (`recoverAudioDevices`, `scheduleTaskIn`).
- **External:** Windows Core Audio API (system) — no third-party libraries.

<!-- MANUAL: -->
