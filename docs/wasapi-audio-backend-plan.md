# Replace PortAudio with a native WASAPI audio backend (Windows 8+)

Research report + implementation plan. Goal: delete the PortAudio Windows backend and
replace it with a native WASAPI `AudioLayer` so **Windows consumers of `sip_core` notice
no difference** — same device names, same audio format, no glitches over RDP.

Sources: read-only trace of `audiolayer.{h,cpp}`, `portaudio/portaudiolayer.cpp`,
`manager.cpp`, `preferences.{h,cpp}`, `configurationmanager.cpp`, `resampler.*`,
`ringbuffer*.*`, `coreaudio/osx/corelayer.mm`, `pulseaudio/*`, CMake + `compat/msvc/config.h`.

---

## 1. The interface the new backend must implement

New class `WasapiLayer : public AudioLayer` in `src/media/audio/wasapi/`. Pure virtuals
(from `audiolayer.h`):

| Method | Notes |
|---|---|
| `getCaptureDeviceList() / getPlaybackDeviceList()` | index 0 = `"{{Default}} - <name>"`, then raw friendly names |
| `getAudioDeviceIndex(name,type)` | exact-string linear match, `-1` if absent |
| `getAudioDeviceName(index,type)` | may stay a `{}` stub (matches PortAudio) |
| `getIndexCapture/Playback/Ringtone()` | resolve stored pref → index, default 0 |
| `startStream(type) / stopStream(type)` | open/close WASAPI clients, flush ring buffers |
| `updatePreference(pref,index,type)` | index→name, store into AudioPreference |
| `isPreferredDeviceResolved(type)` | empty pref → true; else scan live endpoints |

Base services the backend calls (do not reimplement): `hardwareFormatAvailable`,
`hardwareInputFormatAvailable`, `putRecorded`, `getPlayback`, `playbackChanged`,
`recordChanged`, `setHasNativeAEC/NS`, `notifyDevicesChanged`, `flushMain/flushUrgent`,
and members `audioFormat_`, `audioInputFormat_`, `mutex_`, `status_`.

## 1.1 Device names (consumer-visible, must match exactly)

`getDevicesByType(type)` contract to reproduce:
1. Enumerate active endpoints of the direction: `IMMDeviceEnumerator::EnumAudioEndpoints(
   eRender|eCapture, DEVICE_STATE_ACTIVE)`. This gives A9 (direction-usability) for free.
2. Name each via `IMMDevice::OpenPropertyStore(STGM_READ)` → `PKEY_Device_FriendlyName`
   (`PROPVARIANT.pwszVal`, UTF-16 → UTF-8 with `WideCharToMultiByte(CP_UTF8,…)`).
3. If the list is non-empty, insert at index 0: `"{{Default}} - " + <default name>`, where
   `<default name>` = `GetDefaultAudioEndpoint(dir, eCommunications)` friendly name, falling
   back to `eConsole`, then to the first active device. Mirrors PortAudio comm-default logic.
4. Persist selections by **name** (empty string = "use default"), never by index.
   `isPreferredDeviceResolved` scans live friendly names for an exact match.

**Compatibility risk:** PortAudio's WASAPI host API also uses `PKEY_Device_FriendlyName`,
so hand-rolled names should match byte-for-byte. Verify on the Windows box that a device
name enumerated by the new layer equals what a saved config (written by PortAudio) contains;
if PortAudio applied any suffixing, reproduce it. This is the top verification item for 1.1.

## 1.2 Sample rate / format (ring-buffer contract)

- Ring buffer stores **`AV_SAMPLE_FMT_S16`, interleaved, mono/stereo**, at a negotiated
  internal rate (`resampler.cpp` hardcodes S16; `manager.cpp::audioFormatUsed` caps channels
  at 2 and grows rate monotonically).
- **Windows-critical:** `AudioLayer::adjustVolume` is compiled under `_WIN32` and does
  `reinterpret_cast<int16_t*>(data)` assuming S16. The backend therefore MUST hand S16 to
  `putRecorded` and request S16 from `getPlayback` — handing FLTP (as CoreAudio does) would
  corrupt audio. PortAudio is the template: it forces `audioInputFormat_.sampleFormat = S16`
  and converts device float ↔ S16 in the callbacks.
- **Capture:** WASAPI shared mode delivers float32 @ mix rate/channels. Convert float32 →
  interleaved S16 (keep mix rate/channels), build `AudioFrame(audioInputFormat_ = {mixRate,
  mixCh, S16}, n)`, `putRecorded(std::move(frame))`. `RingBuffer::put` resamples rate/channels
  to internal — backend does NOT resample rate itself.
- **Playback:** request `getPlayback({mixRate, mixCh, S16}, n)` (explicit format arg →
  `getToPlay` resamples internal → this format, sidestepping the monotonic-max-rate pitfall),
  convert the returned S16 → the WASAPI render format (float32); write silence / `SILENT`
  flag when it returns null.
- Publish format up: set `audioFormat_`/`audioInputFormat_`, call `hardwareFormatAvailable(
  playbackFormat, bufSize)` (store its returned negotiated format into `audioFormat_`) and
  `hardwareInputFormatAvailable(captureFormat)`. Call `playbackChanged(true)`/`recordChanged(
  true)` on start (both must be true for the audio processor / AEC path to open).
- Target ~20 ms (`rate/50`) processing grain; interpose an accumulator so a variable WASAPI
  packet size is re-chunked to 20 ms.

## 1.3 RDP: zero lag / glitches

WASAPI design (Win8 floor — use `IAudioClient`/`IAudioClient2`, **never** `IAudioClient3`):
- **Shared mode only** (`AUDCLNT_SHAREMODE_SHARED`). RDP "Remote Audio" supports only shared;
  take `GetMixFormat()` verbatim (do not negotiate), convert to/from S16.
- **Event-driven**: `AUDCLNT_STREAMFLAGS_EVENTCALLBACK` + `SetEventHandle` + dedicated
  per-direction thread on `WaitForSingleObject(hEvent, watchdog)`. Watchdog timeout
  (~2× period, ≤100 ms) so a missed/late RDP event falls through to `GetCurrentPadding`
  polling instead of hanging.
- **Buffer sizing** (`hnsBufferDuration`, `hnsPeriodicity`=0 in shared mode): local ≈
  max(3×devicePeriod, 30 ms); **RDP ≈ 200 ms** to absorb network jitter. Detect RDP via
  `GetSystemMetrics(SM_REMOTESESSION)` and/or "Remote Audio" default-endpoint name.
- **MMCSS**: on each audio thread `AvSetMmThreadCharacteristicsW(L"Pro Audio")` (fallback
  `L"Audio"`), revert on exit. Biggest single glitch-free lever, esp. on loaded RDP hosts.
- **COM**: `CoInitializeEx(nullptr, COINIT_MULTITHREADED)` per audio/monitor thread.
- **`IAudioClient2::SetClientProperties`** `eCategory = AudioCategory_Communications` (Win8+),
  guarded — fall back to plain `IAudioClient` if the QI fails.
- **Device-change resilience**: reuse the existing `WindowsAudioDeviceMonitor` +
  `DefaultDeviceListener (IMMNotificationClient)` verbatim (rename to drop "PortAudio"),
  routed into the 250 ms-debounced `Manager::recoverAudioDevices` (destroys + rebuilds layer).
  In every stream loop, treat `AUDCLNT_E_DEVICE_INVALIDATED` / `AUDCLNT_E_RESOURCES_INVALIDATED`
  as an RDP-drop signal → stop, release, trigger the same debounced recovery.
- **Starvation**: render writes `AUDCLNT_BUFFERFLAGS_SILENT` when empty and keeps running;
  capture tolerates `AUDCLNT_S_BUFFER_EMPTY`. Never block/abort the loop.

COM interfaces: `IMMDeviceEnumerator`, `IMMDevice`, `IAudioClient`/`IAudioClient2`,
`IAudioCaptureClient`, `IAudioRenderClient`, `IMMNotificationClient`.

---

## 2. Config / preference compatibility — ZERO migration

- Keep the on-disk YAML **verbatim**: submap `portaudio` with keys `devicePlayback` /
  `deviceRecord` / `deviceRingtone` (`preferences.cpp` serialize/deserialize). These string
  keys are the compatibility surface; C++ member/getter names may be renamed but the plan
  keeps them to minimize churn.
- `audioApi` identifier: introduce `WASAPI_API_STR "wasapi"` for the new backend. On Windows
  a single backend is compiled, and `createAudioLayer()` does not branch on the stored
  `audioApi` value (only sets it), so an old `audioApi: portaudio` value is inert/harmless.
- Activation: in `compat/msvc/config.h` set `HAVE_PORTAUDIO 0`, add `HAVE_WASAPI 1`. Switch
  `preferences.cpp` guards `#if HAVE_PORTAUDIO` → `#if HAVE_WASAPI`.

---

## 3. Ordered implementation checklist

**A. Add WASAPI backend**
1. `src/media/audio/wasapi/wasapilayer.h` — `WasapiLayer : public AudioLayer`, pimpl.
2. `src/media/audio/wasapi/wasapilayer.cpp` — enumeration, streams, callbacks, monitor.
3. `src/media/audio/wasapi/CMakeLists.txt` — clone portaudio's 2-line source-list + PARENT_SCOPE.
4. `src/media/audio/wasapi/AGENTS.md` — doc the backend.

**B. Wire selection / build**
5. `compat/msvc/config.h` — `HAVE_PORTAUDIO 0`, `HAVE_WASAPI 1`.
6. `src/media/audio/audiolayer.h` — add `#define WASAPI_API_STR "wasapi"` (keep PORTAUDIO_API_STR removed).
7. `src/preferences.cpp` — `#if HAVE_PORTAUDIO`→`HAVE_WASAPI` (include, factory branch line ~323,
   getSupportedAudioManagers line ~380); `new PortAudioLayer` → `new WasapiLayer`. Keep YAML keys.
8. `src/media/audio/CMakeLists.txt:52-57` — `add_subdirectory(portaudio)` → `wasapi`; rename
   `Source_Files__media__audio__portaudio` → `__wasapi`.
9. `src/CMakeLists.txt` + `src/media/CMakeLists.txt` — rename the propagated source-set var.
10. Root `CMakeLists.txt` — source_group (~338), ALL_FILES append (~387), rename var; remove
    portaudio include dir (~498) and the two `portaudio_static_x64.lib` link lines (~621, ~661);
    add system libs `Mmdevapi.lib uuid.lib ksuser.lib avrt.lib` (ole32 already linked) in the
    MSVC `target_link_libraries` block. (`HAVE_WASAPI` comes from config.h, not CMake.)
11. `compat/msvc/package.json` — remove `"portaudio"` from `deps`.

**C. Delete PortAudio**
12. Delete `src/media/audio/portaudio/` (cpp/h/AGENTS.md/Makefile.am + CMakeLists.txt).
13. Delete `contrib/src/portaudio/` (rules.mak, package.json, patch, SHA512SUMS).
14. Remove portaudio rows in `contrib/AGENTS.md`, `contrib/src/AGENTS.md`.

**D. Docs**
15. Update `AGENTS.md`, `CLAUDE.md`, `WARP.md`, `src/media/audio/AGENTS.md`,
    `src/config/AGENTS.md` audio-backend enumerations (portaudio → wasapi). `test.yaml` submap
    key left as `portaudio` (config compat) — note in doc.

**E. Tests** (see §4)

**F. Build + iterate on `ssh dev_real_win_11` : `C:\dev\sip_core`** (never build locally).

---

## 4. Test strategy

No CTest suite exists. Add self-contained tests under `tests/` exercising the pure logic
that does NOT need live audio hardware, so they run headless in CI-like fashion:
- **Float32/Int16/Int32 ↔ S16 conversion** round-trip correctness (the `convert*` helpers
  factored out as free functions) — clamping, channel interleave, silence.
- **Device-name formatting**: given a fake endpoint list, `getDevicesByType` produces
  `"{{Default}} - X"` at index 0 and correct name↔index round-trip (mock the enumerator via a
  seam, or unit-test the pure formatting helper).
- **UTF-16 → UTF-8** friendly-name conversion.
- **20 ms re-chunking accumulator** logic.
On-box manual validation with `sip_cli`: enumerate devices, place a call, verify audio both
locally and inside an RDP session (record/playback, device switch mid-call, RDP reconnect).

---

## 5. Requirement traceability

| Req | Covered by |
|---|---|
| 1 sciomc report + plan | this document |
| 1.1 same device names | §1.1 (friendly-name enumeration + `{{Default}}` alias + name persistence) |
| 1.2 correct rate/format | §1.2 (float↔S16 in callbacks, S16 ring-buffer contract, pipeline resampling) |
| 1.3 no RDP lag | §1.3 (shared mode, event+watchdog, 200ms buffer, MMCSS, invalidation recovery) |
| 2 context7 docs | used during implementation of §3.A |
| 3 tests | §4 |
| 4 build only on ssh dev_real_win_11 | §3.F |
| 5 Windows 8 min | IAudioClient/IAudioClient2 only, no IAudioClient3; all libs in Win8 SDK |
| 6 ralph critics/architects per 1.1→1.2→1.3 | post-first-build loop |
</content>
</invoke>
