#pragma once

#include <linux/videodev2.h>
#include <stdint.h>
#include <time.h>

#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "exif.h"
#include "parameters.h"
#include "sec_v4l2.h"
#include "unique_fd.h"

namespace wave {

class Sensor {
public:
    enum Id { kBack = 0, kFront = 1 };
    static constexpr int kBuffers = 8;

    struct PhysAddr {
        uint32_t y;
        uint32_t cbcr;
    };

    Sensor() = default;
    ~Sensor();
    Sensor(const Sensor&) = delete;
    Sensor& operator=(const Sensor&) = delete;

    bool open(int id);
    int id() const { return id_; }
    const char* name() const { return name_; }

    void setPreviewSize(Size s);
    Size previewSize();
    int previewFrameSize();
    int previewFd() const { return fd_.get(); }
    int startPreview();
    int stopPreview();
    bool previewStarted();
    static constexpr int kTimeout = -2, kError = -1;
    int dequeuePreview();
    int queuePreview(int index);
    std::optional<PhysAddr> previewPhys(int index);

    void setRecordingSize(Size s);
    int startRecord();
    int stopRecord();
    int dequeueRecord();
    int releaseRecordFrame(int index);
    std::optional<PhysAddr> recordPhys(int index);

    int startIspCapture();
    std::span<const uint8_t> readIspJpeg();
    int captureYuyv(std::vector<uint8_t>& out);
    void endSnapshot();

    void setSnapshotSize(Size s);
    Size snapshotSize();
    void setSnapshotFormat(unsigned v4l2Format);
    unsigned snapshotFormat();
    int snapshotFrameSize();
    void setJpegThumbnailSize(Size s);
    Size jpegThumbnailSize();
    int jpegQuality();
    ExifAttributes exifAttributes();

    int startAutofocus();
    int autofocusResult();
    int cancelAutofocus();

    int setFrameRate(int fps);
    int setWhiteBalance(int v);
    int setBrightness(int ev);
    int setImageEffect(int v);
    int setSceneMode(int v);
    int sceneMode();
    int setFlashMode(int v);
    int setISO(int v);
    int setContrast(int v);
    int setSaturation(int v);
    int setSharpness(int v);
    int setWDR(int v);
    int setAntiShake(int v);
    int setMetering(int v);
    int setJpegQuality(int v);
    int setZoom(int v);
    int setFocusMode(int v);
    int setFaceDetect(int v);
    int setObjectPosition(int x, int y);
    int setTouchAF(int startStop);
    int setGamma(int v);
    int setSlowAE(int v);
    int setExifOrientation(int degrees);
    int setBatchReflection();
    int setVTmode(int v);
    int setBlur(int v);
    int setDataLineCheck(int v);
    int setSensorMode(int v);
    int setShotMode(int v);
    void setGpsLatitude(const char* s);
    void setGpsLongitude(const char* s);
    void setGpsAltitude(const char* s);
    void setGpsTimestamp(const char* s);
    void setGpsProcessingMethod(const char* s);

private:
    using Lock = std::lock_guard<std::mutex>;

    int ctrl(unsigned id, int value);
    int setCached(int& cache, int value, unsigned cid);
    int startPreviewLocked();
    int stopPreviewLocked();
    int stopRecordLocked();
    int prepareCapture(unsigned fmt, int width, int height);
    std::optional<PhysAddr> phys(int fd, int index);

    std::mutex lock_;
    int id_ = kBack;
    char name_[32] = {};
    UniqueFd fd_, fd2_;
    bool previewStarted_ = false;
    bool recordStarted_ = false;

    v4l2_streamparm streamparm_{};
    v4l2ext::sec_cam_parm& params_ =
        *reinterpret_cast<v4l2ext::sec_cam_parm*>(streamparm_.parm.raw_data);

    Size preview_{0, 0};
    Size maxPreview_{1280, 720};
    Size snapshot_{0, 0};
    Size maxSnapshot_{2560, 1920};
    unsigned snapshotFormat_ = V4L2_PIX_FMT_YUYV;
    Size recording_{0, 0};
    Size jpegThumb_{0, 0};
    Mapping capture_;

    int antiBanding_ = -1, wdr_ = -1, antiShake_ = -1, zoom_ = -1, beauty_ = -1;
    int faceDetect_ = -1, vtmode_ = 0, sensorMode_ = -1, shotMode_ = -1;
    int exifOrientation_ = -1, blur_ = -1, chkDataline_ = -1, gamma_ = -1, slowAe_ = -1;
    int jpegQuality_ = 100;

    bool gpsEnabled_ = false;
    long gpsLatitude_ = -1, gpsLongitude_ = -1, gpsAltitude_ = -1;
    time_t gpsTimestamp_ = -1;
    char gpsMethod_[100] = {};
    v4l2ext::gps_info gpsLatInfo_{}, gpsLonInfo_{}, gpsAltInfo_{};

    std::string exifMaker_, exifModel_, exifSoftware_;
};

}
