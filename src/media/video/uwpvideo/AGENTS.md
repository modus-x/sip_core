<!-- Parent: ../AGENTS.md -->

# src/media/video/uwpvideo/ — Windows UWP camera backend

Daemon-side stub for UWP (Windows Store) builds. Like the Android and iOS backends, **actual capture happens host-side** (UWP app uses `Windows.Media.Capture`); device capability info is passed into the daemon via `devInfo` vectors at construction time, and frames are pushed via the same `getNewFrame` / `publishFrame` public API.

## Files

| File                                | Role                                                                                              |
|-------------------------------------|---------------------------------------------------------------------------------------------------|
| `video_device_impl.cpp`             | `VideoDeviceImpl` — bookkeeping only. Parses device capability info (`format`, `width`, `height`, `rate`) supplied by the host at construction; no direct WinRT camera calls. Pixel format is mapped to `AV_PIX_FMT_BGRA` via a static `uwp_formats` table. |
| `video_device_monitor_impl.cpp`     | `VideoDeviceMonitorImpl` — stub with an empty `run()`. Hot-plug events must be driven entirely by the host calling `addVideoDevice` / `removeVideoDevice`. |
| `CMakeLists.txt`                    | Included when `WINDOWS_STORE` (UWP) is true.                                                      |

## Gotchas

- **Host owns enumeration**: unlike `winvideo`, the daemon never calls any WinRT camera API directly. The host UWP app must enumerate devices (e.g. via `DeviceInformation::FindAllAsync`) and push the capability list when calling `addVideoDevice`.
- **App manifest**: the host UWP app must declare `<DeviceCapability Name="webcam" />`. The daemon itself has no manifest requirements.
- **Pixel format**: all formats (MJPG, RGB24, NV12, YUY2) are internally remapped to `AV_PIX_FMT_BGRA` — the host must deliver frames in the negotiated format.

## Dependencies

- **Internal**: `media/video/video_device.h`.
- **External**: WinRT (header-only), Windows.Media.Capture runtime.

<!-- MANUAL: -->
