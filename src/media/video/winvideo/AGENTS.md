<!-- Parent: ../AGENTS.md -->

# src/media/video/winvideo/ — Windows camera backend (DirectShow)

Capture via DirectShow on classic Win32 (non-UWP). For UWP, see [`../uwpvideo/`](../uwpvideo/AGENTS.md).

## Files

| File                                | Role                                                                                              |
|-------------------------------------|---------------------------------------------------------------------------------------------------|
| `video_device_impl.cpp`             | `VideoDeviceImpl` — builds a DirectShow filter graph: `ICaptureGraphBuilder2` → `IBaseFilter` (camera) → `ISampleGrabber` callback → `IBaseFilter` (null renderer). The grabber callback delivers frames into the daemon. |
| `video_device_monitor_impl.cpp`     | `VideoDeviceMonitorImpl` — enumerates devices via `ICreateDevEnum`; hot-plug listens for `WM_DEVICECHANGE` via a hidden window. |
| `capture_graph_interfaces.h`        | COM interface forward declarations / GUIDs used by the device impl (helps avoid pulling the full `<dshow.h>` everywhere). |
| `CMakeLists.txt`                    | Includes this subset when `WIN32 AND NOT WINDOWS_STORE`.                                          |

## Gotchas

- **DirectShow is deprecated** by Microsoft but still works on Win10/Win11. Media Foundation would be the modern replacement; not used here yet.
- **COM apartments**: all DirectShow calls must come from a COM-initialized thread (`CoInitializeEx(... COINIT_APARTMENTTHREADED)`). The device thread does this.
- **YUY2 is the safest pixel format**; some cameras only report MJPEG and require a decoder hop.

## Dependencies

- **Internal**: `media/video/video_device.h`.
- **External**: DirectShow headers (`dshow.h`, `strmif.h`); `strmiids.lib`, `ole32.lib`, `oleaut32.lib` are standard Windows SDK libs implicitly available — they are **not** explicitly listed in `winvideo/CMakeLists.txt`.

<!-- MANUAL: -->
