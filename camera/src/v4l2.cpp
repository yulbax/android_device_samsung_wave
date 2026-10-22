#define LOG_TAG "WaveCamera"

#include "v4l2.h"

#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "log.h"
#include "sec_v4l2.h"

namespace wave {

Mapping Mapping::map(int fd, size_t size, off_t offset)
{
    void* p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
    if (p == MAP_FAILED) {
        ALOGE("mmap(%zu) failed: %s", size, strerror(errno));
        return {};
    }
    return {p, size};
}

void Mapping::reset()
{
    if (addr_)
        ::munmap(addr_, size_);
    addr_ = nullptr;
    size_ = 0;
}

namespace v4l2 {

namespace {

int xioctl(int fd, unsigned long req, void* arg, const char* what)
{
    int ret;
    do {
        ret = ::ioctl(fd, req, arg);
    } while (ret < 0 && errno == EINTR);
    if (ret < 0)
        ALOGE("%s failed: %s", what, strerror(errno));
    return ret;
}

int depth(unsigned fmt)
{
    switch (fmt) {
    case V4L2_PIX_FMT_NV12:
    case v4l2ext::PIX_FMT_NV12T:
    case V4L2_PIX_FMT_NV21:
    case V4L2_PIX_FMT_YUV420:
        return 12;
    case V4L2_PIX_FMT_RGB565:
    case V4L2_PIX_FMT_YUYV:
    case V4L2_PIX_FMT_YVYU:
    case V4L2_PIX_FMT_UYVY:
    case V4L2_PIX_FMT_VYUY:
    case V4L2_PIX_FMT_NV16:
    case V4L2_PIX_FMT_NV61:
    case V4L2_PIX_FMT_YUV422P:
        return 16;
    case V4L2_PIX_FMT_RGB32:
        return 32;
    default:
        return 0;
    }
}

}

int queryCap(int fd)
{
    v4l2_capability cap{};
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap, "VIDIOC_QUERYCAP") < 0)
        return -1;
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        ALOGE("no capture devices");
        return -1;
    }
    return 0;
}

bool enumInput(int fd, int index, char (&name)[32])
{
    v4l2_input input{};
    input.index = index;
    if (xioctl(fd, VIDIOC_ENUMINPUT, &input, "VIDIOC_ENUMINPUT") < 0)
        return false;
    static_assert(sizeof(input.name) == sizeof(name));
    memcpy(name, input.name, sizeof(name));
    name[sizeof(name) - 1] = '\0';
    ALOGI("input %d is %s", index, name);
    return true;
}

int setInput(int fd, int index)
{
    v4l2_input input{};
    input.index = index;
    return xioctl(fd, VIDIOC_S_INPUT, &input, "VIDIOC_S_INPUT");
}

int enumFormat(int fd, unsigned fmt)
{
    v4l2_fmtdesc desc{};
    desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    while (::ioctl(fd, VIDIOC_ENUM_FMT, &desc) == 0) {
        if (desc.pixelformat == fmt)
            return 0;
        desc.index++;
    }
    ALOGE("unsupported pixel format %#x", fmt);
    return -1;
}

int setFormat(int fd, int width, int height, unsigned fmt)
{
    v4l2_format f{};
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    f.fmt.pix.width = width;
    f.fmt.pix.height = height;
    f.fmt.pix.pixelformat = fmt;
    f.fmt.pix.sizeimage = width * height * depth(fmt) / 8;
    f.fmt.pix.field = V4L2_FIELD_NONE;
    return xioctl(fd, VIDIOC_S_FMT, &f, "VIDIOC_S_FMT");
}

int setCaptureFormat(int fd, int width, int height, unsigned fmt)
{
    v4l2_format f{};
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    f.fmt.pix.width = width;
    f.fmt.pix.height = height;
    f.fmt.pix.pixelformat = fmt;
    if (fmt == V4L2_PIX_FMT_JPEG)
        f.fmt.pix.colorspace = V4L2_COLORSPACE_JPEG;
    f.fmt.pix.sizeimage = width * height * depth(fmt) / 8;
    return xioctl(fd, VIDIOC_S_FMT, &f, "VIDIOC_S_FMT (capture)");
}

int requestBuffers(int fd, int count)
{
    v4l2_requestbuffers req{};
    req.count = count;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req, "VIDIOC_REQBUFS") < 0)
        return -1;
    return req.count;
}

Mapping mapBuffer0(int fd)
{
    v4l2_buffer buf{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;
    if (xioctl(fd, VIDIOC_QUERYBUF, &buf, "VIDIOC_QUERYBUF") < 0)
        return {};
    return Mapping::map(fd, buf.length, buf.m.offset);
}

int streamOn(int fd)
{
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    return xioctl(fd, VIDIOC_STREAMON, &type, "VIDIOC_STREAMON");
}

int streamOff(int fd)
{
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    return xioctl(fd, VIDIOC_STREAMOFF, &type, "VIDIOC_STREAMOFF");
}

int queue(int fd, int index)
{
    v4l2_buffer buf{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = index;
    return xioctl(fd, VIDIOC_QBUF, &buf, "VIDIOC_QBUF");
}

int dequeue(int fd)
{
    v4l2_buffer buf{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_DQBUF, &buf, "VIDIOC_DQBUF") < 0)
        return -1;
    return buf.index;
}

std::optional<int> getCtrl(int fd, unsigned id)
{
    v4l2_control ctrl{};
    ctrl.id = id;
    if (::ioctl(fd, VIDIOC_G_CTRL, &ctrl) < 0) {
        ALOGE("VIDIOC_G_CTRL(%u) failed: %s", id - V4L2_CID_PRIVATE_BASE, strerror(errno));
        return std::nullopt;
    }
    return ctrl.value;
}

int setCtrl(int fd, unsigned id, int value)
{
    v4l2_control ctrl{};
    ctrl.id = id;
    ctrl.value = value;
    if (::ioctl(fd, VIDIOC_S_CTRL, &ctrl) < 0) {
        ALOGE("VIDIOC_S_CTRL(%u, %d) failed: %s", id - V4L2_CID_PRIVATE_BASE, value,
              strerror(errno));
        return -1;
    }
    return ctrl.value;
}

int setExtCtrl(int fd, unsigned id, void* value)
{
    v4l2_ext_control ctrl{};
    ctrl.id = id;
    ctrl.string = static_cast<char*>(value);
    v4l2_ext_controls ctrls{};
    ctrls.ctrl_class = V4L2_CTRL_CLASS_CAMERA;
    ctrls.count = 1;
    ctrls.controls = &ctrl;
    return xioctl(fd, VIDIOC_S_EXT_CTRLS, &ctrls, "VIDIOC_S_EXT_CTRLS");
}

int setParm(int fd, v4l2_streamparm& parm)
{
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    return xioctl(fd, VIDIOC_S_PARM, &parm, "VIDIOC_S_PARM");
}

int pollIn(int fd, int timeoutMs)
{
    pollfd p{fd, POLLIN | POLLERR, 0};
    int ret;
    do {
        ret = ::poll(&p, 1, timeoutMs);
    } while (ret < 0 && errno == EINTR);
    if (ret < 0)
        ALOGE("poll failed: %s", strerror(errno));
    return ret;
}

int frameSize(unsigned fmt, int width, int height)
{
    auto align = [](int x, int a) { return (x + a - 1) & ~(a - 1); };
    switch (fmt) {
    case V4L2_PIX_FMT_YUV420:
    case V4L2_PIX_FMT_NV12:
    case V4L2_PIX_FMT_NV21:
        return width * height * 3 / 2;
    case v4l2ext::PIX_FMT_NV12T:
        return align(align(width, 128) * align(height, 32), 8192) +
               align(align(width, 128) * align(height / 2, 32), 8192);
    case V4L2_PIX_FMT_YUV422P:
    case V4L2_PIX_FMT_YUYV:
    case V4L2_PIX_FMT_UYVY:
        return width * height * 2;
    case V4L2_PIX_FMT_RGB565:
        return width * height * 2;
    default:
        ALOGE("frameSize: unknown format %#x", fmt);
        return width * height * 2;
    }
}

}
}
