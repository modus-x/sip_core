<!-- Parent: ../AGENTS.md -->

# src/media/audio/audio-processing/

WebRTC-based audio preprocessing: noise suppression (NS), acoustic echo cancellation (AEC), automatic gain control (AGC), and voice activity detection (VAD).

## Files

| File                          | Role                                                                                            |
|-------------------------------|-------------------------------------------------------------------------------------------------|
| `audio_processor.h`           | Abstract `AudioProcessor` interface. Two methods: `analyzeReverse(frame)` (far-end ref) and `getProcessed(frame)` (near-end → processed). Plus per-feature toggles. |
| `webrtc.h`, `webrtc.cpp`      | Implementation backed by Google's `webrtc-audio-processing` (the standalone APM library). Wires every knob from `setNoiseSuppressState`, `setEchoCancellerState`, `setAGCState`, `setVADState`, `setVADSensitivity`, `setWebRtcParams`. |
| `null_audio_processor.h/cpp`  | No-op implementation; used when `audioProcessor: null` is set in config.                        |

## Configuration

YAML config under `audio:` selects the processor:

```yaml
audio:
  audioProcessor: webrtc        # or "null"
  noiseReduce: off|low|moderate|high
  echoCancel: off|on
  voiceActivityDetection: false
  automaticGainControl: true
```

Plus the WebRTC-only tunables from `configurationmanager_interface.h::WebRtcParams`:
`targetLevelDbfs`, `compressionGainDb`, `limiter`, `experimentalNs`, `noiseGen`.

## Wiring

`AudioInput` (in [`../audio_input.cpp`](../audio_input.cpp)) holds a `unique_ptr<AudioProcessor>` and calls:

```cpp
processor_->analyzeReverse(playbackFrame);   // far-end ref (what we play)
auto out = processor_->getProcessed(captureFrame); // near-end (mic)
```

The processor instance is replaced when the user calls `setAudioProcessor("webrtc"|"null")` on the public API. Hot-swap is safe because `AudioInput` is single-threaded for processing.

## Gotchas

- The WebRTC APM operates on 10ms frames at a fixed sample rate (usually 16 or 32 kHz). `AudioFrameResizer` upstream of the processor is mandatory; if you bypass it the APM will throw.
- AEC needs both directions to share a timebase. If the playback path is rerouted (e.g. via a different `AudioLayer`), the far-end reference may stall and the AEC degrades — `analyzeReverse` must continue to be called every 10ms.
- VAD output is exposed through `AudioFrame::has_voice` (set after processing). Conference voice-activity detection reads this — see `Conference::updateVoiceActivity`.

## Dependencies

- **External**: `webrtc-audio-processing` (built from `contrib/src/webrtc-audio-processing/`).

<!-- MANUAL: -->
