<!-- Parent: ../AGENTS.md -->

# src/media/audio/alsa/ — Linux ALSA backend (optional)

Direct ALSA backend. **Mostly legacy** — PulseAudio (or PipeWire-Pulse) is the default Linux path. Keep this around because some embedded/server distros run without a sound server.

## Files

| File                  | Role                                                                                                      |
|-----------------------|-----------------------------------------------------------------------------------------------------------|
| `alsalayer.h/cpp`     | `AlsaLayer` — `snd_pcm_*` plumbing for capture, playback, ringtone. Uses the plug PCM (`default`, `plug:dsnoop`, `dmix/dsnoop`). |

## Configuration

Selected by `audio: audioApi: alsa` in YAML. Plugin name comes from `audio.alsa.plugin` (`default` by default). `cardIn` / `cardOut` / `cardRing` correspond to ALSA card indices.

## Gotchas

- ALSA blocking I/O is the common case here; underruns can cause stutter. The layer attempts auto-recover (`snd_pcm_recover`).
- ALSA doesn't multiplex by itself — if PulseAudio (or PipeWire) is also running, you'll likely get device-busy errors. This backend is for systems where you've intentionally bypassed the sound server.

## Dependencies

- **Internal**: `media/audio/audiolayer.h`.
- **External**: `libasound2-dev` (Debian/Ubuntu) — currently linked only if available. The build does **not** require ALSA to be present.

<!-- MANUAL: -->
