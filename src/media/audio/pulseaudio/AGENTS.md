<!-- Parent: ../AGENTS.md -->

# src/media/audio/pulseaudio/ — Linux backend (PulseAudio)

Default Linux `AudioLayer`. Uses `libpulse`'s **asynchronous mainloop** API (`pa_threaded_mainloop`) and PA streams.

## Files

| File                          | Role                                                                                              |
|-------------------------------|---------------------------------------------------------------------------------------------------|
| `pulselayer.h/cpp`            | `PulseLayer` — derives from `AudioLayer`. Owns the PA context + threaded mainloop, opens capture + playback + ringtone streams, listens for sink/source list changes, handles default-device tracking. |
| `audiostream.h/cpp`           | `AudioStream` — thin wrapper around `pa_stream` covering open/connect/cork/drain/teardown.        |

## Behavior

- **Three streams** per call (typical): `playback`, `capture`, `ringtone` — each its own `AudioStream`.
- **Default device tracking**: subscribes to PA server info events so changing the system default device is picked up automatically.
- **Hot-plug**: PA `subscribe` events for sinks/sources trigger `Manager::onAudioDevicesChanged()`.
- **Volume**: per-stream PA volume; `Manager::setVolume("speaker"|"mic", value)` maps to the right stream.

## Build

Requires `libpulse` at build time (`pkg_search_module(pulseaudio REQUIRED libpulse)` in root `CMakeLists.txt`). On distros without PulseAudio dev headers the build fails; install `libpulse-dev` (Debian/Ubuntu) or `pulseaudio-libs-devel` (RHEL).

PipeWire is wire-compatible via `pipewire-pulse`, so `PulseLayer` works on a PipeWire system without changes.

## Gotchas

- Always lock the PA mainloop (`pa_threaded_mainloop_lock`) before touching streams. `AudioStream`'s methods do this for you; if you write new code that touches `pa_stream` directly, lock first.
- PA stream latency reports include the server-side buffer; don't compare them to the local-only latency.

## Dependencies

- **Internal**: `media/audio/audiolayer.h`.
- **External**: `libpulse` (system package; not built in contrib).

<!-- MANUAL: -->
