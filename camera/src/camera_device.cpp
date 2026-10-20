#define LOG_TAG "WaveCamera"

#include "camera_device.h"

#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>

#include "exif.h"
#include "jpeg_hw.h"
#include "log.h"
#include "sec_v4l2.h"

namespace wave {

using namespace v4l2ext;

namespace {

constexpr int OK = 0;
constexpr int UNKNOWN_ERROR = INT32_MIN;
constexpr int INVALID_OPERATION = -ENOSYS;
constexpr int BAD_VALUE = -EINVAL;
constexpr int TIMED_OUT = -ETIMEDOUT;

constexpr int kBuffers = Sensor::kBuffers;
constexpr int kInitialSkipFrames = 3;
constexpr int kEffectSkipFrames = 1;
constexpr Size kPostview{640, 480};
constexpr int kMetadataBufferTypeCameraSource = 0;

struct RecordAddrs {
    uint32_t type;
    uint32_t addrY;
    uint32_t addrCbcr;
    uint32_t bufIndex;
    uint32_t reserved;
};

struct AddrsCap {
    uint32_t addrY;
    uint32_t width;
    uint32_t height;
};

constexpr char kBackFocusAuto[] = "0.10,1.20,Infinity";
constexpr char kBackFocusMacro[] = "0.10,0.20,Infinity";
constexpr char kBackFocusInfinity[] = "0.10,1.20,Infinity";
constexpr char kFrontFocus[] = "0.20,0.25,Infinity";
constexpr char kFocusFaceDetect[] = "facedetect";

int64_t monotonicNs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

int align(int x, int a) { return (x + a - 1) & ~(a - 1); }

void scaleDownYuyv(const uint8_t* src, Size s, uint8_t* dst, Size d)
{
    int stepX = s.width / d.width, stepY = s.height / d.height;
    for (int y = 0; y < d.height; y++) {
        const uint8_t* line = src + size_t(y) * stepY * s.width * 2;
        for (int x = 0; x < d.width; x += 2) {
            memcpy(dst, line + size_t(x) * stepX * 2, 4);
            dst += 4;
        }
    }
}

template <size_t N>
const std::pair<std::string_view, int>* findName(const std::pair<std::string_view, int> (&table)[N],
                                                 std::string_view name)
{
    return std::find_if(table, table + N, [&](const auto& e) { return e.first == name; });
}

void setThreadName(const char* name) { pthread_setname_np(pthread_self(), name); }

}

CameraDevice::CameraDevice(int id) : id_(id) {}

CameraDevice::~CameraDevice()
{
    release();
}

bool CameraDevice::init()
{
    sensor_ = std::make_unique<Sensor>();
    if (!sensor_->open(id_))
        return false;
    if (hw_get_module(GRALLOC_HARDWARE_MODULE_ID,
                      reinterpret_cast<const hw_module_t**>(&gralloc_)) != 0) {
        ALOGE("cannot load the gralloc module");
        gralloc_ = nullptr;
    }
    initDefaultParameters();
    previewThread_ = std::thread([this] { previewLoop(); });
    focusThread_ = std::thread([this] { focusLoop(); });
    return true;
}

CameraDevice::Memory CameraDevice::allocate(size_t size, unsigned count)
{
    if (!getMemory_)
        return nullptr;
    camera_memory_t* m = getMemory_(-1, size, count, nullptr);
    if (!m || !m->data) {
        ALOGE("cannot allocate %u x %zu bytes", count, size);
        if (m)
            m->release(m);
        return nullptr;
    }
    return Memory(m);
}

void CameraDevice::notify(int32_t msg, int32_t ext1, int32_t ext2)
{
    if (notifyCb_ && (msgEnabled_ & msg))
        notifyCb_(msg, ext1, ext2, cookie_);
}

void CameraDevice::setCallbacks(camera_notify_callback notify, camera_data_callback data,
                                camera_data_timestamp_callback dataTimestamp,
                                camera_request_memory getMemory, void* user)
{
    notifyCb_ = notify;
    dataCb_ = data;
    dataTimestampCb_ = dataTimestamp;
    getMemory_ = getMemory;
    cookie_ = user;
}

void CameraDevice::initDefaultParameters()
{
    const bool back = id_ == Sensor::kBack;
    Parameters p;
    if (back) {
        p.set("preview-size-values", "1280x720,800x480,720x480,640x480,592x480,352x288");
        p.set("picture-size-values",
              "2560x1920,2560x1536,2048x1536,2048x1232,1600x1200,1600x960,800x480,640x480");
    } else {
        p.set("preview-size-values", "640x480,320x240,176x144");
        p.set("picture-size-values", "640x480");
    }
    supportedPreviewSizes_ = p.sizes("preview-size-values");

    p.set("preview-format", "yuv420sp");
    p.set("preview-format-values", "yuv420sp,yuv420p");
    p.set("video-frame-format", "yuv420p");
    p.setPreviewSize(back ? Size{1280, 720} : Size{640, 480});
    p.set("picture-format", "jpeg");
    p.set("picture-format-values", "jpeg");
    p.setPictureSize(back ? Size{2560, 1920} : Size{640, 480});
    p.set("jpeg-quality", "100");

    if (back) {
        p.set("focus-mode-values", "auto,infinity,macro,facedetect");
        p.set("focus-mode", "auto");
        p.set("focus-distances", kBackFocusAuto);
        p.set("jpeg-thumbnail-size-values", "320x240,0x0");
        p.set("jpeg-thumbnail-width", "320");
        p.set("jpeg-thumbnail-height", "240");
        p.set("preview-frame-rate-values", "30");
        p.set("preview-frame-rate", 30);
    } else {
        p.set("focus-mode-values", "fixed");
        p.set("focus-mode", "fixed");
        p.set("focus-distances", kFrontFocus);
        p.set("jpeg-thumbnail-size-values", "160x120,0x0");
        p.set("jpeg-thumbnail-width", "160");
        p.set("jpeg-thumbnail-height", "120");
        p.set("preview-frame-rate-values", "15");
        p.set("preview-frame-rate", 15);
    }
    p.set("effect-values", "none,mono,negative,sepia");

    if (back) {
        p.set("flash-mode-values", "on,off,auto,torch");
        p.set("flash-mode", "off");
        p.set("scene-mode-values", "auto");
        p.set("scene-mode", "auto");
        p.set("preview-fps-range-values", "(15000,30000)");
        p.set("preview-fps-range", "15000,30000");
        p.set("focal-length", "3.43");
        p.set("max-num-focus-areas", "1");
        p.set("focus-areas", "(0,0,0,0,0)");
        p.set("zoom", "0");
        p.set("max-zoom", "12");
        p.set("zoom-ratios", "100,125,150,175,200,225,250,275,300,325,350,375,400");
        p.set("zoom-supported", "true");
    } else {
        p.set("preview-fps-range-values", "(7500,30000)");
        p.set("preview-fps-range", "7500,30000");
        p.set("focal-length", "0.9");
    }
    p.set("whitebalance-values", "auto,incandescent,fluorescent,daylight,cloudy-daylight");
    p.set("jpeg-thumbnail-quality", "100");
    p.set("rotation", 0);
    p.set("whitebalance", "auto");
    p.set("effect", "none");
    p.set("iso-values", "auto,ISO50,ISO100,ISO200,ISO400,ISO800,ISO1600");
    p.set("iso", "auto");
    p.set("horizontal-view-angle", "51.2");
    p.set("vertical-view-angle", "39.4");
    p.set("exposure-compensation", "0");
    p.set("max-exposure-compensation", "4");
    p.set("min-exposure-compensation", "-4");
    p.set("exposure-compensation-step", "0.5");

    {
        std::lock_guard l(paramsLock_);
        params_ = p;
    }
    applyParameters(p);
    sensor_->setISO(ISO_AUTO);
    sensor_->setMetering(METERING_CENTER);
    sensor_->setContrast(LEVEL_DEFAULT);
    sensor_->setSharpness(LEVEL_DEFAULT);
    sensor_->setSaturation(LEVEL_DEFAULT);
    sensor_->setFrameRate(back ? 30 : 15);
}

int CameraDevice::setParameters(const char* flat)
{
    if (!flat)
        return BAD_VALUE;
    if (waitCaptureCompletion() != OK)
        return TIMED_OUT;
    return applyParameters(Parameters(flat));
}

int CameraDevice::applyParameters(const Parameters& params)
{
    std::lock_guard pl(paramsLock_);
    Sensor& s = *sensor_;
    const bool back = id_ == Sensor::kBack;
    int ret = OK;

    Size preview = params.previewSize();
    const char* previewFormat = params.previewFormat();
    if (!previewFormat ||
        (strcmp(previewFormat, "yuv420sp") && strcmp(previewFormat, "yuv420p"))) {
        ALOGE("unsupported preview format %s", previewFormat ? previewFormat : "(none)");
        return BAD_VALUE;
    }
    if (preview.width > 0 && preview.height > 0 &&
        std::find(supportedPreviewSizes_.begin(), supportedPreviewSizes_.end(), preview) !=
            supportedPreviewSizes_.end()) {
        Size current = s.previewSize();
        const char* currentFormat = params_.previewFormat();
        bool formatChanged = !currentFormat || strcmp(currentFormat, previewFormat);
        if (current != preview || formatChanged) {
            std::lock_guard l(previewLock_);
            if (previewRunning_ && !previewDeferred_) {
                if (current != preview) {
                    ALOGE("preview is running, cannot change its size");
                    ret = INVALID_OPERATION;
                } else {
                    ALOGW("preview is running, format %s applies from the next start",
                          previewFormat);
                    params_.set("preview-format", previewFormat);
                    callbackNv21Next_ = strcmp(previewFormat, "yuv420p") != 0;
                }
            } else {
                s.setPreviewSize(preview);
                if (window_)
                    window_->set_buffers_geometry(window_, preview.width, preview.height,
                                                  HAL_PIXEL_FORMAT_YV12);
                params_.setPreviewSize(preview);
                params_.set("preview-format", previewFormat);
                callbackNv21Next_ = strcmp(previewFormat, "yuv420p") != 0;
            }
        }
    } else {
        ALOGE("invalid preview size %dx%d", preview.width, preview.height);
        ret = INVALID_OPERATION;
    }

    Size picture = params.pictureSize();
    if (picture.width > 0 && picture.height > 0) {
        s.setSnapshotSize(picture);
        params_.setPictureSize(picture);
    }
    if (const char* fmt = params.pictureFormat()) {
        unsigned v4l2;
        std::string_view f(fmt);
        if (f == "rgb565")
            v4l2 = V4L2_PIX_FMT_RGB565;
        else if (f == "rgba8888")
            v4l2 = V4L2_PIX_FMT_RGB32;
        else if (f == "yuv420sp_custom")
            v4l2 = PIX_FMT_NV12T;
        else if (f == "yuv420p")
            v4l2 = V4L2_PIX_FMT_YUV420;
        else if (f == "yuv422i" || f == "jpeg")
            v4l2 = V4L2_PIX_FMT_YUYV;
        else if (f == "uyv422i_custom" || f == "uyv422i")
            v4l2 = V4L2_PIX_FMT_UYVY;
        else if (f == "yuv422p")
            v4l2 = V4L2_PIX_FMT_YUV422P;
        else
            v4l2 = V4L2_PIX_FMT_NV21;
        s.setSnapshotFormat(v4l2);
        params_.set("picture-format", fmt);
    }

    int quality = params.getInt("jpeg-quality");
    if (quality >= 1 && quality <= 100) {
        if (s.setJpegQuality(quality) < 0)
            ret = UNKNOWN_ERROR;
        else
            params_.set("jpeg-quality", quality);
    }

    int thumbW = params.getInt("jpeg-thumbnail-width");
    int thumbH = params.getInt("jpeg-thumbnail-height");
    if (thumbW >= 0 && thumbH >= 0) {
        s.setJpegThumbnailSize({thumbW, thumbH});
        params_.set("jpeg-thumbnail-width", thumbW);
        params_.set("jpeg-thumbnail-height", thumbH);
    }

    if (params.previewFrameRate() != params_.previewFrameRate())
        ALOGW("preview frame rate %d ignored, staying at %d", params.previewFrameRate(),
              params_.previewFrameRate());

    int rotation = params.getInt("rotation");
    if (rotation >= 0) {
        if (s.setExifOrientation(rotation) < 0)
            ret = UNKNOWN_ERROR;
        else
            params_.set("rotation", rotation);
    }

    int ev = params.getInt("exposure-compensation");
    if (params.getInt("min-exposure-compensation") <= ev &&
        ev <= params.getInt("max-exposure-compensation")) {
        if (s.setBrightness(ev) < 0)
            ret = UNKNOWN_ERROR;
        else
            params_.set("exposure-compensation", ev);
    }

    if (const char* iso = params.get("iso")) {
        static constexpr std::pair<std::string_view, int> kIso[] = {
            {"auto", ISO_AUTO}, {"ISO50", ISO_50},   {"ISO100", ISO_100},  {"ISO200", ISO_200},
            {"ISO400", ISO_400}, {"ISO800", ISO_800}, {"ISO1600", ISO_1600},
        };
        auto it = findName(kIso, iso);
        if (it == std::end(kIso)) {
            ALOGE("invalid iso %s", iso);
            ret = UNKNOWN_ERROR;
        } else if (s.setISO(it->second) < 0) {
            ret = UNKNOWN_ERROR;
        } else {
            params_.set("iso", iso);
        }
    }

    if (const char* wb = params.get("whitebalance")) {
        static constexpr std::pair<std::string_view, int> kWb[] = {
            {"auto", WHITE_BALANCE_AUTO},
            {"daylight", WHITE_BALANCE_SUNNY},
            {"cloudy-daylight", WHITE_BALANCE_CLOUDY},
            {"fluorescent", WHITE_BALANCE_FLUORESCENT},
            {"incandescent", WHITE_BALANCE_TUNGSTEN},
        };
        auto it = findName(kWb, wb);
        if (it == std::end(kWb)) {
            ALOGE("invalid white balance %s", wb);
            ret = UNKNOWN_ERROR;
        } else if (s.setWhiteBalance(it->second) < 0) {
            ret = UNKNOWN_ERROR;
        } else {
            params_.set("whitebalance", wb);
        }
    }

    const char* sceneStr = params.get("scene-mode");
    const char* currentScene = params_.get("scene-mode");
    int newMin, newMax, curMin, curMax;
    params.previewFpsRange(newMin, newMax);
    params_.previewFpsRange(curMin, curMax);
    if (sceneStr && currentScene) {
        if (!strcmp(sceneStr, currentScene) && (newMin != curMin || newMax != curMax)) {
            ALOGW("preview fps range %d,%d not allowed, staying at %d,%d", newMin, newMax,
                  curMin, curMax);
            ret = UNKNOWN_ERROR;
        }
    } else if (newMin > newMax || newMin < 0 || newMax < 0) {
        ret = UNKNOWN_ERROR;
    }

    const char* focusStr = params.get("focus-mode");
    if (back) {
        int scene = -1;
        const char* flashStr = params.get("flash-mode");
        params_.set("preview-fps-range-values", "(15000,30000)");
        params_.set("preview-fps-range", "15000,30000");
        std::string_view sc = sceneStr ? sceneStr : "auto";
        if (sc == "auto") {
            scene = SCENE_MODE_NONE;
            params_.set("flash-mode-values", "on,off,auto,torch");
        } else {
            focusStr = "auto";
            flashStr = "off";
            const char* flashModes = "off";
            if (sc == "portrait") {
                scene = SCENE_MODE_PORTRAIT;
                flashStr = flashModes = "auto";
            } else if (sc == "landscape") {
                scene = SCENE_MODE_LANDSCAPE;
            } else if (sc == "sports") {
                scene = SCENE_MODE_SPORTS;
            } else if (sc == "party") {
                scene = SCENE_MODE_PARTY_INDOOR;
                flashStr = flashModes = "auto";
            } else if (sc == "beach" || sc == "snow") {
                scene = SCENE_MODE_BEACH_SNOW;
            } else if (sc == "sunset") {
                scene = SCENE_MODE_SUNSET;
            } else if (sc == "night") {
                scene = SCENE_MODE_NIGHTSHOT;
                params_.set("preview-fps-range-values", "(4000,30000)");
                params_.set("preview-fps-range", "4000,30000");
            } else if (sc == "fireworks") {
                scene = SCENE_MODE_FIREWORKS;
            } else if (sc == "candlelight") {
                scene = SCENE_MODE_CANDLE_LIGHT;
            } else {
                ALOGE("unsupported scene mode %s", sceneStr);
                ret = UNKNOWN_ERROR;
            }
            if (scene >= 0)
                params_.set("flash-mode-values", flashModes);
        }

        if (focusStr) {
            int focus = -1;
            std::string_view f(focusStr);
            if (f == "auto") {
                focus = FOCUS_MODE_AUTO;
                params_.set("focus-distances", kBackFocusAuto);
            } else if (f == "macro") {
                focus = FOCUS_MODE_MACRO;
                params_.set("focus-distances", kBackFocusMacro);
            } else if (f == "infinity") {
                focus = FOCUS_MODE_INFINITY;
                params_.set("focus-distances", kBackFocusInfinity);
            } else if (f == kFocusFaceDetect) {
                if (s.setFaceDetect(FACE_DETECTION_ON) < 0)
                    ret = UNKNOWN_ERROR;
                params_.set("focus-mode", focusStr);
                params_.set("focus-distances", kBackFocusAuto);
            } else {
                ALOGE("unsupported focus mode %s", focusStr);
                ret = UNKNOWN_ERROR;
            }
            if (focus >= 0) {
                if (s.setFaceDetect(FACE_DETECTION_OFF) < 0)
                    ret = UNKNOWN_ERROR;
                if (s.setFocusMode(focus) < 0)
                    ret = UNKNOWN_ERROR;
                else
                    params_.set("focus-mode", focusStr);
            }
        }

        if (flashStr) {
            int flash = -1;
            std::string_view f(flashStr);
            if (f == "off")
                flash = FLASH_MODE_OFF;
            else if (f == "auto")
                flash = FLASH_MODE_AUTO;
            else if (f == "on")
                flash = FLASH_MODE_ON;
            else if (f == "torch")
                flash = FLASH_MODE_TORCH;
            else {
                ALOGE("unsupported flash mode %s", flashStr);
                ret = UNKNOWN_ERROR;
            }
            if (flash >= 0) {
                if (s.setFlashMode(flash) < 0)
                    ret = UNKNOWN_ERROR;
                else
                    params_.set("flash-mode", flashStr);
            }
        }

        if (scene >= 0) {
            if (s.setSceneMode(scene) < 0)
                ret = UNKNOWN_ERROR;
            else
                params_.set("scene-mode", sc);
        }

        if (const char* area = params.get("focus-areas")) {
            int l = 0, t = 0, r = 0, b = 0, w = 0;
            if (sscanf(area, "(%d,%d,%d,%d,%d)", &l, &t, &r, &b, &w) != 5)
                l = t = r = b = w = 0;
            bool dummy = !l && !t && !r && !b;
            if (!dummy) {
                Size ps = s.previewSize();
                int x = ((l + r) / 2 + 1000) * ps.width / 2000;
                int y = ((t + b) / 2 + 1000) * ps.height / 2000;
                if (s.setObjectPosition(x, y) < 0)
                    ret = UNKNOWN_ERROR;
            }
            if (s.setTouchAF(dummy ? 0 : 1) < 0)
                ret = UNKNOWN_ERROR;
        }

        int zoom = params.getInt("zoom");
        if (zoom >= 0 && zoom <= params.getInt("max-zoom")) {
            if (s.setZoom(zoom) < 0)
                ret = UNKNOWN_ERROR;
            else
                params_.set("zoom", zoom);
        }
    } else if (!listContains(params_.get("focus-mode-values"), focusStr)) {
        ALOGE("unsupported focus mode %s", focusStr ? focusStr : "(none)");
        ret = UNKNOWN_ERROR;
    }

    if (const char* effectStr = params.get("effect")) {
        static constexpr std::pair<std::string_view, int> kEffects[] = {
            {"none", IMAGE_EFFECT_NONE},   {"mono", IMAGE_EFFECT_BNW},
            {"sepia", IMAGE_EFFECT_SEPIA}, {"aqua", IMAGE_EFFECT_AQUA},
            {"negative", IMAGE_EFFECT_NEGATIVE},
        };
        auto it = findName(kEffects, effectStr);
        if (it == std::end(kEffects)) {
            ALOGE("invalid effect %s", effectStr);
            ret = UNKNOWN_ERROR;
        } else if (s.setImageEffect(it->second) < 0) {
            ret = UNKNOWN_ERROR;
        } else {
            const char* old = params_.get("effect");
            if (old && strcmp(old, effectStr))
                setSkipFrame(kEffectSkipFrames);
            params_.set("effect", effectStr);
        }
    }

    s.setVTmode(0);
    s.setContrast(LEVEL_DEFAULT);
    s.setWDR(WDR_OFF);
    if (!back)
        s.setBlur(BLUR_LEVEL_0);
    s.setDataLineCheck(0);

    auto gps = [&](const char* key, auto setter) {
        const char* v = params.get(key);
        (s.*setter)(v);
        if (v)
            params_.set(key, v);
        else
            params_.remove(key);
    };
    gps("gps-latitude", &Sensor::setGpsLatitude);
    gps("gps-longitude", &Sensor::setGpsLongitude);
    gps("gps-altitude", &Sensor::setGpsAltitude);
    gps("gps-timestamp", &Sensor::setGpsTimestamp);
    gps("gps-processing-method", &Sensor::setGpsProcessingMethod);

    s.setRecordingSize(preview);
    if (s.setBatchReflection() < 0)
        ret = UNKNOWN_ERROR;
    return ret;
}

char* CameraDevice::getParameters()
{
    std::lock_guard l(paramsLock_);
    return strdup(params_.flatten().c_str());
}

int CameraDevice::sendCommand(int32_t cmd, int32_t arg1, int32_t)
{
    if (cmd == CAMERA_CMD_ENABLE_FOCUS_MOVE_MSG && arg1 == 0)
        return OK;
    return BAD_VALUE;
}

int CameraDevice::setPreviewWindow(preview_stream_ops* w)
{
    std::unique_lock l(previewLock_);
    if (!w) {
        if (previewRunning_ && !previewDeferred_) {
            ALOGI("preview window removed, stopping the preview");
            stopPreviewLocked();
        }
        window_ = nullptr;
        return OK;
    }
    bool restart = previewRunning_;
    if (previewRunning_ && !previewDeferred_) {
        ALOGI("preview window changed, restarting the preview");
        stopPreviewLocked();
    }
    previewRunning_ = previewDeferred_ = false;
    window_ = w;

    int minBufs;
    if (w->get_min_undequeued_buffer_count(w, &minBufs)) {
        ALOGE("cannot get the min undequeued buffer count");
        return INVALID_OPERATION;
    }
    if (minBufs >= kBuffers)
        ALOGE("min undequeued buffer count %d too high", minBufs);
    if (w->set_buffer_count(w, kBuffers)) {
        ALOGE("cannot set the buffer count");
        return INVALID_OPERATION;
    }
    if (w->set_usage(w, GRALLOC_USAGE_SW_WRITE_OFTEN)) {
        ALOGE("cannot set the buffer usage");
        return INVALID_OPERATION;
    }
    Size ps = sensor_->previewSize();
    if (w->set_buffers_geometry(w, ps.width, ps.height, HAL_PIXEL_FORMAT_YV12)) {
        ALOGE("cannot set the buffer geometry");
        return INVALID_OPERATION;
    }

    return restart ? startPreviewLocked() : OK;
}

int CameraDevice::startPreview()
{
    if (waitCaptureCompletion() != OK)
        return TIMED_OUT;
    std::lock_guard l(previewLock_);
    if (previewRunning_) {
        ALOGE("preview already running");
        return INVALID_OPERATION;
    }
    if (!window_) {
        ALOGI("no preview window yet, deferring the preview");
        previewRunning_ = true;
        previewDeferred_ = true;
        return OK;
    }
    return startPreviewLocked();
}

int CameraDevice::startPreviewLocked()
{
    if (sensor_->startPreview() < 0) {
        ALOGE("sensor preview start failed");
        sensor_->stopPreview();
        return UNKNOWN_ERROR;
    }
    setSkipFrame(kInitialSkipFrames);

    previewSize_ = sensor_->previewSize();
    frameSize_ = sensor_->previewFrameSize();
    previewMap_ = Mapping::map(sensor_->previewFd(), size_t(frameSize_) * kBuffers);
    callbackNv21_ = callbackNv21Next_;
    const int w = previewSize_.width, h = previewSize_.height;
    callbackFrameSize_ = callbackNv21_
                             ? size_t(w) * h * 3 / 2
                             : size_t(align(w, 16)) * h + size_t(align(w / 2, 16)) * h;
    callbackHeap_ = allocate(callbackFrameSize_, kBuffers);
    chroma_.resize(size_t(w) * h / 2);
    if (!previewMap_ || !callbackHeap_) {
        sensor_->stopPreview();
        previewMap_.reset();
        callbackHeap_.reset();
        return UNKNOWN_ERROR;
    }

    previewRunning_ = true;
    previewDeferred_ = false;
    previewActive_ = true;
    previewCond_.notify_all();
    return OK;
}

void CameraDevice::stopPreviewLocked()
{
    std::unique_lock l(previewLock_, std::adopt_lock);
    if (previewActive_) {
        previewActive_ = false;
        previewCond_.notify_all();
        previewIdleCond_.wait(l, [this] { return previewIdle_; });
    }
    l.release();
    if (previewRunning_ && !previewDeferred_) {
        sensor_->stopPreview();
        previewMap_.reset();
        callbackHeap_.reset();
    }
    previewRunning_ = false;
    previewDeferred_ = false;
}

void CameraDevice::stopPreview()
{
    std::lock_guard l(previewLock_);
    stopPreviewLocked();
}

bool CameraDevice::previewEnabled()
{
    std::lock_guard l(previewLock_);
    return previewRunning_;
}

void CameraDevice::setSkipFrame(int frames)
{
    std::lock_guard l(skipLock_);
    skipFrames_ = std::max(skipFrames_, frames);
}

void CameraDevice::previewLoop()
{
    setThreadName("CameraPreview");
    setpriority(PRIO_PROCESS, 0, -4);
    std::unique_lock l(previewLock_);
    while (true) {
        previewIdle_ = true;
        previewIdleCond_.notify_all();
        previewCond_.wait(l, [this] { return previewActive_ || exitPreview_; });
        if (exitPreview_)
            return;
        previewIdle_ = false;
        while (previewActive_ && !exitPreview_) {
            l.unlock();
            bool ok = previewFrame();
            l.lock();
            if (!ok && previewActive_) {
                previewActive_ = false;
                l.unlock();
                notify(CAMERA_MSG_ERROR, CAMERA_ERROR_UNKNOWN, 0);
                l.lock();
            }
        }
    }
}

bool CameraDevice::previewFrame()
{
    int index = sensor_->dequeuePreview();
    if (index == Sensor::kTimeout) {
        ALOGE("preview stalled, restarting the sensor");
        sensor_->stopPreview();
        if (sensor_->startPreview() < 0) {
            sensor_->stopPreview();
            return false;
        }
        setSkipFrame(kInitialSkipFrames);
        return true;
    }
    if (index < 0) {
        usleep(10000);
        return true;
    }

    int64_t timestamp = monotonicNs();
    bool skip;
    {
        std::lock_guard l(skipLock_);
        skip = skipFrames_ > 0;
        if (skip)
            skipFrames_--;
    }
    if (!skip) {
        const uint8_t* frame = previewMap_.data() + size_t(frameSize_) * index;
        copyToWindow(frame);
        if (msgEnabled_ & CAMERA_MSG_PREVIEW_FRAME)
            previewCallback(frame, index);
    }
    sensor_->queuePreview(index);
    if (!skip)
        recordFrame(timestamp);
    return true;
}

void CameraDevice::copyToWindow(const uint8_t* frame)
{
    preview_stream_ops* w = window_;
    if (!w || !gralloc_)
        return;
    buffer_handle_t* handle;
    int stride;
    if (w->dequeue_buffer(w, &handle, &stride)) {
        ALOGE("cannot dequeue a preview buffer");
        return;
    }
    const int width = previewSize_.width, height = previewSize_.height;
    void* vaddr;
    if (gralloc_->lock(gralloc_, *handle, GRALLOC_USAGE_SW_WRITE_OFTEN, 0, 0, width, height,
                       &vaddr)) {
        ALOGE("cannot lock a preview buffer");
        w->cancel_buffer(w, handle);
        return;
    }
    auto dst = static_cast<uint8_t*>(vaddr);
    const uint8_t* src = frame;
    for (int y = 0; y < height; y++, dst += stride, src += width)
        memcpy(dst, src, width);
    const int cstride = align(stride / 2, 16);
    uint8_t* cr = static_cast<uint8_t*>(vaddr) + size_t(stride) * height;
    uint8_t* cb = cr + size_t(cstride) * height / 2;
    const int cw = width / 2;
    for (int y = 0; y < height / 2; y++, cb += cstride, src += cw)
        memcpy(cb, src, cw);
    for (int y = 0; y < height / 2; y++, cr += cstride, src += cw)
        memcpy(cr, src, cw);
    gralloc_->unlock(gralloc_, *handle);
    if (w->enqueue_buffer(w, handle))
        ALOGE("cannot enqueue a preview buffer");
}

void CameraDevice::previewCallback(const uint8_t* frame, int index)
{
    if (!dataCb_ || !callbackHeap_)
        return;
    const int w = previewSize_.width, h = previewSize_.height;
    auto out = static_cast<uint8_t*>(callbackHeap_->data) + callbackFrameSize_ * index;
    const uint8_t* u = frame + size_t(w) * h;
    const uint8_t* v = u + size_t(w) * h / 4;
    if (callbackNv21_) {
        memcpy(out, frame, size_t(w) * h);
        memcpy(chroma_.data(), u, size_t(w) * h / 2);
        const uint8_t* cu = chroma_.data();
        const uint8_t* cv = cu + size_t(w) * h / 4;
        uint8_t* vu = out + size_t(w) * h;
        for (size_t i = 0, n = size_t(w) * h / 4; i < n; i++) {
            *vu++ = cv[i];
            *vu++ = cu[i];
        }
    } else {
        const int ys = align(w, 16), cs = align(w / 2, 16);
        uint8_t* dst = out;
        for (int y = 0; y < h; y++, dst += ys)
            memcpy(dst, frame + size_t(y) * w, w);
        uint8_t* cr = out + size_t(ys) * h;
        uint8_t* cb = cr + size_t(cs) * h / 2;
        for (int y = 0; y < h / 2; y++) {
            memcpy(cr + size_t(y) * cs, v + size_t(y) * w / 2, w / 2);
            memcpy(cb + size_t(y) * cs, u + size_t(y) * w / 2, w / 2);
        }
    }
    dataCb_(CAMERA_MSG_PREVIEW_FRAME, callbackHeap_.get(), index, nullptr, cookie_);
}

int CameraDevice::storeMetaDataInBuffers(bool enable)
{
    if (!enable) {
        ALOGE("only metadata buffers are supported for recording");
        return INVALID_OPERATION;
    }
    return OK;
}

int CameraDevice::startRecording()
{
    std::lock_guard l(recordLock_);
    if (recordRunning_)
        return OK;
    if (!recordHeap_)
        recordHeap_ = allocate(sizeof(RecordAddrs), kBuffers);
    if (!recordHeap_)
        return UNKNOWN_ERROR;
    if (sensor_->startRecord() < 0) {
        ALOGE("sensor recording start failed");
        return UNKNOWN_ERROR;
    }
    recordRunning_ = true;
    return OK;
}

void CameraDevice::stopRecording()
{
    std::lock_guard l(recordLock_);
    if (!recordRunning_)
        return;
    recordRunning_ = false;
    if (sensor_->stopRecord() < 0)
        ALOGE("sensor recording stop failed");
}

bool CameraDevice::recordingEnabled()
{
    std::lock_guard l(recordLock_);
    return recordRunning_;
}

void CameraDevice::recordFrame(int64_t timestamp)
{
    std::lock_guard l(recordLock_);
    if (!recordRunning_)
        return;
    int index = sensor_->dequeueRecord();
    if (index < 0) {
        ALOGE("no recording frame");
        return;
    }
    auto phys = sensor_->recordPhys(index);
    if (!phys) {
        sensor_->releaseRecordFrame(index);
        return;
    }
    auto addrs = static_cast<RecordAddrs*>(recordHeap_->data);
    addrs[index] = {kMetadataBufferTypeCameraSource, phys->y, phys->cbcr, uint32_t(index), 0};
    if ((msgEnabled_ & CAMERA_MSG_VIDEO_FRAME) && dataTimestampCb_)
        dataTimestampCb_(timestamp, CAMERA_MSG_VIDEO_FRAME, recordHeap_.get(), index, cookie_);
    else
        sensor_->releaseRecordFrame(index);
}

void CameraDevice::releaseRecordingFrame(const void* opaque)
{
    if (!opaque)
        return;
    auto addrs = static_cast<const RecordAddrs*>(opaque);
    sensor_->releaseRecordFrame(int(addrs->bufIndex));
}

int CameraDevice::autoFocus()
{
    std::lock_guard l(focusLock_);
    focusRequested_ = true;
    focusCond_.notify_all();
    return OK;
}

int CameraDevice::cancelAutoFocus()
{
    {
        std::lock_guard l(previewLock_);
        if (!previewRunning_ || previewDeferred_)
            return OK;
    }
    if (id_ != Sensor::kBack)
        return OK;
    if (sensor_->cancelAutofocus() < 0)
        ALOGW("cancelling autofocus failed");
    return OK;
}

void CameraDevice::focusLoop()
{
    setThreadName("CameraFocus");
    std::unique_lock l(focusLock_);
    while (true) {
        focusCond_.wait(l, [this] { return focusRequested_ || exitFocus_; });
        if (exitFocus_)
            return;
        focusRequested_ = false;
        l.unlock();
        bool focused;
        if (id_ != Sensor::kBack) {
            focused = true;
        } else if (sensor_->startAutofocus() < 0) {
            ALOGE("autofocus start failed");
            focused = false;
        } else {
            focused = sensor_->autofocusResult() == 1;
        }
        notify(CAMERA_MSG_FOCUS, focused, 0);
        l.lock();
    }
}

int CameraDevice::waitCaptureCompletion()
{
    std::unique_lock l(captureLock_);
    if (!captureCond_.wait_for(l, std::chrono::seconds(5),
                               [this] { return !captureInProgress_; })) {
        ALOGE("timed out waiting for the picture thread");
        return TIMED_OUT;
    }
    return OK;
}

int CameraDevice::takePicture()
{
    stopPreview();
    if (!rawHeap_) {
        rawHeapSize_ = size_t(kPostview.width) * kPostview.height * 2;
        rawHeap_ = allocate(rawHeapSize_, 1);
    }
    if (waitCaptureCompletion() != OK)
        return TIMED_OUT;
    if (pictureThread_.joinable())
        pictureThread_.join();
    {
        std::lock_guard l(captureLock_);
        captureInProgress_ = true;
    }
    pictureThread_ = std::thread([this] { pictureMain(); });
    return OK;
}

int CameraDevice::cancelPicture()
{
    if (pictureThread_.joinable())
        pictureThread_.join();
    return OK;
}

void CameraDevice::pictureMain()
{
    setThreadName("CameraPicture");
    Sensor& s = *sensor_;
    const bool back = id_ == Sensor::kBack;
    std::vector<uint8_t> postview(size_t(kPostview.width) * kPostview.height * 2, 0);
    std::vector<uint8_t> jpeg;
    bool ok = false;

    if (back) {
        if (s.startIspCapture() < 0) {
            ALOGE("capture start failed");
        } else {
            notify(CAMERA_MSG_SHUTTER, 0, 0);
            auto data = s.readIspJpeg();
            if (data.empty())
                ALOGE("no JPEG from the sensor");
            else {
                jpeg.assign(data.begin(), data.end());
                ok = true;
            }
        }
    } else {
        notify(CAMERA_MSG_SHUTTER, 0, 0);
        std::vector<uint8_t> yuyv;
        Size size = s.snapshotSize();
        HwJpegEncoder encoder;
        if (s.captureYuyv(yuyv) < 0) {
            ALOGE("capture failed");
        } else if (!encoder.open()) {
            ALOGE("JPEG encoder unavailable");
        } else {
            memcpy(postview.data(), yuyv.data(), std::min(postview.size(), yuyv.size()));
            auto main = encoder.encodeYuyv(yuyv.data(), size.width, size.height,
                                           HwJpegEncoder::qualityFor(s.jpegQuality()));
            if (main.size() > 2) {
                std::vector<uint8_t> mainJpeg(main.begin(), main.end());
                ExifAttributes attr = s.exifAttributes();
                std::vector<uint8_t> thumbJpeg;
                Size thumbReq = s.jpegThumbnailSize();
                constexpr Size kThumb{160, 120};
                if (thumbReq.width > 0 && thumbReq.height > 0) {
                    std::vector<uint8_t> thumb(size_t(kThumb.width) * kThumb.height * 2);
                    scaleDownYuyv(postview.data(), kPostview, thumb.data(), kThumb);
                    auto t = encoder.encodeYuyv(thumb.data(), kThumb.width, kThumb.height,
                                                HwJpegEncoder::kQuality2);
                    thumbJpeg.assign(t.begin(), t.end());
                    attr.thumbWidth = kThumb.width;
                    attr.thumbHeight = kThumb.height;
                }
                auto app1 = makeExifApp1(attr, thumbJpeg);
                jpeg.reserve(mainJpeg.size() + app1.size());
                jpeg.insert(jpeg.end(), mainJpeg.begin(), mainJpeg.begin() + 2);
                jpeg.insert(jpeg.end(), app1.begin(), app1.end());
                jpeg.insert(jpeg.end(), mainJpeg.begin() + 2, mainJpeg.end());
                ok = true;
            }
        }
    }
    s.endSnapshot();

    if (ok) {
        if (rawHeap_) {
            memcpy(rawHeap_->data, postview.data(), std::min(rawHeapSize_, postview.size()));
            static_assert(sizeof(AddrsCap) <= size_t(kPostview.width) * kPostview.height * 2);
        }
        if ((msgEnabled_ & CAMERA_MSG_RAW_IMAGE) && rawHeap_ && dataCb_)
            dataCb_(CAMERA_MSG_RAW_IMAGE, rawHeap_.get(), 0, nullptr, cookie_);
        else
            notify(CAMERA_MSG_RAW_IMAGE_NOTIFY, 0, 0);
        if ((msgEnabled_ & CAMERA_MSG_COMPRESSED_IMAGE) && dataCb_) {
            if (Memory mem = allocate(jpeg.size(), 1)) {
                memcpy(mem->data, jpeg.data(), jpeg.size());
                dataCb_(CAMERA_MSG_COMPRESSED_IMAGE, mem.get(), 0, nullptr, cookie_);
            }
        }
    } else {
        notify(CAMERA_MSG_ERROR, CAMERA_ERROR_UNKNOWN, 0);
    }

    std::lock_guard l(captureLock_);
    captureInProgress_ = false;
    captureCond_.notify_all();
}

void CameraDevice::release()
{
    if (released_)
        return;
    released_ = true;

    if (previewThread_.joinable()) {
        {
            std::lock_guard l(previewLock_);
            stopPreviewLocked();
            exitPreview_ = true;
            previewCond_.notify_all();
        }
        previewThread_.join();
    }
    if (focusThread_.joinable()) {
        {
            std::lock_guard l(focusLock_);
            exitFocus_ = true;
            focusCond_.notify_all();
        }
        focusThread_.join();
    }
    if (pictureThread_.joinable())
        pictureThread_.join();
    stopRecording();

    rawHeap_.reset();
    callbackHeap_.reset();
    recordHeap_.reset();
    previewMap_.reset();
    sensor_.reset();
}

int CameraDevice::dump(int fd)
{
    char buf[256];
    bool running;
    {
        std::lock_guard l(previewLock_);
        running = previewRunning_;
    }
    int n = snprintf(buf, sizeof(buf), "camera %d: preview %s, %dx%d, messages %#x\n", id_,
                     running ? "running" : "stopped", previewSize_.width, previewSize_.height,
                     int(msgEnabled_));
    if (n > 0)
        write(fd, buf, std::min<size_t>(n, sizeof(buf) - 1));
    std::string flat;
    {
        std::lock_guard l(paramsLock_);
        flat = params_.flatten();
    }
    write(fd, flat.data(), flat.size());
    write(fd, "\n", 1);
    return OK;
}

}
