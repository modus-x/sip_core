<!-- Parent: ../AGENTS.md -->

# src/media/video/uwpvideo/ — Windows UWP camera backend

Capture via the UWP `Windows.Media.Capture` API for sandboxed/UWP builds.

## Files

| File                                | Role                                                                                              |
|-------------------------------------|---------------------------------------------------------------------------------------------------|
| `video_device_impl.cpp`             | `VideoDeviceImpl` — uses `MediaCapture` + `MediaFrameReader`. Returns frames via the daemon's frame pump. |
| `video_device_monitor_impl.cpp`     | `VideoDeviceMonitorImpl` — enumerates with `DeviceInformation::FindAllAsync(DeviceClass::VideoCapture)`; hot-plug via `DeviceWatcher`. |
| `CMakeLists.txt`                    | Included when `WINDOWS_STORE` (UWP) is true.                                                      |

## Gotchas

- **App manifest**: the host UWP app must declare `<DeviceCapability Name="webcam" />` and the `Microphone` capability if audio capture is needed in the same app.
- **C++/WinRT** vs **C++/CX**: this code uses C++/WinRT (header-only consumption of WinRT). Some old branches used C++/CX; do not mix.

## Dependencies

- **Internal**: `media/video/video_device.h`.
- **External**: WinRT (header-only), Windows.Media.Capture runtime.

<!-- MANUAL: -->
