<!-- Parent: ../AGENTS.md -->

# src/media/video/iosvideo/ — iOS camera bridge

Daemon-side stub for iOS video. **Actual capture happens host-side** because iOS access to `AVCaptureSession` is owned by the app process; the daemon receives frames pushed via the public API.

## Files

| File                                | Role                                                                                              |
|-------------------------------------|---------------------------------------------------------------------------------------------------|
| `video_device_impl.cpp`             | `VideoDeviceImpl` — bookkeeping only. Stores size/rate/format info supplied by the host. On `setDeviceParams`, emits `VideoSignal::ParametersChanged` to notify the daemon pipeline of new settings. Frames themselves arrive via the `getNewFrame()` / `publishFrame()` free functions in `videomanager_interface.h`. |
| `video_device_monitor_impl.cpp`     | `VideoDeviceMonitorImpl` — stub with an empty `run()`. Hot-plug is driven entirely by the host calling `addVideoDevice(node, devInfo)` / `removeVideoDevice(node)` from `videomanager_interface.h`. |

## How host integration works

1. Host enumerates `AVCaptureDevice`, then calls `libsip_core::addVideoDevice(node, devInfo)` per camera.
2. Host opens its own capture session, sets pixel format (NV12 / BGRA), and on each frame calls `getNewFrame(deviceId)` to obtain a `VideoFrame*` from the daemon, copies the pixels into it, and then calls `publishFrame(deviceId)`.
3. Daemon picks up the frame in its encode pipeline.

## Gotchas

- The daemon's `VideoFrame` must have its geometry set via `setFromMemory(...)` or `reserve(...)` before publishing.
- iOS extension hosts (CallKit) run in a separate process and have a different audio session — see `LIBSIP_CORE_FLAG_IOS_EXTENSION`.

## Dependencies

- **Internal**: `media/video/video_device.h`, `sip_core/videomanager_interface.h`.
- **External**: none directly (host owns AVFoundation).

<!-- MANUAL: -->
