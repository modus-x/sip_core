@AGENTS.md

# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Shared contributor conventions (structure, style, commits) live in [AGENTS.md](AGENTS.md). Read that first — the notes below only cover what Claude needs in addition.

## Build

Two-phase CMake build: phase 1 configures and (optionally) builds third-party deps into per-triplet folders under `contrib/<triplet>/` (e.g. `x86_64-apple-darwin24.0.0`); phase 2 builds the `sip_core` library, linking against those triplet-scoped deps via `CMAKE_PREFIX_PATH` / `CMAKE_FIND_ROOT_PATH`.

Preferred path is CMake presets (see `CMakePresets.json`):

```bash
cmake --preset darwin                                    # configures into out/build/darwin
cmake --build out/build/darwin --target sip_core         # library
cmake --build out/build/darwin --target sip_cli          # CLI example (requires -DENABLE_CLI_EXAMPLE=ON)
cmake --build out/build/darwin --target install
```

Manual configure (Linux/macOS):

```bash
mkdir build && cd build
cmake .. -DBUILD_DEPS=ON -DCMAKE_INSTALL_PREFIX=/some/path -DCMAKE_BUILD_TYPE=Debug
cmake --build . --target install
```

Windows uses a multi-config generator: pass `-A x64 -Thost=x64` at configure, then `cmake --build . --config Release --target install`.

Key CMake options: `BUILD_DEPS` (off when deps are prebuilt — pair with `PREBUILD_DEPS_PATHS` pointing at a directory containing the same triplet layout as `contrib/`), `ENABLE_VIDEO`, `ENABLE_CLI_EXAMPLE`, `HW_ACCEL`, `CONFERENCE_METADATA`.

Dep recipes: Unix uses `contrib/src/<dep>/rules.mak`; Windows uses `contrib/src/<dep>/package.json` driven by Python (no pip deps). On Windows, gcc-built `.a` archives are renamed to `.lib` so MSVC can link them — see `contrib/src/ffmpeg/package.json` for the canonical pattern.

## Tests / lint

No automated test suite is committed. Validation is: build both Debug and Release, and exercise `sip_cli` against a config shaped like `test.yaml`. Before submitting changes run `clang-format -i <file>` and `clang-tidy -p <build-dir> <file>` (configs at repo root). Includes are deliberately NOT sorted — preserve existing order.

## Architecture (big picture)

The library wraps a patched PJSIP stack and exposes a C-style facade. Three layers worth knowing before editing across files:

1. **Facade — `src/sip_core/`.** Public headers consumers link against: `sip_core.h` (lifecycle: `init`/`start`/`fini`, callback registration), `callmanager_interface.h`, `configurationmanager_interface.h`, `videomanager_interface.h`, `presencemanager_interface.h`. Treat these as the API contract; changes here ripple to every consumer.

2. **Core — `src/`.** `Manager` (`manager.h/cpp`) is the central singleton wiring everything together. `AccountFactory` / `CallFactory` (and `account*.cpp`, `call*.cpp`) own account and call lifecycles. `Conference` handles multi-party calls; `conference_protocol.*` carries metadata over SIP when `CONFERENCE_METADATA=ON`. YAML config is loaded via `src/config/` and the `*_schema.h` files; `test.yaml` shows the shape (`accounts`, `audio`, `video`, `preferences`, `voipPreferences`).

3. **Stack — `src/sip/` + `src/media/`.** `src/sip/` is the PJSIP integration: `SIPVoIPLink` is the PJSIP bridge, `SIPAccount`/`SIPAccountBase` handle registration and credentials, `SIPCall` drives call state and media negotiation, `SDP` parses/generates session descriptions, `SIPTransport` covers UDP/TCP/TLS. `src/media/` has per-platform audio backends (`coreaudio/`, `pulseaudio/`, `portaudio/`, `opensl/`) plus `audio-processing/` (WebRTC NS/AEC/AGC), and per-platform video backends (`osxvideo/`, `iosvideo/`, `v4l2/`, `winvideo/`, `androidvideo/`) with FFmpeg encode/decode. `src/client/`, `src/connectivity/`, `src/im/`, `src/compat/` are support modules.

When a change crosses these layers (e.g. a new call-control verb), it typically touches the facade header, a manager method, a factory, and the SIP/media implementation — grep for an existing verb to find the pattern.
