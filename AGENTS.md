# AGENTS.md — Repository Guide for AI Agents

`sip_core` is a C++17 SIP/VoIP library built on a patched **PJSIP** stack with FFmpeg-driven media. It exposes a C-style facade in the `libsip_core` namespace and ships as a shared library used by Windows / Linux / macOS / iOS / Android consumers (see `specs/SIP_CORE_ABI_russian.md` for the C-binding contract). The CLI in `example/sip_cli/` is the only in-tree consumer.

> **Start here.** Per-directory `AGENTS.md` files document each subsystem. Follow the *Where do I edit?* table below — do **not** grep blindly. The codebase is large and most edits cross 3–4 layers in a predictable pattern.

---

## 1. Layered architecture (read once, refer to forever)

```
+--------------------------------------------------------------+
|  Consumer (sip_core_bindings, sip_cli, host app)             |
|  - Calls libsip_core:: functions, registers signal handlers  |
+----------------------------+---------------------------------+
                             |
+----------------------------v---------------------------------+
|  FACADE  src/sip_core/                                       |  <-- public API
|  sip_core.h, callmanager_interface.h,                        |      (ABI contract)
|  configurationmanager_interface.h,                           |
|  videomanager_interface.h, presencemanager_interface.h       |
+----------------------------+---------------------------------+
                             |
+----------------------------v---------------------------------+
|  IMPLEMENTATION  src/client/                                 |
|  callmanager.cpp / configurationmanager.cpp /                |
|  presencemanager.cpp / videomanager.cpp                      |
|  -> forward into Manager singleton                           |
+----------------------------+---------------------------------+
                             |
+----------------------------v---------------------------------+
|  CORE  src/                                                  |
|  Manager (singleton) -> AccountFactory / CallFactory         |
|  Account, Call, Conference, Preferences, ScheduledExecutor   |
+--------------+------------------------------+----------------+
               |                              |
+--------------v----------------+ +-----------v---------------+
| SIP STACK  src/sip/           | | MEDIA  src/media/         |
| SIPVoIPLink (PJSIP bridge),   | | Audio/Video pipelines,    |
| SIPAccount(Base), SIPCall,    | | RTP sessions, FFmpeg      |
| Sdp, SipTransport, SIPPresence| | encode/decode, platform   |
| -> wraps patched pjproject    | | backends (per-OS subdirs) |
+-------------------------------+ +---------------------------+
```

**Manager is the central singleton.** Every public API function from `src/sip_core/*.h` is implemented in `src/client/*.cpp` and immediately delegates to `Manager::instance()` or a factory. `Manager` then dispatches into SIP/media subsystems.

Events flow the other direction: subsystems call `emitSignal<CallSignal::StateChange>(...)` (see `src/client/ring_signal.h`) — the signal is dispatched on the `eventScheduler` thread and ends up at the host-registered callback.

---

## 2. Where do I edit? — feature-to-file map

| If you want to …                            | Touch                                                                                                  |
|---------------------------------------------|--------------------------------------------------------------------------------------------------------|
| Add a new public API function (verb)        | `src/sip_core/<area>_interface.h` (decl) + `src/client/<area>manager.cpp` (impl) + Manager method      |
| Add a new client-facing signal/callback     | `src/sip_core/<area>_interface.h` (struct in Signal namespace) + `emitSignal<...>` at the source site  |
| Change SIP call lifecycle / state machine   | `src/sip/sipcall.cpp` + `src/call.cpp` base behavior + signal emission                                 |
| Change SIP registration / OPTIONS keepalive | `src/sip/sipaccount.cpp` (`doRegister`, `onRegister`, `registerKeepAliveTimer`, route fast-probe)      |
| Add/modify SDP negotiation                  | `src/sip/sdp.cpp`, `src/sip/sdes_negotiator.cpp`, `src/sip/sipcall.cpp::setupNegotiatedMedia`          |
| Change transport (UDP/TCP/TLS) behavior     | `src/sip/siptransport.cpp` + `src/sip/sipvoiplink.cpp` (transport broker)                              |
| Add/modify audio processing (NS/AEC/AGC)    | `src/media/audio/audio-processing/webrtc.cpp` (and `audio_processor.h` interface)                      |
| Add/modify a codec                          | `src/media/system_codec_container.cpp` + relevant `src/media/{audio,video}/*` encode/decode paths      |
| Change conference behavior                  | `src/conference.cpp` + `src/conference_protocol.cpp` (over-SIP metadata) + signals                     |
| Add a YAML config field                     | `src/config/*` + `src/{account_config.cpp, preferences.cpp, sip/sipaccount_config.cpp}`                |
| Per-platform audio backend                  | `src/media/audio/{coreaudio,pulseaudio,portaudio,opensl,alsa,jack}/`                                   |
| Per-platform video backend                  | `src/media/video/{osxvideo,iosvideo,v4l2,winvideo,uwpvideo,androidvideo}/`                             |
| Add a third-party dep                       | `contrib/src/<name>/{rules.mak, package.json}` + `CMakeLists.txt` find/link block                      |
| Change build / CMake options                | `CMakeLists.txt` (root) + `CMakePresets.json` + per-dir `CMakeLists.txt`                               |
| Update the C binding ABI doc                | `specs/SIP_CORE_ABI_russian.md` (canonical contract — Russian)                                         |

> **Cross-cutting verbs** (e.g. a new call-control action) almost always touch four files: facade header → manager.cpp method → factory or Account method → SIPCall/SIPVoIPLink. Grep an existing verb (`hangUp`, `transfer`, `joinParticipant`) to find the pattern; copy it.

---

## 3. Lifecycle: how a call actually happens

**Init / start / stop** (`libsip_core::init` → `start` → `fini`):

```
init(flags)
  -> Manager::instance() created
  -> registers PJSIP modules, opens audio driver, loads codecs
start(config_file, data_path)
  -> Manager::init(config_file, data_path) (sync)
  -> YAML loaded via src/config/yamlparser + per-account/preference schemas
  -> AccountFactory creates SIPAccount instances -> doRegister()
  -> ScheduledExecutor (event thread) starts
fini()
  -> unregisterAccountsImmediate() (fire-and-forget UNREGISTER)
  -> Manager::finish() — tears down audio/video, releases pjproject
```

**Outgoing call**:

```
placeCallWithMedia(acct, to, media)            [src/sip_core/callmanager_interface.h]
  -> Manager::outgoingCall                     [src/manager.cpp]
  -> SIPAccount::newOutgoingCall               [src/sip/sipaccount.cpp]
  -> CallFactory::newSipCall                   [src/call_factory.cpp]
  -> SIPCall ctor + SIPStartCall (INVITE)      [src/sip/sipcall.cpp, sipaccount.cpp]
  -> SDP offer built (src/sip/sdp.cpp)
  -> media RtpSession objects created          [src/media/{audio,video}/...]
  -> emitSignal<CallSignal::StateChange>       [via src/client/ring_signal.h]
```

**Incoming call**:

```
PJSIP module receives INVITE
  -> SIPVoIPLink callback                      [src/sip/sipvoiplink.cpp]
  -> guessAccount() -> SIPAccount::newIncomingCall
  -> Manager::incomingCall
  -> emitSignal<CallSignal::IncomingCallWithMedia>
client calls acceptWithMedia / refuse
  -> Manager::answerCall / refuseCall -> SIPCall::answer / refuse
  -> SDP answer + media negotiation (onMediaNegotiationComplete)
  -> emitSignal<CallSignal::MediaNegotiationStatus>
```

**Registration & route management** is non-trivial — `SIPAccount` supports a **main + backup service route** with OPTIONS keepalive, fast-probe escalation, and automatic failover (`switchToBackupRoute` / `switchRouteAndReregister`). Connectivity changes drive `connectivityChanged` → `recoverTransport` → `reinviteOnConnectivityChange` on active calls. Read `src/sip/sipaccount.h` carefully before touching this path.

---

## 4. Threading model

- **Main thread**: host application; `libsip_core::init/start/fini` are called here.
- **PJSIP thread** (`SIPVoIPLink::sipThread_`): owns the PJSIP event loop (`pjsip_endpt_handle_events`). All PJSIP API calls must happen here or via PJSIP-safe constructs.
- **Event thread** (`eventScheduler` in `src/client/ring_signal.h`): all `emitSignal<>` callbacks dispatch here so host code runs off the PJSIP thread.
- **Manager scheduler** (`Manager::scheduler()`): general task scheduling — use `scheduleTask(...)` / `scheduleTaskIn(...)` / `runOnMainThread(...)`.
- **Audio/video pipelines** spawn their own `ThreadLoop` instances (encode/decode/capture/playback).

If you’re unsure which thread you’re on, dump it via the logger; do not assume.

---

## 5. Repository layout

| Path                          | What lives there                                                          | Per-dir guide                              |
|-------------------------------|---------------------------------------------------------------------------|--------------------------------------------|
| `src/`                        | All daemon source. Manager + Account + Call + utilities.                  | [src/AGENTS.md](src/AGENTS.md)             |
| `src/sip_core/`               | **Public headers** (API contract — be careful changing these).            | [src/sip_core/AGENTS.md](src/sip_core/AGENTS.md) |
| `src/client/`                 | Public-API → Manager bridge + signal dispatch + video manager glue.       | [src/client/AGENTS.md](src/client/AGENTS.md) |
| `src/sip/`                    | PJSIP integration: VoIPLink, Account, Call, SDP, Transport, Presence.     | [src/sip/AGENTS.md](src/sip/AGENTS.md)     |
| `src/media/`                  | RTP, codecs, recorder, player, congestion control.                        | [src/media/AGENTS.md](src/media/AGENTS.md) |
| `src/media/audio/`            | Audio mixer, ringbuffer, per-OS backends, audio-processing.               | [src/media/audio/AGENTS.md](src/media/audio/AGENTS.md) |
| `src/media/video/`            | Video pipeline, mixer, per-OS capture backends.                           | [src/media/video/AGENTS.md](src/media/video/AGENTS.md) |
| `src/config/`                 | YAML parser, account/preference schemas, serialization.                   | [src/config/AGENTS.md](src/config/AGENTS.md) |
| `src/connectivity/`           | IP/SIP utilities, UTF-8 helpers, security/memory utils.                   | [src/connectivity/AGENTS.md](src/connectivity/AGENTS.md) |
| `src/im/`                     | SIP MESSAGE / instant-messaging engine.                                   | [src/im/AGENTS.md](src/im/AGENTS.md)       |
| `compat/`                     | Cross-platform shims (MSVC mostly).                                       | [compat/AGENTS.md](compat/AGENTS.md)       |
| `contrib/`                    | Third-party dep recipes (per-triplet build outputs).                      | [contrib/AGENTS.md](contrib/AGENTS.md)     |
| `example/sip_cli/`            | Console example using the public API + SDL3 for video preview.            | [example/AGENTS.md](example/AGENTS.md)     |
| `tests/`                      | Unit/integration test sources (not yet wired into CTest).                 | [tests/AGENTS.md](tests/AGENTS.md)         |
| `scripts/`                    | Misc dev/diag scripts (e.g. slice-thread counter).                        | [scripts/AGENTS.md](scripts/AGENTS.md)     |
| `specs/`                      | ABI / protocol specs (binding contract is here).                          | [specs/AGENTS.md](specs/AGENTS.md)         |
| `CMakeLists.txt`              | Root build script (two-phase: contrib + library).                         | (see Build below)                          |
| `CMakePresets.json`           | Presets: `darwin`, `linux`, `win32`.                                      |                                            |
| `test.yaml`                   | Canonical config example — schema of `accounts/audio/video/preferences`.  |                                            |
| `version.h.in`                | Version templating consumed by CMake.                                     |                                            |

> Build artifacts live under `out/build/<preset>/` (presets) or `build/` (manual). Never commit them. `contrib/build-<triplet>/` holds in-tree third-party builds; `contrib/<triplet>/` holds their installed outputs.

---

## 6. Build

**Preferred — CMake presets** (`CMakePresets.json` defines `darwin`, `linux`, `win32`):

```bash
cmake --preset darwin                                    # configures + builds deps into out/build/darwin
cmake --build out/build/darwin --target sip_core         # library
cmake --build out/build/darwin --target sip_cli          # CLI example (needs -DENABLE_CLI_EXAMPLE=ON; preset turns this on)
cmake --build out/build/darwin --target install
```

**Manual** (Linux/macOS):

```bash
mkdir build && cd build
cmake .. -DBUILD_DEPS=ON -DCMAKE_INSTALL_PREFIX=/some/path -DCMAKE_BUILD_TYPE=Debug
cmake --build . --target install
```

**Windows** uses a multi-config generator: `-A x64 -Thost=x64` at configure time, then `cmake --build . --config Release --target install`. MSYS2 is required for some deps (notably ffmpeg) — see `README.md` for the GCC→MSVC `.a→.lib` rename trick in `contrib/src/ffmpeg/package.json`.

**Two-phase build** explained:
1. **Contrib** — `CMakeLists.txt` shells out to `contrib/bootstrap` + `make` to build every dep into `contrib/<triplet>/`. On Windows this is driven by `contrib/src/<dep>/package.json` + Python (no pip deps required).
2. **Library** — `sip_core` proper is built and links against the triplet-scoped deps via `CMAKE_PREFIX_PATH` / `CMAKE_FIND_ROOT_PATH`.

**Key CMake options** (see `CMakeLists.txt` for the full list):

| Option                     | Default | Purpose                                                                                       |
|----------------------------|---------|-----------------------------------------------------------------------------------------------|
| `BUILD_DEPS`               | ON      | Build deps in-tree. Turn OFF and pair with `PREBUILD_DEPS_PATHS` to use prebuilt artifacts.   |
| `PREBUILD_DEPS_PATHS`      | (empty) | Semicolon-separated list of dirs; each must contain the same triplet subdirs as `contrib/`.   |
| `ENABLE_VIDEO`             | ON      | Build video subsystem and FFmpeg video paths.                                                 |
| `ENABLE_CLI_EXAMPLE`       | OFF     | Build `sip_cli` (preset sets ON).                                                             |
| `HW_ACCEL`                 | (off)   | Enable platform HW codec accel (videotoolbox, vaapi, nvenc, …).                               |
| `CONFERENCE_METADATA`      | ON      | Send conference info as JSON over SIP (`src/conference_protocol.cpp`) and disable border overlays. |

A quick incremental build for darwin during development:

```bash
/opt/homebrew/bin/cmake --build /Users/modus.operandi/Code/sip_core/out/build/darwin --target sip_core
```

---

## 7. Tests and lint

No automated suite is committed (yet). Test sources live in `tests/` but are not wired into CTest. Manual validation:

1. Build both Debug and Release.
2. Run `sip_cli` against a config shaped like `test.yaml`.
3. For changes that affect platforms you cannot run locally, document the unverified platforms in the PR description.

Always run before submitting:

```bash
clang-format -i <file>
clang-tidy -p out/build/darwin <file>     # configs at repo root
```

> **Includes are intentionally NOT sorted.** Preserve existing include order — most files include PJSIP headers in a specific order to avoid macro clashes. `clang-format` is configured not to reorder includes (`.clang-format` at root).

---

## 8. Coding style

- C++17, 4-space indent, no tabs, 100-char line limit.
- Braces wrap on new line for classes/functions.
- Types use **CamelCase**; files/functions use existing **snake_case** patterns.
- Public symbols in the C++ API live under `namespace libsip_core`; internal daemon code lives under `namespace sip_core`. Don't confuse the two.
- Public exports are tagged `LIBSIP_CORE_PUBLIC` (defined in `src/sip_core/def.h`). Symbols needed only for tests use `LIBSIP_CORE_TESTABLE`.
- Use `NON_COPYABLE(ClassName)` for non-copyable classes.
- Log with `SIP_CORE_DBG / SIP_CORE_INFO / SIP_CORE_WARN / SIP_CORE_ERR` from `logger.h`.
- For signals emitted from PJSIP callbacks, `emitSignal<T>(...)` already dispatches onto the event thread — do not wrap further.

---

## 9. Commit & PR conventions

Conventional Commit prefixes only: `feat:`, `fix:`, `chore:`, `refactor:`, `docs:`. Keep messages imperative and scoped (e.g. `fix: handle null device name`). PRs should summarize the change, list build options and platforms tested, and include logs or screenshots for behavioral updates. Call out new dependencies or CMake options introduced.

The current branch is `feat/17`; main is `master`. Releases bump `project(sip_core VERSION x.y.z)` in the root `CMakeLists.txt` (currently `0.18.13`) plus a `chore: bump version` commit.

---

## 10. Working with this codebase as an agent — efficiency tips

- **Read this file and the relevant per-dir `AGENTS.md` before grepping.** Most "where does X live?" questions are answered above.
- **`Manager::instance()` is a singleton accessed everywhere.** When tracing a public API call into the daemon, expect: facade header → `src/client/<area>manager.cpp` → `Manager::method` → factory / Account / Call.
- **Signals are how the daemon talks back.** Search for `emitSignal<` to find every place an event is raised. Search for `CallSignal::` etc. in the relevant `_interface.h` to find every event type.
- **PJSIP types** (`pjsip_inv_session`, `pjsip_regc`, `pj_pool_t`) are everywhere in `src/sip/`. PJSIP docs: https://www.pjsip.org/ — keep a tab open.
- **FFmpeg types** (`AVFrame`, `AVPacket`, `AVCodec`) live in `src/media/`. The `MediaFrame` / `AudioFrame` / `VideoFrame` wrappers in `videomanager_interface.h` are the boundary between FFmpeg and the public API.
- **The CLI example** (`example/sip_cli/`) is the simplest end-to-end usage — read `CallController.cpp` to see how to register, place a call, accept, and route video to SDL.
- **The ABI spec** (`specs/SIP_CORE_ABI_russian.md`, Russian) is the canonical contract for the `sip_core_bindings` C wrapper that real clients link against. If you change a public signature, update this file.

<!-- MANUAL: Notes added below are preserved on regeneration. -->
