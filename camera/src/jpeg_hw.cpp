#define LOG_TAG "WaveCamera"

#include "jpeg_hw.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>

#include "log.h"

namespace wave {

namespace {

constexpr char kDevice[] = "/dev/s3c-jpg";
constexpr char kMagic = 'J';
constexpr unsigned long kEncode = _IO(kMagic, 2);
constexpr unsigned long kGetStrBuf = _IO(kMagic, 3);
constexpr unsigned long kGetFrmBuf = _IO(kMagic, 4);
constexpr unsigned long kGetInfo = _IO(kMagic, 9);

constexpr int kSample422 = 1;
constexpr int kInYCbCr = 1;
constexpr int kEncMain = 0;
constexpr int kJpgSuccess = 1;

constexpr uint32_t kPage = 4096;
constexpr uint32_t pages(uint32_t bytes) { return (bytes / kPage + 1) * kPage; }
constexpr uint32_t kMaxRes = 800 * 480;
constexpr uint32_t kMaxThumbRes = 320 * 240;

}

bool HwJpegEncoder::open()
{
    fd_.reset(::open(kDevice, O_RDWR | O_CLOEXEC));
    if (!fd_) {
        ALOGE("cannot open %s: %s", kDevice, strerror(errno));
        return false;
    }
    if (::ioctl(fd_.get(), kGetInfo, &info_) != 0) {
        ALOGW("JPEG driver info unavailable, using legacy sizes");
        info_.streamBufSize = pages(kMaxRes);
        info_.thumbStreamBufSize = pages(kMaxThumbRes);
        info_.frameBufSize = pages(kMaxRes * 3);
        info_.thumbFrameBufSize = pages(kMaxThumbRes * 3);
        info_.totalBufSize = info_.streamBufSize + info_.thumbStreamBufSize +
                             info_.frameBufSize + info_.thumbFrameBufSize +
                             pages(kMaxRes * 4);
        info_.maxWidth = 800;
        info_.maxHeight = 480;
        info_.maxThumbWidth = 320;
        info_.maxThumbHeight = 240;
    }
    map_ = Mapping::map(fd_.get(), info_.totalBufSize);
    if (!map_)
        return false;
    args_.mappedAddr = reinterpret_cast<char*>(map_.data());
    args_.encParam = &enc_;
    args_.thumbEncParam = &thumb_;
    enc_.encType = kEncMain;
    return true;
}

uint8_t* HwJpegEncoder::bufferAt(unsigned long request, size_t size)
{
    uintptr_t base = reinterpret_cast<uintptr_t>(map_.data());
    uintptr_t addr = static_cast<unsigned>(::ioctl(fd_.get(), request, map_.data()));
    if (addr < base || addr - base + size > map_.size()) {
        ALOGE("JPEG buffer %#lx outside the mapping", request);
        return nullptr;
    }
    return map_.data() + (addr - base);
}

std::span<const uint8_t> HwJpegEncoder::encodeYuyv(const uint8_t* yuyv, int width, int height,
                                                   Quality quality)
{
    if (!map_)
        return {};
    if (width <= 0 || height <= 0 || width > info_.maxWidth || height > info_.maxHeight ||
        width % 16 || height % 8) {
        ALOGE("JPEG encode: unsupported size %dx%d", width, height);
        return {};
    }
    size_t inSize = size_t(width) * height * 2;
    if (inSize > info_.frameBufSize) {
        ALOGE("JPEG encode: frame too large");
        return {};
    }
    uint8_t* in = bufferAt(kGetFrmBuf, inSize);
    if (!in)
        return {};
    memcpy(in, yuyv, inSize);

    enc_.sampleMode = kSample422;
    enc_.encType = kEncMain;
    enc_.inFormat = kInYCbCr;
    enc_.quality = quality;
    enc_.width = width;
    enc_.height = height;
    enc_.fileSize = 0;
    thumb_.sampleMode = kSample422;
    thumb_.inFormat = kInYCbCr;
    args_.inBuf = reinterpret_cast<char*>(in);

    int ret = ::ioctl(fd_.get(), kEncode, &args_);
    if (ret != kJpgSuccess || enc_.fileSize == 0 || enc_.fileSize > info_.streamBufSize) {
        ALOGE("JPEG encode failed (%d, %u bytes)", ret, enc_.fileSize);
        return {};
    }
    uint8_t* out = bufferAt(kGetStrBuf, enc_.fileSize);
    if (!out)
        return {};
    return {out, enc_.fileSize};
}

HwJpegEncoder::Quality HwJpegEncoder::qualityFor(int percent)
{
    if (percent >= 90)
        return kQualityHigh;
    if (percent >= 80)
        return kQuality2;
    if (percent >= 70)
        return kQuality3;
    return kQualityLow;
}

}
