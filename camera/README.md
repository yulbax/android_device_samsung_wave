# camera.wave

The HAL1 camera module of the Samsung Wave (GT-S8500), in C++20. It replaces
`../libcamera` (SecCamera and CameraHardwareSec), keeping its behaviour
towards the CE147 (back) and S5KA3DFX (front) drivers.

## Build

The platform compiler (GCC 4.7) has no C++20, so the module is built with the
NDK and installed as a prebuilt:

    NDK=~/Android/Sdk/ndk/23.2.8568313 ./build.sh

`build.sh` needs a finished `out/target/product/wave` for `libhardware.so`
and writes `prebuilt/camera.wave.so`; `Android.mk` installs that file to
`system/lib/hw`. libc++ is linked statically, so the module only depends on
libc, libm, libdl, liblog and libhardware of Android 4.4.

`WAVE_LEGACY_CAMERA_HAL := true` in the board config builds the old
`libcamera` instead.

## Layout

| File | Contents |
|---|---|
| `module.cpp` | `camera_module_t`, open/close, the `camera_device_ops` entry points |
| `camera_device.*` | preview, recording, autofocus and capture threads, parameters |
| `sensor.*` | the V4L2 sequences of each operation (port of SecCamera) |
| `v4l2.*`, `sec_v4l2.h` | ioctl wrappers and the Samsung control IDs |
| `jpeg_hw.*` | the hardware JPEG encoder (`/dev/s3c-jpg`) for the front camera |
| `exif.*` | the EXIF APP1 segment of front camera pictures |
| `parameters.*` | the `key=value;…` camera parameters |

## Differences from libcamera

- Each `open()` gets its own objects; there is no singleton and no shared
  device. A second open while a camera is in use fails with `-EBUSY`; a
  released camera no longer counts, even if the framework closes it later.
- Threads are `std::thread` with explicit states: a stop waits until the
  preview thread has parked, so the sensor, the window and the buffers are
  never torn down under it.
- A preview frame is copied while its driver buffer is dequeued, and the NV21
  callback is written into its own buffer instead of converting the driver's
  memory in place.
- `yuv420p` preview callbacks are real YV12 (Cr first, 16-aligned strides).
- A sensor that stops delivering frames is restarted once; if that fails the
  app gets `CAMERA_MSG_ERROR` instead of a thread spinning forever.
- A failed capture or autofocus is reported to the app instead of leaving it
  waiting. The front camera, which has fixed focus, answers autofocus at once.
- A failed preview start stops the stream it began (the front camera flash
  crash of 19.10 came from a start that failed half way).
- A new preview window while previewing restarts the preview; a preview size
  change while previewing is refused.
- The JPEG from the CE147 is copied at its real size (the old heap was 0.7 of
  a YUYV frame), and EXIF is built in a growing buffer.
