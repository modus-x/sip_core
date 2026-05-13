<!-- Parent: ../AGENTS.md -->

# src/media/audio/ — Audio pipeline

Audio ingestion, mixing, RTP send/recv, file playback, DTMF, ring tones, plus all per-OS audio backends.

The audio mixer is **`RingBufferPool` + `AudioInput`**: capture devices, file players, and incoming RTP streams all write into named ring buffers, and `AudioInput` mixes/dispatches them to subscribers (encoders, the playback device, recorders).

## Top-level files

| File                              | Role                                                                                                  |
|-----------------------------------|-------------------------------------------------------------------------------------------------------|
| `audiolayer.h/cpp`                | Abstract `AudioLayer` — every platform backend (CoreAudio, PulseAudio, …) derives from this. Manages capture/playback/ringtone streams and per-stream `RingBuffer`s. |
| `audio_input.h/cpp`               | Mixer source — one per call (or shared). Pulls from capture + applies `AudioProcessor` (NS/AEC/AGC). |
| `audio_sender.h/cpp`              | Encodes audio frames and writes to the RTP socket pair via `MediaEncoder`.                            |
| `audio_receive_thread.h/cpp`      | Reads RTP, decodes, pushes into a ring buffer for the mixer.                                          |
| `audio_rtp_session.h/cpp`         | Concrete `RtpSession` for audio — wires sender + receive thread + recorder.                           |
| `audiobuffer.h/cpp`               | `AudioBuffer` + `AudioFormat` (sample rate × channels × format). Used everywhere PCM crosses a boundary. |
| `audio_frame_resizer.h/cpp`       | Splits/joins frames to the encoder's expected frame size (e.g. 20ms for Opus).                        |
| `audioloop.h/cpp`                 | Looping audio source (used for ringback / hold tone / DTMF generator).                                |
| `ringbuffer.h/cpp`                | Lock-free single-producer multi-consumer ring buffer.                                                 |
| `ringbufferpool.h/cpp`            | Registry + binding (who reads from whom). Determines mixer topology.                                  |
| `resampler.h/cpp`                 | libswresample wrapper.                                                                                |
| `dcblocker.h/cpp`                 | DC-bias remover applied before encoding.                                                              |
| `dsp.h/cpp`                       | Speex-DSP wrapper for preprocessing (echo state, denoise) when the WebRTC processor is off.           |
| `tonecontrol.h/cpp`               | Dispatches tones (busy, ringback, congestion) based on call state.                                    |
| `g729_decoder.cpp/.h`, `g729_encoder.cpp/.h` | Direct bcg729 codec wrappers (FFmpeg doesn't ship G.729).                                |

## Subdirectories

| Dir                    | Role                                                                                | Guide                                                                |
|------------------------|-------------------------------------------------------------------------------------|----------------------------------------------------------------------|
| `audio-processing/`    | WebRTC NS/AEC/AGC + null processor (the "off" choice).                              | [audio-processing/AGENTS.md](audio-processing/AGENTS.md)             |
| `sound/`               | Tone generation, DTMF generator, audio file (WAV) loader.                           | [sound/AGENTS.md](sound/AGENTS.md)                                   |
| `coreaudio/`           | macOS + iOS backend (Core Audio).                                                   | [coreaudio/AGENTS.md](coreaudio/AGENTS.md)                           |
| `pulseaudio/`          | Linux backend (PulseAudio).                                                         | [pulseaudio/AGENTS.md](pulseaudio/AGENTS.md)                         |
| `portaudio/`           | Windows backend (PortAudio).                                                        | [portaudio/AGENTS.md](portaudio/AGENTS.md)                           |
| `opensl/`              | Android backend (OpenSL ES).                                                        | [opensl/AGENTS.md](opensl/AGENTS.md)                                 |
| `alsa/`                | Optional Linux backend (ALSA direct). Mostly legacy.                                | [alsa/AGENTS.md](alsa/AGENTS.md)                                     |
| `jack/`                | Optional JACK backend (Linux/macOS pro audio).                                      | [jack/AGENTS.md](jack/AGENTS.md)                                     |

## Audio data flow

```
   capture device                file/playback                incoming RTP
        |                              |                            |
        v                              v                            v
   +-----------+                  +-----------+              +---------------------+
   | AudioLayer|                  | AudioLoop |              | AudioReceiveThread  |
   +-----+-----+                  +-----+-----+              +----------+----------+
         |                              |                                |
         +------+------------+---------+--+                              |
                |            |                                            |
                v            v                                            v
            +------+    +---------+                                  +----------+
            | Ring | -->| Audio   |---- AudioProcessor (WebRTC) ---->| AudioSender|--> RTP
            |Buffer|    | Input   |                                  +----------+
            +------+    +---------+
                            |
                            v
                    playback device (back through AudioLayer)
                            +
                    MediaRecorder (if recording)
```

`RingBufferPool::bindCallID(callId, peerId)` is what makes the mixer topology — bind two calls and they hear each other; bind to `RingBufferPool::DEFAULT_ID` and you talk to the local mic/speaker.

## Selecting the audio backend

`Manager::setAudioManager(api)` accepts `"coreaudio"`, `"pulseaudio"`, `"alsa"`, `"jack"`, `"portaudio"`, `"opensl"`. Default is platform-derived. Backend factory selection is in `manager.cpp::createAudioLayer` (search for the API name strings defined at the top of `audiolayer.h`).

## Working in this directory

- **Frame sizes**: Opus expects 20ms by default; PCMU/PCMA expect 20ms; G.722 expects 20ms at 16 kHz. `AudioFrameResizer` handles the impedance mismatch — don't bypass it.
- **Sample-rate negotiation**: `Manager::audioFormatUsed(format)` is called by external sources (e.g. video player audio track) to negotiate the mixer's working format. Higher quality wins.
- **AEC reference**: WebRTC's AEC needs a far-end reference — that's piped through `AudioProcessor::analyzeReverse()`. If you split the playback path, the AEC will degrade.
- **Mute** is implemented at multiple levels (capture, sender, RTP, mixer) — read `Call::muteMedia` for the canonical path.

## Dependencies

- **Internal**: `media/` parent (`media_codec`, `media_filter`, `media_buffer`, etc.).
- **External**: FFmpeg (libswresample for resampling), libspeex/speexdsp, libopus (via FFmpeg), bcg729 (for G.729), webrtc-audio-processing.

<!-- MANUAL: -->
