<!-- Parent: ../AGENTS.md -->

# src/media/video/ — Video pipeline

Video capture, encoding, RTP transport, decoding, scaling, mixing (for conferences), sink delivery to clients. Gated by `ENABLE_VIDEO=ON` (the default; disable for headless/audio-only builds).

## Top-level files

| File                              | Role                                                                                            |
|-----------------------------------|-------------------------------------------------------------------------------------------------|
| `video_base.h/cpp`                | Shared video plumbing: `VideoFrameActiveWriter`, observer interfaces, generator base class.     |
| `video_device.h`                  | Abstract `VideoDeviceImpl` — implemented by each per-OS subdirectory.                           |
| `video_device_monitor.h/cpp`      | Watches the system for camera add/remove events; maintains the active device list.              |
| `video_input.h/cpp`               | Captures from a `VideoDeviceImpl` (or a file via FFmpeg) into a frame stream.                   |
| `video_source_utils.h/cpp`        | Helpers for resolving input strings (`camera://...`, `file://...`, `display://...`).            |
| `video_sender.h/cpp`              | Encoder + RTP packetizer pipeline. Reads from a `VideoFrameActiveWriter`, writes to a socket pair. |
| `video_receive_thread.h/cpp`      | RTP depacketizer + FFmpeg decoder pipeline.                                                     |
| `video_rtp_session.h/cpp`         | Concrete `RtpSession` for video. Wires sender, receive thread, recorder. Drives `CongestionControl`. |
| `video_mixer.h/cpp`               | Conference video mixer. Composes multiple `VideoFrameActiveWriter`s into a single output stream, respecting layout. |
| `video_scaler.h/cpp`              | libswscale wrapper — pixel-format + size conversion.                                            |
| `accel.h/cpp`                     | Hardware-acceleration selection (VideoToolbox, VAAPI, NVENC/NVDEC, AMF, MediaFoundation). Gated by `HW_ACCEL=ON`. |
| `filter_transpose.h/cpp`          | FFmpeg filter for rotating/flipping frames according to device orientation.                     |
| `sinkclient.h/cpp`                | `SinkClient` — the consumer-facing handle for a video stream. Holds shared memory (`shm_*`) and/or a callback `SinkTarget` registered via `registerSinkTarget(...)`. |
| `shm_header.h`                    | Header struct for the shared-memory ring used to ship frames to clients without copying.        |

## Subdirectories (platform capture backends)

| Dir              | Platform                       | Notes                                                                                                |
|------------------|--------------------------------|------------------------------------------------------------------------------------------------------|
| `osxvideo/`      | macOS                          | AVFoundation — `.mm` files (Objective-C++).                                                          |
| `iosvideo/`      | iOS                            | AVFoundation; daemon-side stub — actual capture happens host-side, frames are pushed in via JNI-like glue. |
| `v4l2/`          | Linux                          | Video4Linux2 ioctl-based capture; webcam + V4L2 loopback.                                            |
| `winvideo/`      | Windows                        | DirectShow (`capture_graph_interfaces.h`).                                                           |
| `uwpvideo/`      | Windows UWP                    | UWP MediaCapture API.                                                                                |
| `androidvideo/`  | Android                        | Daemon-side stub — host pushes frames via `addVideoDevice` / `captureVideoFrame` JNI glue.           |

Each platform subdir implements two classes: `VideoDeviceImpl` (device handle: open/close/capabilities/settings) and `VideoDeviceMonitorImpl` (hot-plug listener). See individual subdirectory AGENTS.md.

## Video data flow

```
   camera / file / desktop                    incoming RTP
            |                                       |
            v                                       v
       +---------+                          +----------------------+
       | Video   |                          | VideoReceiveThread   |
       | Input   |                          | (FFmpeg decoder)     |
       +----+----+                          +----+-----------------+
            |                                    |
            v                                    v
   +---------------------+                +-------+--------+
   | VideoFrameActiveWriter|------------->|  SinkClient    |--> SHM / SinkTarget cb (to host)
   +---------+-----------+                +----------------+
             |                                    |
             |  (in conferences)                  |  (also recorded if recording on)
             v                                    v
       +------------+                       MediaRecorder
       | VideoMixer |
       +-----+------+
             |
             v
       +-------------+
       | VideoSender |--- FFmpeg encoder --- socket pair (RTP)
       +-------------+
```

Sinks are the public surface: clients call `registerSinkTarget(sinkId, target)` (videomanager_interface.h) and receive frames through `target.push(FrameBuffer)`.

## Working in this directory

- **Hardware accel**: read `accel.cpp` — selection is per-platform and per-codec. Hard-coded fallbacks to software decode if HW init fails. Don't add new HW paths without a robust fallback.
- **Decoder restarts**: on resolution change, decoders are torn down and re-created. `VideoReceiveThread::setDecoderProperties` triggers this.
- **Bitrate adaptation**: `CongestionControl` reads RTCP receiver reports and adjusts encoder bitrate via `VideoSender::setBitrate`. Don't bypass — congestion control prevents catastrophic packet loss.
- **Mixer layouts**: `setConferenceLayout(layout)` is an integer; `VideoMixer` interprets it. Layout 0 = auto grid, others are protocol-specific (see `conference_protocol.cpp`).
- **Orientation**: device orientation is reported via `setDeviceOrientation`. `filter_transpose` rotates the captured stream. Encoders are told via SIP INFO (see `SIPCall::setVideoOrientation`).
- **Display capture** ("desktop"): on macOS/Linux uses FFmpeg's `avfoundation` / `x11grab` / Windows `gdigrab`. Patch trail in `contrib/src/ffmpeg/screen-sharing-x11-fix.patch`.

## Dependencies

- **Internal**: `media/` parent.
- **External**: FFmpeg (libavformat, libavcodec, libavfilter, libavdevice, libswscale), per-platform capture frameworks.

<!-- MANUAL: -->
