<!-- Parent: ../AGENTS.md -->

# src/media/ — Media pipeline

RTP sending/receiving, codecs (FFmpeg + pjmedia for some legacy), media recording, file playback, congestion control, and the **socket pair reservation** mechanism used by `SIPCall` to allocate consecutive RTP/RTCP UDP ports.

Audio-specific code lives in [`audio/`](audio/AGENTS.md); video-specific code in [`video/`](video/AGENTS.md). Code in this directory is shared between the two.

## Files

| File                                | Role                                                                                                  |
|-------------------------------------|-------------------------------------------------------------------------------------------------------|
| `rtp_session.h`                     | Abstract `RtpSession` — base for `AudioRtpSession` (audio/) and `VideoRtpSession` (video/). Owns peer endpoint, mute state, the `MediaDescription` for send/recv. |
| `media_codec.h/cpp`                 | `SystemCodecInfo` / `AccountCodecInfo` (system catalog + per-account overrides). `MediaType` and `CodecType` enums. |
| `system_codec_container.h/cpp`      | Singleton catalog of every available codec on the system; populated at startup from FFmpeg + the bundled pjmedia codecs (opus, g729, etc.). |
| `media_attribute.h/cpp`             | `MediaAttribute` — wire-format-agnostic representation of a media stream (type, enabled, muted, sourceUri, label, secure). Mirrors what consumers see in `MediaMap`. |
| `media_buffer.h/cpp`                | Owns `AVFrame` / `AVPacket` lifecycle for the daemon — the base behind the public `MediaFrame`.       |
| `media_codec.h/cpp`, `media_codec.cpp` | Codec parameter manipulation.                                                                       |
| `media_decoder.h/cpp`, `media_decoder_base.h` | FFmpeg-backed decoder; used by audio/video receive threads.                                    |
| `media_encoder.h/cpp`, `media_encoder_base.h` | FFmpeg-backed encoder; used by senders.                                                          |
| `media_device.h/cpp`                | `DeviceParams` for camera/mic device specs.                                                           |
| `media_filter.h/cpp`                | FFmpeg filter graph (`libavfilter`) — used for resampling, format conversion, image overlays.         |
| `media_stream.h`                    | `MediaStream` info struct used between encoder/decoder/recorder.                                      |
| `media_io_handle.h/cpp`             | FFmpeg `AVIOContext` adapter for socket-backed RTP.                                                   |
| `media_player.h/cpp`                | File-based playback (audio or video) into the call.                                                   |
| `media_recorder.h/cpp`              | Mux audio+video into MKV; one per call/conference. Public API exposes start/stop via `videomanager_interface.h`. |
| `localrecorder.h/cpp`               | Single-stream recording from a local source (vs. recording a call). Driven by `startLocalMediaRecorder` in the public API. |
| `localrecordermanager.h/cpp`        | Registry of active local recorders.                                                                   |
| `recordable.h/cpp`                  | Mixin for things that can be recorded (Call, Conference).                                             |
| `peerrecorder.h`                    | Interface for "the peer is recording" notifications (used by `SIPCall`).                              |
| `congestion_control.h/cpp`          | TMMBR-/REMB-style congestion control for video — adjusts encoder bitrate based on RTCP reports.       |
| `socket_pair.h/cpp`                 | UDP socket pair (RTP+RTCP) abstraction. `ReservedSocketPair` reserves a consecutive port pair before SDP creation so re-INVITEs don't drift. Used heavily by `SIPCall::prepareLocalMediaReservations`. |
| `srtp.h`, `srtp.c`                  | SRTP context wrapper (libsrtp).                                                                       |
| `decoder_finder.h`                  | Helper for matching FFmpeg decoder by codec name.                                                     |
| `libav_deps.h`, `libav_utils.h/cpp` | FFmpeg version shims (log redirect, `AVERROR` translation, pixel-format helpers).                     |

## Subdirectories

| Dir       | What                                                                          | Guide                                              |
|-----------|-------------------------------------------------------------------------------|----------------------------------------------------|
| `audio/`  | Audio mixer, ring-buffer pool, per-OS backends, audio-processing (NS/AEC/AGC). | [audio/AGENTS.md](audio/AGENTS.md)                |
| `video/`  | Video input/mixer/scaler, per-OS capture backends, FFmpeg encode/decode glue.  | [video/AGENTS.md](video/AGENTS.md)                |

## Common patterns

- An `RtpSession` is owned by a `SIPCall::RtpStream`; one per media stream. `start()` / `stop()` are called from `SIPCall::startAllMedia` / `stopAllMedia`.
- `ReservedSocketPair` is **how port reservation survives re-INVITE.** Always reserve before building the SDP that will publish those ports.
- `MediaRecorder` writes to an MKV; encoder/decoder pipelines route to it when recording is on. Use `Recordable::startRecording(path)` to turn it on.
- FFmpeg logging goes through `libav_utils.cpp`'s `av_log_callback` — wired to `SIP_CORE_*` macros.

## Threading

- **Receive thread**: `AudioReceiveThread` / `VideoReceiveThread` (in `audio/` / `video/`). Reads from socket pair, decodes, dispatches frames.
- **Sender thread**: `AudioSender` / `VideoSender`. Encodes, paces, writes to socket pair.
- **Mixer threads**: `AudioInput` + `RingBufferPool` (audio side), `VideoMixer` (video side) — both schedule on their own `ThreadLoop`.

## Dependencies

- **Internal**: `connectivity/ip_utils`, `connectivity/sip_utils`, `sip/sipaccount.h` (for some calls).
- **External**: FFmpeg (libav*), libsrtp (vendored in PJSIP), webrtc-audio-processing (audio NS/AEC/AGC).

<!-- MANUAL: -->
