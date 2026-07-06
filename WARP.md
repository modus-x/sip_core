# WARP.md

This file provides guidance to WARP (warp.dev) when working with code in this repository.

## Build Commands

### macOS / Linux
```bash
mkdir build && cd build
cmake .. -DBUILD_DEPS=ON -DCMAKE_INSTALL_PREFIX=/opt/sip-core -DCMAKE_BUILD_TYPE=Debug
cmake --build . --target install
```

### Windows (MSVC)
```bash
mkdir build && cd build
cmake .. -DBUILD_DEPS=ON -DCMAKE_INSTALL_PREFIX=/some/path -A x64 -Thost=x64
cmake --build . --config Debug --target install
```

### CLI Example
```bash
cmake --build build --target sip_cli
```

### CMake Presets (Recommended)
```bash
cmake --preset darwin   # macOS with Ninja
cmake --preset Windows  # Windows with VS 2022
cmake --build out/build/darwin --target install
```

## CMake Options
- `BUILD_DEPS` (ON/OFF) - Build third-party dependencies (set OFF to use prebuilt)
- `ENABLE_VIDEO` (ON/OFF) - Video support
- `ENABLE_CLI_EXAMPLE` (ON/OFF) - Build the `sip_cli` console example
- `PREBUILD_DEPS_PATHS` - Path(s) to prebuilt dependencies when `BUILD_DEPS=OFF`

## Architecture

### Core Layers
- **Manager** (`src/manager.h/cpp`) - Central controller/singleton coordinating all subsystems
- **AccountFactory/CallFactory** - Create and manage SIP accounts and calls
- **Conference** - Multi-party call handling with video layout support

### SIP Stack (`src/sip/`)
- Built on PJSIP (pjproject). Key classes:
  - `SIPAccount` / `SIPAccountBase` - Account state, registration, credentials
  - `SIPCall` - Call lifecycle, media negotiation
  - `SIPVoIPLink` - PJSIP integration layer
  - `SDP` - Session description parsing/generation
  - `SIPTransport` - UDP/TCP/TLS transports

### Media (`src/media/`)
- **Audio**: Platform backends in subdirectories:
  - `coreaudio/` (macOS/iOS), `pulseaudio/` (Linux), `wasapi/` (Windows), `opensl/` (Android)
  - `audio-processing/` - WebRTC audio processing (noise suppression, echo cancellation, AGC)
- **Video**: Platform backends:
  - `osxvideo/`, `iosvideo/` (Apple), `v4l2/` (Linux), `winvideo/` (Windows), `androidvideo/`
  - Video encoding/decoding via FFmpeg

### Public API (`src/sip_core/`)
Headers exposed to consumers:
- `sip_core.h` - Initialization (`init`, `start`, `fini`), callback registration
- `callmanager_interface.h` - Call control (place, answer, hangup, hold, transfer, conference)
- `configurationmanager_interface.h` - Account/device/audio configuration
- `videomanager_interface.h` - Video device and rendering control
- `presencemanager_interface.h` - Presence/subscription management

### Dependencies (`contrib/`)
Third-party libraries built per-triplet (e.g., `x86_64-apple-darwin24.0.0`):
- FFmpeg (with patches), pjproject (patched), opus, vpx, x264
- yaml-cpp, jsoncpp, fmt, webrtc-audio-processing

Build rules: `contrib/src/<dep>/rules.mak` (Unix) or `package.json` (Windows)

## Coding Style
- C++17, 4-space indent, 100-char line limit
- Braces on new line for classes/functions
- Run `clang-format -i <file>` and `clang-tidy -p build <file>` before committing
- Includes are intentionally NOT auto-sorted

## Configuration
The library reads YAML config files (see `test.yaml` for structure):
- `accounts` - SIP account definitions
- `audio` - Audio device and processing settings
- `video` - Video device configuration
- `preferences` / `voipPreferences` - General settings
