<!-- Parent: ../AGENTS.md -->

# example/sip_cli/ — Console CLI example

Smallest possible end-to-end consumer of the public API. Reads commands from stdin (`call`, `hangup`, `hold`, `conf`, `subscribe`, `video`, `exit`, …) and uses SDL3 to display received video.

**Read this first** if you're writing a new consumer or trying to understand how the public API is meant to be used in practice.

## Layout

```
example/sip_cli/
├── CMakeLists.txt          (FetchContent SDL3 + link sip_core)
├── include/
│   ├── CallController.h    (wraps libsip_core lifecycle + signal registration)
│   ├── SignalHandlers.h    (each handler is a free function)
│   └── SDLVideoRenderer.h  (SinkTarget glue → SDL window)
└── src/
    ├── main.cpp            (REPL command loop)
    ├── CallController.cpp
    ├── SignalHandlers.cpp
    └── SDLVideoRenderer.cpp
```

## Account it uses

Hard-coded in `main.cpp`:

```cpp
#define ACCOUNT_ID "test_acc"
std::string username = "dev_user";
std::string password = "12345";
std::string domain   = "192.168.92.43";
```

So you need either a matching account in your YAML config, or you have to edit these lines. `test.yaml` at the repo root is a good template.

## What it demonstrates

- **Lifecycle**: `libsip_core::init(FLAG_DEBUG | FLAG_CONSOLE_LOG)` → `start(config_file, data_path)` → `fini()`.
- **Signal registration**: builds the `SignalHandlerMap` with `exportable_callback<...>(lambda)` per event and calls `registerSignalHandlers(map)`.
- **Outgoing call**: `placeCallWithMedia(accountId, "sip:1234@domain", {audio, video})`.
- **Incoming call**: handles `CallSignal::IncomingCallWithMedia`, calls `acceptWithMedia(...)`.
- **Conference**: `joinParticipant`, `addParticipant`, `setActiveStream`, `moveParticipant`.
- **Subscription**: `subscribeBuddy`, `publish` (presence demo).
- **Video sink**: registers a `SinkTarget` whose `push(FrameBuffer)` hands the frame to `SDLVideoRenderer`.

## Build

```bash
cmake --preset darwin                                  # enables ENABLE_CLI_EXAMPLE
cmake --build out/build/darwin --target sip_cli
./out/build/darwin/example/sip_cli/sip_cli /path/to/config.yaml
```

SDL3 is fetched via `FetchContent` (`SDL_VERSION 3.2.12`) from the local Nexus mirror. On Windows the post-build step copies `SDL3.dll` next to the binary.

## Working here

- This is the canonical reference for "how do I use the public API?" — keep it minimal. Don't grow it into a full GUI client.
- If you're testing a new public API feature, add a CLI command for it before claiming the API works.

## Dependencies

- **Internal**: `sip_core` library.
- **External**: SDL3 (fetched via `FetchContent`), platform-specific terminal APIs (`conio.h` on Windows, `termios.h` elsewhere).

<!-- MANUAL: -->
