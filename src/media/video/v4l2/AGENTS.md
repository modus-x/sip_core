<!-- Parent: ../AGENTS.md -->

# src/media/video/v4l2/ — Linux camera backend (Video4Linux2)

Standard Linux camera/webcam capture via the `videodev2` ioctl interface.

## Files

| File                                | Role                                                                                              |
|-------------------------------------|---------------------------------------------------------------------------------------------------|
| `video_device_impl.cpp`             | `VideoDeviceImpl` — opens `/dev/videoN`, queries capabilities (`VIDIOC_QUERYCAP`, `VIDIOC_ENUM_FMT`, `VIDIOC_ENUM_FRAMESIZES`, `VIDIOC_ENUM_FRAMEINTERVALS`), uses MMAP buffers (`VIDIOC_REQBUFS`/`VIDIOC_QBUF`/`VIDIOC_DQBUF`). Worker thread `select()`s on the device fd. |
| `video_device_monitor_impl.cpp`     | `VideoDeviceMonitorImpl` — watches udev for `add` / `remove` on `subsystem=video4linux`. Falls back to scanning `/dev/video*` if udev is unavailable. |

## Gotchas

- **Permissions**: the user must be in the `video` group, or the host needs `cap_dac_override`. Failed open returns `EACCES`.
- **Format negotiation**: `VIDIOC_TRY_FMT` then `VIDIOC_S_FMT`. Many webcams report YUYV by default; MJPEG is preferred when available to keep CPU low.
- **V4L2 loopback** devices (e.g. OBS Virtual Camera) are picked up automatically. They sometimes lack frame-rate negotiation.
- **Hot-plug** depends on `libudev` — listed in root `CMakeLists.txt` as `pkg_search_module(udev REQUIRED libudev)`.

## Dependencies

- **Internal**: `media/video/video_device.h`.
- **External**: libudev (system), Linux kernel V4L2 headers.

<!-- MANUAL: -->
