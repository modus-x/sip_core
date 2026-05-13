<!-- Parent: ../AGENTS.md -->

# src/client/ — Public API implementation + signal dispatch

Thin glue between [`src/sip_core/`](../sip_core/AGENTS.md) headers (the contract) and [`Manager`](../manager.h)/factories (the implementation).

Despite the name, this is **not** "the client" — it's the *server-side* implementation of what clients call. The actual client lives outside the repo (or in `example/sip_cli/` for the in-tree demo).

## Files

| File                          | Role                                                                                                |
|-------------------------------|-----------------------------------------------------------------------------------------------------|
| `callmanager.cpp`             | Implements every function declared in `sip_core/callmanager_interface.h`. Each function ≈ "ask Manager / Account / Call to do X." |
| `configurationmanager.cpp`    | Implements `configurationmanager_interface.h`. Account CRUD, codec lists, audio device control, WebRTC processor params, push tokens. |
| `presencemanager.cpp`         | Implements `presencemanager_interface.h`. Forwards to `SIPAccount::getPresence()` / `SIPEvents`.    |
| `videomanager.cpp`            | Implements `videomanager_interface.h`. Device monitor, sink registration, video input / media player. |
| `videomanager.h`              | Declares the in-process `VideoManager` struct (held by `Manager`) and helpers (`getVideoInput`, `getAudioInput`, `createMediaPlayer`, …). |
| `ring_signal.h`               | **Signal plumbing.** Defines `emitSignal<T>(args...)` and the global `SignalHandlerMap` + `eventScheduler`. Every callback to the host goes through here. |
| `ring_signal.cpp`             | Storage for the handler map + scheduler instance.                                                   |

## How emitSignal works

```cpp
emitSignal<libsip_core::CallSignal::StateChange>(accId, callId, "RINGING", 0);
```

1. Looks up the handler by `CallSignal::StateChange::name` ("StateChange").
2. Wraps the args in a tuple and posts onto `eventScheduler` (on non-Android platforms) — this guarantees host code runs off the PJSIP thread.
3. Calls the handler; catches and logs exceptions so a buggy host can't crash the daemon.
4. Emits LTTng tracepoints on entry/exit.

On Android, the call is synchronous to interop with JNI thread attachment.

## Working in this directory

- **Don't put business logic here.** These files should be 1–10 lines per function: validate args, call `Manager::instance()` / `Account` / `Call`, return.
- **Adding a new public function** → add to the `_interface.h`, then implement here. The body is usually `return Manager::instance().X(...)`.
- **Adding a new signal** → declare the struct in the corresponding `*Signal` namespace in the public header, then emit it from wherever the event occurs (often `src/sip/sipcall.cpp` or `src/manager.cpp`).
- **VideoManager glue is heavier** because it owns the device monitor + active input cache. Read `videomanager.h` carefully.

## Common gotchas

- `emitSignal<T>` will log an error (and silently no-op) if no handler is registered. If a signal seems lost, double-check the host actually registered it via `registerSignalHandlers`.
- The handler map is global state. Calling `registerSignalHandlers` replaces previously registered handlers for the same keys; `unregisterSignalHandlers` clears all.
- `eventScheduler` is initialized lazily on `init()` and torn down on `fini()`. Signals emitted before `init()` (e.g. from a misordered host) are dropped.

## Dependencies

- **Internal**: every public header in `sip_core/`, plus `manager.h`, `account_factory.h`, `call_factory.h`, `scheduled_executor.h`.
- **External**: PJSIP types appear in some signatures (e.g. `pjsip_evsub_state`).

<!-- MANUAL: -->
