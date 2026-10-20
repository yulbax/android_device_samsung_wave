#pragma once

#include <stdint.h>

#include <span>

#include "unique_fd.h"

namespace wave {

class HwJpegEncoder {
public:
    enum Quality { kQualityHigh = 0, kQuality2, kQuality3, kQualityLow };

    bool open();
    std::span<const uint8_t> encodeYuyv(const uint8_t* yuyv, int width, int height,
                                        Quality quality);

    static Quality qualityFor(int percent);

private:
    struct EncParam {
        int sampleMode;
        int encType;
        int inFormat;
        int quality;
        uint32_t width;
        uint32_t height;
        uint32_t dataSize;
        uint32_t fileSize;
        uint32_t setFramebuf;
    };
    struct Args {
        char* inBuf;
        char* phyInBuf;
        int inBufSize;
        char* outBuf;
        char* phyOutBuf;
        int outBufSize;
        char* inThumbBuf;
        char* phyInThumbBuf;
        int inThumbBufSize;
        char* outThumbBuf;
        char* phyOutThumbBuf;
        int outThumbBufSize;
        char* mappedAddr;
        void* decParam;
        EncParam* encParam;
        EncParam* thumbEncParam;
    };
    struct Info {
        uint32_t frameBufSize;
        uint32_t thumbFrameBufSize;
        uint32_t streamBufSize;
        uint32_t thumbStreamBufSize;
        uint32_t totalBufSize;
        int maxWidth;
        int maxHeight;
        int maxThumbWidth;
        int maxThumbHeight;
    };

    uint8_t* bufferAt(unsigned long request, size_t size);

    UniqueFd fd_;
    Mapping map_;
    Info info_{};
    EncParam enc_{};
    EncParam thumb_{};
    Args args_{};
};

}
