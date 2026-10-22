#pragma once

#include <linux/videodev2.h>
#include <poll.h>

#include <optional>

#include "unique_fd.h"

namespace wave::v4l2 {

int queryCap(int fd);
bool enumInput(int fd, int index, char (&name)[32]);
int setInput(int fd, int index);
int enumFormat(int fd, unsigned fmt);
int setFormat(int fd, int width, int height, unsigned fmt);
int setCaptureFormat(int fd, int width, int height, unsigned fmt);
int requestBuffers(int fd, int count);
Mapping mapBuffer0(int fd);
int streamOn(int fd);
int streamOff(int fd);
int queue(int fd, int index);
int dequeue(int fd);
std::optional<int> getCtrl(int fd, unsigned id);
int setCtrl(int fd, unsigned id, int value);
int setExtCtrl(int fd, unsigned id, void* value);
int setParm(int fd, v4l2_streamparm& parm);
int pollIn(int fd, int timeoutMs);

int frameSize(unsigned fmt, int width, int height);

}
