#define LOG_TAG "WaveCamera"

#include "sensor.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <algorithm>

#include "log.h"
#include "v4l2.h"

namespace wave {

using namespace v4l2ext;

namespace {

constexpr char kPreviewDevice[] = "/dev/video0";
constexpr char kRecordDevice[] = "/dev/video2";
constexpr unsigned kPreviewFormat = V4L2_PIX_FMT_YUV420;
constexpr int kFirstFrameTimeoutMs = 10000;
constexpr int kFrameTimeoutMs = 1000;
constexpr int kAfPolls = 600;
constexpr int kAfPollUs = 10000;
constexpr int kAfProgress = 0x05;
constexpr int kAfSuccess = 0x02;

template <typename F>
class Rollback {
public:
    explicit Rollback(F f) : f_(f) {}
    ~Rollback()
    {
        if (armed_)
            f_();
    }
    void dismiss() { armed_ = false; }

private:
    F f_;
    bool armed_ = true;
};

std::string property(const char* name, const char* fallback)
{
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get(name, value) > 0)
        return value;
    return fallback;
}

int exifOrientationCode(int degrees)
{
    switch (degrees) {
    case 90:
        return 6;
    case 180:
        return 3;
    case 270:
        return 8;
    default:
        return 1;
    }
}

gps_info toDms(long scaled)
{
    long abs = scaled >= 0 ? scaled : -scaled;
    gps_info info{};
    info.sign = scaled < 0;
    info.degree = abs / 10000000;
    long rest = (abs % 10000000) * 60;
    info.minute = rest / 10000000;
    info.second = (rest % 10000000) * 60 / 10000000;
    return info;
}

}

Sensor::~Sensor()
{
    Lock l(lock_);
    stopRecordLocked();
    stopPreviewLocked();
}

bool Sensor::open(int id)
{
    Lock l(lock_);
    params_.capture.timeperframe.numerator = 1;
    params_.capture.timeperframe.denominator = 0;
    params_.contrast = params_.effects = params_.brightness = -1;
    params_.flash_mode = params_.focus_mode = params_.iso = params_.metering = -1;
    params_.saturation = params_.scene_mode = params_.sharpness = -1;
    params_.white_balance = -1;
    chkDataline_ = 0;

    fd_.reset(::open(kPreviewDevice, O_RDWR | O_CLOEXEC));
    if (!fd_) {
        ALOGE("cannot open %s: %s", kPreviewDevice, strerror(errno));
        return false;
    }
    if (v4l2::queryCap(fd_.get()) < 0 || !v4l2::enumInput(fd_.get(), id, name_) ||
        v4l2::setInput(fd_.get(), id) < 0)
        return false;

    fd2_.reset(::open(kRecordDevice, O_RDWR | O_CLOEXEC));
    if (!fd2_) {
        ALOGE("cannot open %s: %s", kRecordDevice, strerror(errno));
        return false;
    }
    char name2[32];
    if (v4l2::queryCap(fd2_.get()) < 0 || !v4l2::enumInput(fd2_.get(), id, name2) ||
        v4l2::setInput(fd2_.get(), id) < 0)
        return false;

    id_ = id;
    if (id_ == kFront) {
        maxPreview_ = {640, 480};
        maxSnapshot_ = {640, 480};
    } else {
        maxPreview_ = {1280, 720};
        maxSnapshot_ = {2560, 1920};
    }
    exifMaker_ = property("ro.product.brand", "SAMSUNG");
    exifModel_ = property("ro.product.model", "GT-S8500");
    exifSoftware_ = property("ro.build.id", "");
    ALOGI("camera %d (%s) opened", id_, name_);
    return true;
}

int Sensor::ctrl(unsigned id, int value)
{
    return v4l2::setCtrl(fd_.get(), id, value);
}

int Sensor::setCached(int& cache, int value, unsigned cid)
{
    if (cache == value)
        return 0;
    cache = value;
    if (previewStarted_ && ctrl(cid, value) < 0)
        return -1;
    return 0;
}

void Sensor::setPreviewSize(Size s)
{
    Lock l(lock_);
    preview_ = s;
}

Size Sensor::previewSize()
{
    Lock l(lock_);
    return preview_;
}

int Sensor::previewFrameSize()
{
    Lock l(lock_);
    return v4l2::frameSize(kPreviewFormat, preview_.width, preview_.height);
}

bool Sensor::previewStarted()
{
    Lock l(lock_);
    return previewStarted_;
}

int Sensor::startPreview()
{
    Lock l(lock_);
    return startPreviewLocked();
}

int Sensor::startPreviewLocked()
{
    if (previewStarted_) {
        ALOGW("preview already started");
        return 0;
    }
    int fd = fd_.get();
    if (fd < 0)
        return -1;

    if (v4l2::enumFormat(fd, kPreviewFormat) < 0)
        return -1;
    int ret = id_ == kBack
                  ? v4l2::setFormat(fd, preview_.width, preview_.height, kPreviewFormat)
                  : v4l2::setFormat(fd, preview_.height, preview_.width, kPreviewFormat);
    if (ret < 0 || v4l2::requestBuffers(fd, kBuffers) < 0)
        return -1;
    if (ctrl(CID_CHECK_DATALINE, chkDataline_) < 0)
        return -1;
    if (id_ == kFront && ctrl(CID_VT_MODE, vtmode_) < 0)
        return -1;
    for (int i = 0; i < kBuffers; i++)
        if (v4l2::queue(fd, i) < 0)
            return -1;

    if (id_ == kBack) {
        antiBanding_ = ANTI_BANDING_50HZ;
        gamma_ = GAMMA_ON;
        slowAe_ = SLOW_AE_ON;
        if (ctrl(CID_ANTI_BANDING, antiBanding_) < 0 || ctrl(CID_ISO, params_.iso) < 0 ||
            ctrl(CID_BRIGHTNESS, params_.brightness) < 0 ||
            ctrl(CID_FRAME_RATE, params_.capture.timeperframe.denominator) < 0 ||
            ctrl(CID_METERING, params_.metering) < 0 || ctrl(CID_SET_GAMMA, gamma_) < 0 ||
            ctrl(CID_SET_SLOW_AE, slowAe_) < 0 || ctrl(CID_EFFECT, params_.effects) < 0 ||
            ctrl(CID_WHITE_BALANCE, params_.white_balance) < 0)
            return -1;
    }

    if (v4l2::streamOn(fd) < 0)
        return -1;
    Rollback rollback([&] {
        v4l2::streamOff(fd);
        previewStarted_ = false;
    });

    if (id_ == kBack) {
        faceDetect_ = FACE_DETECTION_OFF;
        beauty_ = 0;
        zoom_ = zoom_ < 0 ? 0 : zoom_;
        if (ctrl(CID_FOCUS_MODE, params_.focus_mode) < 0 ||
            ctrl(CID_FACE_DETECTION, faceDetect_) < 0 ||
            ctrl(CID_SHARPNESS, params_.sharpness) < 0 ||
            ctrl(CID_SATURATION, params_.saturation) < 0 ||
            ctrl(CID_CONTRAST, params_.contrast) < 0 || ctrl(CID_BEAUTY_SHOT, beauty_) < 0 ||
            ctrl(CID_ZOOM, zoom_) < 0 || ctrl(CID_BATCH_REFLECTION, 1) < 0)
            return -1;
    }
    previewStarted_ = true;

    if (v4l2::setParm(fd, streamparm_) < 0)
        return -1;
    if (id_ == kFront && ctrl(CID_VGA_BLUR, blur_) < 0)
        return -1;
    if (id_ == kBack && params_.flash_mode > FLASH_MODE_BASE &&
        params_.flash_mode < FLASH_MODE_MAX && ctrl(CID_FLASH_MODE, params_.flash_mode) < 0)
        return -1;
    if (v4l2::pollIn(fd, kFirstFrameTimeoutMs) < 0)
        return -1;
    if (ctrl(CID_RETURN_FOCUS, 0) < 0)
        return -1;
    rollback.dismiss();
    return 0;
}

int Sensor::stopPreview()
{
    Lock l(lock_);
    return stopPreviewLocked();
}

int Sensor::stopPreviewLocked()
{
    if (!previewStarted_)
        return 0;
    if (params_.flash_mode == FLASH_MODE_TORCH) {
        params_.flash_mode = FLASH_MODE_OFF;
        ctrl(CID_FLASH_MODE, FLASH_MODE_OFF);
    }
    previewStarted_ = false;
    return v4l2::streamOff(fd_.get());
}

int Sensor::dequeuePreview()
{
    {
        Lock l(lock_);
        if (!previewStarted_)
            return kError;
    }
    int ret = v4l2::pollIn(fd_.get(), kFrameTimeoutMs);
    if (ret == 0) {
        ALOGE("no preview frame in %d ms", kFrameTimeoutMs);
        return kTimeout;
    }
    if (ret < 0)
        return kError;
    Lock l(lock_);
    if (!previewStarted_)
        return kError;
    int index = v4l2::dequeue(fd_.get());
    if (index < 0 || index >= kBuffers) {
        ALOGE("bad preview buffer index %d", index);
        return kError;
    }
    return index;
}

int Sensor::queuePreview(int index)
{
    Lock l(lock_);
    if (!previewStarted_)
        return 0;
    return v4l2::queue(fd_.get(), index);
}

std::optional<Sensor::PhysAddr> Sensor::phys(int fd, int index)
{
    int y = v4l2::setCtrl(fd, CID_PADDR_Y, index);
    int c = v4l2::setCtrl(fd, CID_PADDR_CBCR, index);
    if (y < 0 || c < 0)
        return std::nullopt;
    return PhysAddr{static_cast<uint32_t>(y), static_cast<uint32_t>(c)};
}

std::optional<Sensor::PhysAddr> Sensor::previewPhys(int index)
{
    Lock l(lock_);
    return phys(fd_.get(), index);
}

void Sensor::setRecordingSize(Size s)
{
    Lock l(lock_);
    recording_ = s;
}

int Sensor::startRecord()
{
    Lock l(lock_);
    if (recordStarted_)
        return 0;
    int fd2 = fd2_.get();
    if (fd2 < 0)
        return -1;
    if (v4l2::enumFormat(fd2, PIX_FMT_NV12T) < 0)
        return -1;
    int ret;
    if (id_ == kBack) {
        setCached(params_.iso, ISO_MOVIE, CID_ISO);
        setCached(params_.metering, METERING_MATRIX, CID_METERING);
        if (previewStarted_)
            ctrl(CID_BATCH_REFLECTION, 1);
        ret = v4l2::setFormat(fd2, recording_.width, recording_.height, PIX_FMT_NV12T);
    } else {
        ret = v4l2::setFormat(fd2, recording_.height, recording_.width, PIX_FMT_NV12T);
    }
    if (ret < 0)
        return -1;
    if (ctrl(CID_FRAME_RATE, params_.capture.timeperframe.denominator) < 0)
        return -1;
    if (v4l2::requestBuffers(fd2, kBuffers) < 0)
        return -1;
    for (int i = 0; i < kBuffers; i++)
        if (v4l2::queue(fd2, i) < 0)
            return -1;
    if (v4l2::streamOn(fd2) < 0)
        return -1;
    Rollback rollback([&] { v4l2::streamOff(fd2); });
    if (v4l2::pollIn(fd2, kFirstFrameTimeoutMs) < 0)
        return -1;
    if (id_ == kBack && ctrl(CID_CAF_START_STOP, 1) < 0)
        return -1;
    rollback.dismiss();
    recordStarted_ = true;
    return 0;
}

int Sensor::stopRecord()
{
    Lock l(lock_);
    return stopRecordLocked();
}

int Sensor::stopRecordLocked()
{
    if (!recordStarted_)
        return 0;
    recordStarted_ = false;
    int ret = 0;
    if (id_ == kBack) {
        if (ctrl(CID_CAF_START_STOP, 0) < 0)
            ret = -1;
        int mode = params_.focus_mode;
        setCached(params_.focus_mode, FOCUS_MODE_MACRO, CID_FOCUS_MODE);
        setCached(params_.focus_mode, mode, CID_FOCUS_MODE);
    }
    if (v4l2::streamOff(fd2_.get()) < 0)
        ret = -1;
    if (ctrl(CID_FRAME_RATE, FRAME_RATE_AUTO) < 0)
        ret = -1;
    if (id_ == kBack) {
        setCached(params_.iso, ISO_AUTO, CID_ISO);
        setCached(params_.metering, METERING_CENTER, CID_METERING);
        if (previewStarted_)
            ctrl(CID_BATCH_REFLECTION, 1);
    }
    return ret;
}

int Sensor::dequeueRecord()
{
    {
        Lock l(lock_);
        if (!recordStarted_)
            return kError;
    }
    int ret = v4l2::pollIn(fd2_.get(), kFrameTimeoutMs);
    if (ret <= 0)
        return ret == 0 ? kTimeout : kError;
    Lock l(lock_);
    if (!recordStarted_)
        return kError;
    int index = v4l2::dequeue(fd2_.get());
    if (index < 0 || index >= kBuffers)
        return kError;
    return index;
}

int Sensor::releaseRecordFrame(int index)
{
    Lock l(lock_);
    if (!recordStarted_ || index < 0 || index >= kBuffers)
        return 0;
    return v4l2::queue(fd2_.get(), index);
}

std::optional<Sensor::PhysAddr> Sensor::recordPhys(int index)
{
    Lock l(lock_);
    return phys(fd2_.get(), index);
}

void Sensor::setSnapshotSize(Size s)
{
    Lock l(lock_);
    snapshot_ = s;
}

Size Sensor::snapshotSize()
{
    Lock l(lock_);
    return snapshot_;
}

void Sensor::setSnapshotFormat(unsigned fmt)
{
    Lock l(lock_);
    snapshotFormat_ = fmt;
}

unsigned Sensor::snapshotFormat()
{
    Lock l(lock_);
    return snapshotFormat_;
}

int Sensor::snapshotFrameSize()
{
    Lock l(lock_);
    return v4l2::frameSize(snapshotFormat_, snapshot_.width, snapshot_.height);
}

void Sensor::setJpegThumbnailSize(Size s)
{
    Lock l(lock_);
    jpegThumb_ = s;
}

Size Sensor::jpegThumbnailSize()
{
    Lock l(lock_);
    return jpegThumb_;
}

int Sensor::jpegQuality()
{
    Lock l(lock_);
    return jpegQuality_;
}

int Sensor::prepareCapture(unsigned fmt, int width, int height)
{
    if (previewStarted_) {
        ALOGW("preview still running at capture, stopping it");
        stopPreviewLocked();
    }
    int fd = fd_.get();
    if (fd < 0)
        return -1;
    if (v4l2::enumFormat(fd, snapshotFormat_) < 0 ||
        v4l2::setCaptureFormat(fd, width, height, fmt) < 0 || v4l2::requestBuffers(fd, 1) < 0)
        return -1;
    capture_ = v4l2::mapBuffer0(fd);
    if (!capture_)
        return -1;
    if (v4l2::queue(fd, 0) < 0 || v4l2::streamOn(fd) < 0)
        return -1;
    return 0;
}

int Sensor::startIspCapture()
{
    Lock l(lock_);
    if (prepareCapture(V4L2_PIX_FMT_JPEG, snapshot_.width, snapshot_.height) < 0)
        return -1;
    int fd = fd_.get();
    Rollback rollback([&] { v4l2::streamOff(fd); });
    unsigned long timestamp = gpsTimestamp_;
    time_t now = time(nullptr);
    struct tm local;
    localtime_r(&now, &local);
    if (v4l2::setExtCtrl(fd, CID_GPS_LATITUDE, &gpsLatInfo_) < 0 ||
        v4l2::setExtCtrl(fd, CID_GPS_LONGITUDE, &gpsLonInfo_) < 0 ||
        v4l2::setExtCtrl(fd, CID_GPS_ALTITUDE, &gpsAltInfo_) < 0 ||
        v4l2::setExtCtrl(fd, CID_GPS_PROCESSINGMETHOD, gpsMethod_) < 0 ||
        v4l2::setExtCtrl(fd, CID_GPS_TIMESTAMP, &timestamp) < 0 ||
        v4l2::setExtCtrl(fd, CID_EXIF_TIME_INFO, &local) < 0)
        return -1;
    if (ctrl(CID_EXIF_ORIENTATION, exifOrientationCode(exifOrientation_)) < 0 ||
        ctrl(CID_CAPTURE, 0) < 0)
        return -1;
    rollback.dismiss();
    return 0;
}

std::span<const uint8_t> Sensor::readIspJpeg()
{
    Lock l(lock_);
    int fd = fd_.get();
    Rollback stop([&] { v4l2::streamOff(fd); });
    if (v4l2::pollIn(fd, kFirstFrameTimeoutMs) <= 0) {
        ALOGE("no JPEG from the sensor");
        return {};
    }
    int index = v4l2::dequeue(fd);
    if (index != 0) {
        ALOGE("bad capture buffer index %d", index);
        return {};
    }
    auto size = v4l2::getCtrl(fd, CID_JPEG_MAIN_SIZE);
    auto offset = v4l2::getCtrl(fd, CID_JPEG_MAIN_OFFSET);
    if (!size || !offset || v4l2::setCtrl(fd, CID_STREAM_PAUSE, 0) < 0)
        return {};
    if (*size <= 0 || *offset < 0 || size_t(*offset) + *size > capture_.size()) {
        ALOGE("JPEG %d bytes at %d outside the %zu byte buffer", *size, *offset,
              capture_.size());
        return {};
    }
    return {capture_.data() + *offset, size_t(*size)};
}

int Sensor::captureYuyv(std::vector<uint8_t>& out)
{
    Lock l(lock_);
    if (prepareCapture(snapshotFormat_, snapshot_.height, snapshot_.width) < 0)
        return -1;
    int fd = fd_.get();
    Rollback stop([&] { v4l2::streamOff(fd); });
    if (v4l2::pollIn(fd, kFirstFrameTimeoutMs) <= 0) {
        ALOGE("no frame from the sensor");
        return -1;
    }
    if (v4l2::dequeue(fd) < 0)
        return -1;
    v4l2::setCtrl(fd, CID_STREAM_PAUSE, 0);
    size_t size = size_t(snapshot_.width) * snapshot_.height * 2;
    if (size > capture_.size()) {
        ALOGE("capture buffer %zu < frame %zu", capture_.size(), size);
        return -1;
    }
    out.assign(capture_.data(), capture_.data() + size);
    return 0;
}

void Sensor::endSnapshot()
{
    Lock l(lock_);
    capture_.reset();
}

ExifAttributes Sensor::exifAttributes()
{
    Lock l(lock_);
    ExifAttributes a;
    a.maker = exifMaker_;
    a.model = exifModel_;
    a.software = exifSoftware_;
    a.width = snapshot_.width;
    a.height = snapshot_.height;
    a.orientation = exifOrientationCode(exifOrientation_);

    time_t now = time(nullptr);
    struct tm local;
    localtime_r(&now, &local);
    char date[20];
    strftime(date, sizeof(date), "%Y:%m:%d %H:%M:%S", &local);
    a.dateTime = date;

    int shutter = v4l2::getCtrl(fd_.get(), CID_GET_SHT_TIME).value_or(-1);
    if (shutter <= 0)
        shutter = 100;
    a.exposureTime = {1, uint32_t(1000000 / shutter)};
    static constexpr uint16_t kIso[] = {100, 50, 100, 200, 400, 800, 1600};
    int iso = v4l2::getCtrl(fd_.get(), CID_GET_ISO).value_or(ISO_100);
    a.isoSpeedRating = iso >= ISO_50 && iso <= ISO_1600 ? kIso[iso] : 100;

    a.fnumber = {26, 10};
    auto log2d = [](double x) { return log(x) / log(2.0); };
    int av = int(log2d(2.6) * 2 + 0.5);
    double t = double(a.exposureTime.num) / a.exposureTime.den;
    int tv = t >= 1 ? int(-(log2d(t) + 0.5)) : int(-(log2d(t) - 0.5));
    int sv = int(log2d(a.isoSpeedRating / 3.125) + 0.5);
    a.aperture = {uint32_t(av * 10), 10};
    a.maxAperture = a.aperture;
    a.shutterSpeed = {tv * 10, 10};
    a.brightness = {(av + tv - sv) * 10, 10};
    a.exposureBias = {params_.scene_mode == SCENE_MODE_BEACH_SNOW ? 10 : 0, 10};
    a.focalLength = {uint32_t(id_ == kBack ? 343 : 90), 100};

    switch (params_.metering) {
    case METERING_SPOT:
        a.meteringMode = 3;
        break;
    case METERING_CENTER:
        a.meteringMode = 2;
        break;
    default:
        a.meteringMode = 1;
        break;
    }
    a.flash = uint16_t(std::max(0, v4l2::getCtrl(fd_.get(), CID_GET_FLASH_ONOFF).value_or(0)));
    a.whiteBalance = params_.white_balance == WHITE_BALANCE_AUTO ? 0 : 1;
    switch (params_.scene_mode) {
    case SCENE_MODE_PORTRAIT:
        a.sceneCaptureType = 2;
        break;
    case SCENE_MODE_LANDSCAPE:
        a.sceneCaptureType = 1;
        break;
    case SCENE_MODE_NIGHTSHOT:
        a.sceneCaptureType = 3;
        break;
    default:
        a.sceneCaptureType = 0;
        break;
    }

    if (gpsEnabled_) {
        a.gps = true;
        a.latitudeRef = gpsLatitude_ >= 0 ? 'N' : 'S';
        a.longitudeRef = gpsLongitude_ >= 0 ? 'E' : 'W';
        a.altitudeRef = gpsAltitude_ >= 0 ? 0 : 1;
        a.latitude[0] = {uint32_t(labs(gpsLatitude_)), 10000000};
        a.latitude[1] = a.latitude[2] = {0, 1};
        a.longitude[0] = {uint32_t(labs(gpsLongitude_)), 10000000};
        a.longitude[1] = a.longitude[2] = {0, 1};
        a.altitude = {uint32_t(labs(gpsAltitude_)), 100};
        struct tm utc;
        gmtime_r(&gpsTimestamp_, &utc);
        a.timestamp[0] = {uint32_t(utc.tm_hour), 1};
        a.timestamp[1] = {uint32_t(utc.tm_min), 1};
        a.timestamp[2] = {uint32_t(utc.tm_sec), 1};
        char day[11];
        snprintf(day, sizeof(day), "%04d:%02d:%02d", utc.tm_year + 1900, utc.tm_mon + 1,
                 utc.tm_mday);
        a.datestamp = day;
        a.processingMethod.assign(gpsMethod_, strnlen(gpsMethod_, sizeof(gpsMethod_)));
    }
    a.thumbWidth = jpegThumb_.width;
    a.thumbHeight = jpegThumb_.height;
    return a;
}

int Sensor::startAutofocus()
{
    Lock l(lock_);
    return ctrl(CID_SET_AUTO_FOCUS, AUTO_FOCUS_ON) < 0 ? -1 : 0;
}

int Sensor::autofocusResult()
{
    int result = -1;
    for (int i = 0; i < kAfPolls; i++) {
        {
            Lock l(lock_);
            result = v4l2::getCtrl(fd_.get(), CID_AUTO_FOCUS_RESULT_FIRST).value_or(-1);
        }
        if (result != kAfProgress)
            break;
        usleep(kAfPollUs);
    }
    Lock l(lock_);
    if (ctrl(CID_FINISH_AUTO_FOCUS, 0) < 0)
        return -1;
    return result == kAfSuccess ? 1 : 0;
}

int Sensor::cancelAutofocus()
{
    Lock l(lock_);
    return ctrl(CID_SET_AUTO_FOCUS, AUTO_FOCUS_OFF) < 0 ? -1 : 0;
}

int Sensor::setFrameRate(int fps)
{
    Lock l(lock_);
    if (fps < FRAME_RATE_AUTO || fps > FRAME_RATE_MAX)
        ALOGE("invalid frame rate %d", fps);
    if (params_.capture.timeperframe.denominator == unsigned(fps))
        return 0;
    params_.capture.timeperframe.denominator = fps;
    if (previewStarted_ && ctrl(CID_FRAME_RATE, fps) < 0)
        return -1;
    return 0;
}

#define RANGE_SETTER(name, field, lo, hi, cid)                         \
    int Sensor::name(int v)                                            \
    {                                                                  \
        Lock l(lock_);                                                 \
        if (v < (lo) || v >= (hi)) {                                   \
            ALOGE(#name ": invalid value %d", v);                      \
            return -1;                                                 \
        }                                                              \
        return setCached(field, v, cid);                               \
    }

RANGE_SETTER(setWhiteBalance, params_.white_balance, WHITE_BALANCE_BASE + 1, WHITE_BALANCE_MAX,
             CID_WHITE_BALANCE)
RANGE_SETTER(setImageEffect, params_.effects, IMAGE_EFFECT_BASE + 1, IMAGE_EFFECT_MAX,
             CID_EFFECT)
RANGE_SETTER(setSceneMode, params_.scene_mode, SCENE_MODE_BASE + 1, SCENE_MODE_MAX,
             CID_SCENE_MODE)
RANGE_SETTER(setFlashMode, params_.flash_mode, FLASH_MODE_BASE + 1, FLASH_MODE_MAX,
             CID_FLASH_MODE)
RANGE_SETTER(setISO, params_.iso, ISO_AUTO, ISO_MAX, CID_ISO)
RANGE_SETTER(setContrast, params_.contrast, LEVEL_MINUS_2, LEVEL_MAX, CID_CONTRAST)
RANGE_SETTER(setSaturation, params_.saturation, LEVEL_MINUS_2, LEVEL_MAX, CID_SATURATION)
RANGE_SETTER(setSharpness, params_.sharpness, LEVEL_MINUS_2, LEVEL_MAX, CID_SHARPNESS)
RANGE_SETTER(setWDR, wdr_, WDR_OFF, WDR_MAX, CID_WDR)
RANGE_SETTER(setAntiShake, antiShake_, ANTI_SHAKE_OFF, ANTI_SHAKE_MAX, CID_ANTI_SHAKE)
RANGE_SETTER(setMetering, params_.metering, METERING_BASE + 1, METERING_MAX, CID_METERING)
RANGE_SETTER(setZoom, zoom_, ZOOM_LEVEL_0, ZOOM_LEVEL_MAX, CID_ZOOM)
RANGE_SETTER(setGamma, gamma_, GAMMA_OFF, GAMMA_MAX, CID_SET_GAMMA)
RANGE_SETTER(setSlowAE, slowAe_, GAMMA_OFF, GAMMA_MAX, CID_SET_SLOW_AE)
RANGE_SETTER(setBlur, blur_, BLUR_LEVEL_0, BLUR_LEVEL_MAX, CID_VGA_BLUR)

#undef RANGE_SETTER

int Sensor::sceneMode()
{
    Lock l(lock_);
    return params_.scene_mode;
}

int Sensor::setBrightness(int ev)
{
    Lock l(lock_);
    int v = ev + EV_DEFAULT;
    if (v < EV_MINUS_4 || v > EV_PLUS_4) {
        ALOGE("invalid exposure compensation %d", ev);
        return -1;
    }
    return setCached(params_.brightness, v, CID_BRIGHTNESS);
}

int Sensor::setJpegQuality(int v)
{
    Lock l(lock_);
    if (v < 0 || v > 100) {
        ALOGE("invalid JPEG quality %d", v);
        return -1;
    }
    if (jpegQuality_ == v)
        return 0;
    jpegQuality_ = v;
    if (previewStarted_ && id_ == kBack && ctrl(CID_JPEG_QUALITY, v) < 0)
        return -1;
    return 0;
}

int Sensor::setFocusMode(int v)
{
    Lock l(lock_);
    if (v >= FOCUS_MODE_MAX) {
        ALOGE("invalid focus mode %d", v);
        return -1;
    }
    return setCached(params_.focus_mode, v, CID_FOCUS_MODE);
}

int Sensor::setFaceDetect(int v)
{
    Lock l(lock_);
    if (faceDetect_ == v)
        return 0;
    faceDetect_ = v;
    if (!previewStarted_)
        return 0;
    if (v != FACE_DETECTION_OFF && ctrl(CID_FOCUS_MODE, FOCUS_MODE_AUTO) < 0)
        return -1;
    return ctrl(CID_FACE_DETECTION, v) < 0 ? -1 : 0;
}

int Sensor::setObjectPosition(int x, int y)
{
    Lock l(lock_);
    if (!previewStarted_)
        return 0;
    if (ctrl(CID_OBJECT_POSITION_X, x) < 0 || ctrl(CID_OBJECT_POSITION_Y, y) < 0)
        return -1;
    return 0;
}

int Sensor::setTouchAF(int startStop)
{
    Lock l(lock_);
    if (!previewStarted_)
        return 0;
    return ctrl(CID_TOUCH_AF_START_STOP, startStop) < 0 ? -1 : 0;
}

int Sensor::setExifOrientation(int degrees)
{
    Lock l(lock_);
    if (degrees < 0)
        return -1;
    exifOrientation_ = degrees;
    return 0;
}

int Sensor::setBatchReflection()
{
    Lock l(lock_);
    if (previewStarted_ && ctrl(CID_BATCH_REFLECTION, 1) < 0)
        return -1;
    return 0;
}

int Sensor::setVTmode(int v)
{
    Lock l(lock_);
    if (v < 0 || v > 1)
        return -1;
    vtmode_ = v;
    return 0;
}

int Sensor::setDataLineCheck(int v)
{
    Lock l(lock_);
    if (v < 0 || v > 1)
        return -1;
    chkDataline_ = v;
    return 0;
}

int Sensor::setSensorMode(int v)
{
    Lock l(lock_);
    if (v < 0 || v > 1)
        return -1;
    sensorMode_ = v;
    return 0;
}

int Sensor::setShotMode(int v)
{
    Lock l(lock_);
    if (v < 0 || v > 6)
        return -1;
    shotMode_ = v;
    return 0;
}

void Sensor::setGpsLatitude(const char* s)
{
    Lock l(lock_);
    gpsEnabled_ = s != nullptr;
    if (s)
        gpsLatitude_ = lround(strtod(s, nullptr) * 10000000);
    if (id_ == kBack)
        gpsLatInfo_ = gpsEnabled_ ? toDms(gpsLatitude_) : gps_info{};
}

void Sensor::setGpsLongitude(const char* s)
{
    Lock l(lock_);
    gpsEnabled_ = s != nullptr;
    if (s)
        gpsLongitude_ = lround(strtod(s, nullptr) * 10000000);
    if (id_ == kBack)
        gpsLonInfo_ = gpsEnabled_ ? toDms(gpsLongitude_) : gps_info{};
}

void Sensor::setGpsAltitude(const char* s)
{
    Lock l(lock_);
    gpsAltitude_ = s ? lround(strtod(s, nullptr) * 100) : 0;
    if (id_ == kBack) {
        gpsAltInfo_.sign = gpsAltitude_ >= 0;
        long abs = gpsAltitude_ >= 0 ? gpsAltitude_ : -gpsAltitude_;
        gpsAltInfo_.degree = abs / 100;
        gpsAltInfo_.minute = abs % 100;
        gpsAltInfo_.second = 0;
    }
}

void Sensor::setGpsTimestamp(const char* s)
{
    Lock l(lock_);
    gpsTimestamp_ = s ? atol(s) : 0;
}

void Sensor::setGpsProcessingMethod(const char* s)
{
    Lock l(lock_);
    memset(gpsMethod_, 0, sizeof(gpsMethod_));
    if (s)
        memcpy(gpsMethod_, s, std::min(strlen(s), sizeof(gpsMethod_) - 1));
}

}
