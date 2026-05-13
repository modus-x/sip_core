<!-- Parent: ../AGENTS.md -->

# src/media/audio/jack/ — JACK backend (optional)

Optional JACK Audio Connection Kit backend, for pro-audio / low-latency setups on Linux or macOS where the user has explicitly chosen JACK.

## Files

| File                  | Role                                                                                              |
|-----------------------|---------------------------------------------------------------------------------------------------|
| `jacklayer.h/cpp`     | `JackLayer` — registers JACK ports, copies between JACK's float buffers and the daemon's PCM.    |

## Configuration

Selected by `audio: audioApi: jack` in YAML, or by `Manager::setAudioManager("jack")`. JACK must already be running; the daemon will not start it.

## Gotchas

- JACK is sample-rate driven by the JACK server — the daemon's mixer must adapt. `Manager::hardwareAudioFormatChanged` handles this.
- Ports auto-connect to system input/output ports if the user hasn't manually wired them, but you may want to do the wiring in `qjackctl`.

## Dependencies

- **Internal**: `media/audio/audiolayer.h`.
- **External**: `libjack` (system package or `contrib/src/jack/` for a vendored fallback).

<!-- MANUAL: -->
