#pragma once

#include <hardware/camera.h>
#include <hardware/gralloc.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include "parameters.h"
#include "sensor.h"
#include "unique_fd.h"

namespace wave {

class CameraDevice {
public:
    explicit CameraDevice(int id);
    ~CameraDevice();
    CameraDevice(const CameraDevice&) = delete;
    CameraDevice& operator=(const CameraDevice&) = delete;

    bool init();
    int id() const { return id_; }

    int setPreviewWindow(preview_stream_ops* window);
    void setCallbacks(camera_notify_callback notify, camera_data_callback data,
                      camera_data_timestamp_callback dataTimestamp,
                      camera_request_memory getMemory, void* user);
    void enableMsgType(int32_t type) { msgEnabled_ |= type; }
    void disableMsgType(int32_t type) { msgEnabled_ &= ~type; }
    bool msgTypeEnabled(int32_t type) const { return msgEnabled_ & type; }
    int startPreview();
    void stopPreview();
    bool previewEnabled();
    int storeMetaDataInBuffers(bool enable);
    int startRecording();
    void stopRecording();
    bool recordingEnabled();
    void releaseRecordingFrame(const void* opaque);
    int autoFocus();
    int cancelAutoFocus();
    int takePicture();
    int cancelPicture();
    int setParameters(const char* flat);
    char* getParameters();
    int sendCommand(int32_t cmd, int32_t arg1, int32_t arg2);
    void release();
    int dump(int fd);

private:
    struct MemoryDeleter {
        void operator()(camera_memory_t* m) const { m->release(m); }
    };
    using Memory = std::unique_ptr<camera_memory_t, MemoryDeleter>;
    Memory allocate(size_t size, unsigned count);

    void initDefaultParameters();
    int applyParameters(const Parameters& p);
    int startPreviewLocked();
    void stopPreviewLocked();
    int waitCaptureCompletion();
    void setSkipFrame(int frames);
    void notify(int32_t msg, int32_t ext1, int32_t ext2);

    void previewLoop();
    bool previewFrame();
    void copyToWindow(const uint8_t* frame);
    void previewCallback(const uint8_t* frame, int index);
    void recordFrame(int64_t timestamp);
    void focusLoop();
    void pictureMain();

    const int id_;
    std::unique_ptr<Sensor> sensor_;
    const gralloc_module_t* gralloc_ = nullptr;

    camera_notify_callback notifyCb_ = nullptr;
    camera_data_callback dataCb_ = nullptr;
    camera_data_timestamp_callback dataTimestampCb_ = nullptr;
    camera_request_memory getMemory_ = nullptr;
    void* cookie_ = nullptr;
    std::atomic<int32_t> msgEnabled_{0};

    std::mutex paramsLock_;
    Parameters params_;
    std::vector<Size> supportedPreviewSizes_;

    std::mutex previewLock_;
    std::condition_variable previewCond_;
    std::condition_variable previewIdleCond_;
    preview_stream_ops* window_ = nullptr;
    bool previewRunning_ = false;
    bool previewDeferred_ = false;
    bool previewActive_ = false;
    bool previewIdle_ = false;
    bool exitPreview_ = false;
    std::thread previewThread_;

    Size previewSize_{0, 0};
    int frameSize_ = 0;
    Mapping previewMap_;
    Memory callbackHeap_;
    size_t callbackFrameSize_ = 0;
    bool callbackNv21_ = true;
    std::atomic<bool> callbackNv21Next_{true};
    std::vector<uint8_t> chroma_;

    std::mutex skipLock_;
    int skipFrames_ = 0;

    std::mutex recordLock_;
    bool recordRunning_ = false;
    Memory recordHeap_;

    std::mutex focusLock_;
    std::condition_variable focusCond_;
    bool focusRequested_ = false;
    bool exitFocus_ = false;
    std::thread focusThread_;

    std::mutex captureLock_;
    std::condition_variable captureCond_;
    bool captureInProgress_ = false;
    std::thread pictureThread_;
    Memory rawHeap_;
    size_t rawHeapSize_ = 0;

    bool released_ = false;
};

}
