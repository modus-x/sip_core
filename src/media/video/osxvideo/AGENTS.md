<!-- Parent: ../AGENTS.md -->

# src/media/video/osxvideo/ — macOS camera backend (AVFoundation)

Objective-C++ implementation of camera capture and hot-plug for macOS.

## Files

| File                                | Role                                                                                              |
|-------------------------------------|---------------------------------------------------------------------------------------------------|
| `video_device_impl.mm`              | `VideoDeviceImpl` — wraps `AVCaptureDevice` / `AVCaptureSession`. Opens the device, configures format (resolution + framerate), and pushes frames into the daemon's video pipeline via `AVCaptureVideoDataOutputSampleBufferDelegate`. |
| `video_device_monitor_impl.mm`      | `VideoDeviceMonitorImpl` — observes `AVCaptureDevice` `NSNotificationCenter` notifications (`AVCaptureDeviceWasConnected/Disconnected`) and notifies `Manager` via `emitSignal<VideoSignal::DeviceAdded>`. |

`.mm` = Objective-C++; needed because AVFoundation is Objective-C.

## Gotchas

- **macOS sandbox / TCC**: the host process needs `NSCameraUsageDescription` and `com.apple.security.device.camera`. If permission isn't granted, `AVCaptureSession` will fail to start without a clear error — log `[session canAddInput:]` and `error` returns.
- **Pixel format**: prefer `kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange` (NV12) — matches what VideoToolbox HW encoders accept.
- **Screen capture** for `desktop` is handled by FFmpeg's `avfoundation` device, not here. This subdir is camera-only.

## Dependencies

- **Internal**: `media/video/video_device.h`, `media/video/video_base.h`.
- **External**: AVFoundation, CoreMedia, CoreVideo.

<!-- MANUAL: -->
