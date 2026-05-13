<!-- Parent: ../AGENTS.md -->

# src/media/video/androidvideo/ — Android camera bridge

Daemon-side stub for Android. Like iOS, **actual capture happens host-side** (Java/Kotlin Camera2 API); frames are pushed into the daemon over JNI via `captureVideoFrame` / `publishFrame`.

## Files

| File                                | Role                                                                                              |
|-------------------------------------|---------------------------------------------------------------------------------------------------|
| `video_device_impl.cpp`             | `VideoDeviceImpl` — bookkeeping only. Holds device params reported by the host via `setParameters` callback. |
| `video_device_monitor_impl.cpp`     | `VideoDeviceMonitorImpl` — bookkeeping only. Host calls `addVideoDevice(node, devInfo)` after each camera enumeration. |

## Host integration

The daemon emits these signals (declared in `videomanager_interface.h`, Android-only):

- `VideoSignal::SetParameters(device, format, width, height, rate)` — daemon asks host to apply settings.
- `VideoSignal::GetCameraInfo(device, formats, sizes, rates)` — daemon asks host to report capabilities.
- `VideoSignal::RequestKeyFrame(device)` — encoder needs an I-frame from the host's encoder (or the daemon will request a SW key-frame).
- `VideoSignal::SetBitrate(device, bitrate)` — congestion control telling the host encoder to throttle.

Host pushes frames via `captureVideoFrame(...)` (raw YUV/NV21) or `captureVideoPacket(...)` (already-encoded H.264 from MediaCodec). The encoded path skips the daemon-side encoder entirely.

## Gotchas

- **Encoded-input path**: when the host uses MediaCodec, the daemon **does not** re-encode. `VideoSender` recognizes the pre-encoded packet and just packetizes/transmits it. This avoids double-encode on mobile.
- **Camera permissions**: host responsibility. The daemon will not start a camera that the OS refused.
- **Rotation**: device rotation is reported with each frame; daemon-side `filter_transpose` applies it.

## Dependencies

- **Internal**: `media/video/video_device.h`, `sip_core/videomanager_interface.h` (Android signals).
- **External**: none directly (host owns Camera2 + MediaCodec).

<!-- MANUAL: -->
