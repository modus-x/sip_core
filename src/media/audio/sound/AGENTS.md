<!-- Parent: ../AGENTS.md -->

# src/media/audio/sound/

Generated and file-backed audio sources played to the user: DTMF tones, ringback / busy / congestion tones, and user-configurable ringtones.

## Files

| File                          | Role                                                                                            |
|-------------------------------|-------------------------------------------------------------------------------------------------|
| `audiofile.h/cpp`             | WAV/Ogg file loader (PCM). `AudioFile` is an `AudioLoop` source — used for ringtones and recorded-file playback. |
| `tone.h/cpp`                  | Single tone generator. `Tone` is an `AudioLoop` produced by additive sine synthesis from a `definition` string. |
| `tonelist.h/cpp`              | Pre-defined zone-specific tone definitions (`Russia`, `North America`, …). `Manager::preferences.zoneToneChoice` selects. Provides `getTelephoneTone(state)` mapping CallState/ConnectionState to the right tone. |
| `dtmf.h/cpp`                  | DTMF tone container — holds a generated DTMF tone for a digit.                                  |
| `dtmfgenerator.h/cpp`         | Generates the dual-frequency wave for a DTMF digit on demand; cached per-digit.                |

## Wiring

`Manager::playTone()`, `Manager::ringback()`, `Manager::congestion()`, `Manager::callBusy()`, `Manager::callFailure()`, `Manager::playRingtone()`, `Manager::playDtmf()` all read from this directory and route through `AudioInput` / `AudioLayer` to the ringtone or playback stream.

`Manager::startRecordedFilePlayback(path)` plays an arbitrary file (uses `AudioFile`).

## Common gotchas

- `Tone` definitions are strings like `"425/200,0/200"` (freq/duration pairs). Read `tone.cpp::Tone::genSine`.
- Ringtone path resolution lives on `Account::setRingtone` — pass a *filename*, full path is resolved against the account's ringtone directory inside `data_path`.
- For incoming calls with an `Alert-Info` header, `Account::pauseAfterAlertInfo` delays the default ringtone so the client can call `setRingtoneForIncomingCall(...)` with a downloaded custom file.

## Dependencies

- **Internal**: `audio/audioloop.h`, `audio/audiolayer.h`.
- **External**: none beyond standard FFmpeg already in scope.

<!-- MANUAL: -->
