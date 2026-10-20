#define LOG_TAG "WaveCamera"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <mutex>

#include <hardware/camera.h>

#include "camera_device.h"
#include "log.h"

namespace wave {
namespace {

struct Device {
    camera_device_t hal;
    CameraDevice* impl;
};

CameraDevice* impl(camera_device* dev) { return reinterpret_cast<Device*>(dev)->impl; }

std::mutex gLock;
CameraDevice* gActive = nullptr;

constexpr camera_info kCameras[] = {
    {CAMERA_FACING_BACK, 90, 0, nullptr},
    {CAMERA_FACING_FRONT, 270, 0, nullptr},
};

int setPreviewWindow(camera_device* d, preview_stream_ops* w)
{
    return impl(d)->setPreviewWindow(w);
}

void setCallbacks(camera_device* d, camera_notify_callback n, camera_data_callback data,
                  camera_data_timestamp_callback ts, camera_request_memory mem, void* user)
{
    impl(d)->setCallbacks(n, data, ts, mem, user);
}

void enableMsgType(camera_device* d, int32_t t) { impl(d)->enableMsgType(t); }
void disableMsgType(camera_device* d, int32_t t) { impl(d)->disableMsgType(t); }
int msgTypeEnabled(camera_device* d, int32_t t) { return impl(d)->msgTypeEnabled(t); }
int startPreview(camera_device* d) { return impl(d)->startPreview(); }
void stopPreview(camera_device* d) { impl(d)->stopPreview(); }
int previewEnabled(camera_device* d) { return impl(d)->previewEnabled(); }
int storeMetaDataInBuffers(camera_device* d, int e) { return impl(d)->storeMetaDataInBuffers(e); }
int startRecording(camera_device* d) { return impl(d)->startRecording(); }
void stopRecording(camera_device* d) { impl(d)->stopRecording(); }
int recordingEnabled(camera_device* d) { return impl(d)->recordingEnabled(); }
void releaseRecordingFrame(camera_device* d, const void* o) { impl(d)->releaseRecordingFrame(o); }
int autoFocus(camera_device* d) { return impl(d)->autoFocus(); }
int cancelAutoFocus(camera_device* d) { return impl(d)->cancelAutoFocus(); }
int takePicture(camera_device* d) { return impl(d)->takePicture(); }
int cancelPicture(camera_device* d) { return impl(d)->cancelPicture(); }
int setParameters(camera_device* d, const char* p) { return impl(d)->setParameters(p); }
char* getParameters(camera_device* d) { return impl(d)->getParameters(); }
void putParameters(camera_device*, char* p) { free(p); }
int sendCommand(camera_device* d, int32_t c, int32_t a1, int32_t a2)
{
    return impl(d)->sendCommand(c, a1, a2);
}
int dump(camera_device* d, int fd) { return impl(d)->dump(fd); }

void release(camera_device* d)
{
    CameraDevice* cam = impl(d);
    ALOGI("release camera %d", cam->id());
    cam->release();
    std::lock_guard l(gLock);
    if (gActive == cam)
        gActive = nullptr;
}

camera_device_ops_t gOps = {
    .set_preview_window = setPreviewWindow,
    .set_callbacks = setCallbacks,
    .enable_msg_type = enableMsgType,
    .disable_msg_type = disableMsgType,
    .msg_type_enabled = msgTypeEnabled,
    .start_preview = startPreview,
    .stop_preview = stopPreview,
    .preview_enabled = previewEnabled,
    .store_meta_data_in_buffers = storeMetaDataInBuffers,
    .start_recording = startRecording,
    .stop_recording = stopRecording,
    .recording_enabled = recordingEnabled,
    .release_recording_frame = releaseRecordingFrame,
    .auto_focus = autoFocus,
    .cancel_auto_focus = cancelAutoFocus,
    .take_picture = takePicture,
    .cancel_picture = cancelPicture,
    .set_parameters = setParameters,
    .get_parameters = getParameters,
    .put_parameters = putParameters,
    .send_command = sendCommand,
    .release = release,
    .dump = dump,
};

int closeDevice(hw_device_t* hw)
{
    auto dev = reinterpret_cast<Device*>(hw);
    ALOGI("close camera %d", dev->impl->id());
    {
        std::lock_guard l(gLock);
        if (gActive == dev->impl)
            gActive = nullptr;
    }
    delete dev->impl;
    delete dev;
    return 0;
}

int openDevice(const hw_module_t* module, const char* name, hw_device_t** out)
{
    char* end;
    long id = strtol(name, &end, 10);
    if (end == name || *end || id < 0 || id >= long(std::size(kCameras))) {
        ALOGE("invalid camera id %s", name);
        return -EINVAL;
    }
    std::lock_guard l(gLock);
    if (gActive) {
        ALOGE("cannot open camera %ld: camera %d is in use", id, gActive->id());
        return -EBUSY;
    }
    auto cam = new CameraDevice(int(id));
    if (!cam->init()) {
        ALOGE("camera %ld failed to initialise", id);
        delete cam;
        return -ENODEV;
    }
    auto dev = new Device{};
    dev->hal.common.tag = HARDWARE_DEVICE_TAG;
    dev->hal.common.version = CAMERA_DEVICE_API_VERSION_1_0;
    dev->hal.common.module = const_cast<hw_module_t*>(module);
    dev->hal.common.close = closeDevice;
    dev->hal.ops = &gOps;
    dev->hal.priv = cam;
    dev->impl = cam;
    gActive = cam;
    *out = &dev->hal.common;
    ALOGI("opened camera %ld", id);
    return 0;
}

int getNumberOfCameras() { return int(std::size(kCameras)); }

int getCameraInfo(int id, camera_info* info)
{
    if (id < 0 || id >= getNumberOfCameras())
        return -EINVAL;
    *info = kCameras[id];
    return 0;
}

hw_module_methods_t gMethods = {
    .open = openDevice,
};

}
}

extern "C" __attribute__((visibility("default"))) camera_module_t HAL_MODULE_INFO_SYM = {
    .common =
        {
            .tag = HARDWARE_MODULE_TAG,
            .module_api_version = CAMERA_MODULE_API_VERSION_1_0,
            .hal_api_version = HARDWARE_HAL_API_VERSION,
            .id = CAMERA_HARDWARE_MODULE_ID,
            .name = "Samsung Wave camera HAL",
            .author = "Badadroid",
            .methods = &wave::gMethods,
        },
    .get_number_of_cameras = wave::getNumberOfCameras,
    .get_camera_info = wave::getCameraInfo,
};
