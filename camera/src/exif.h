#pragma once

#include <stdint.h>

#include <span>
#include <string>
#include <vector>

namespace wave {

struct Rational {
    uint32_t num = 0;
    uint32_t den = 1;
};

struct SRational {
    int32_t num = 0;
    int32_t den = 1;
};

struct ExifAttributes {
    std::string maker, model, software;
    std::string dateTime;
    uint32_t width = 0, height = 0;
    uint16_t orientation = 1;
    uint16_t isoSpeedRating = 100;
    uint16_t meteringMode = 0;
    uint16_t flash = 0;
    uint16_t whiteBalance = 0;
    uint16_t sceneCaptureType = 0;
    Rational exposureTime;
    Rational fnumber;
    Rational aperture;
    Rational maxAperture;
    Rational focalLength;
    SRational shutterSpeed;
    SRational brightness;
    SRational exposureBias;

    bool gps = false;
    char latitudeRef = 'N', longitudeRef = 'E';
    uint8_t altitudeRef = 0;
    Rational latitude[3], longitude[3], altitude, timestamp[3];
    std::string datestamp;
    std::string processingMethod;

    uint32_t thumbWidth = 0, thumbHeight = 0;
};

std::vector<uint8_t> makeExifApp1(const ExifAttributes& attr,
                                  std::span<const uint8_t> thumbnailJpeg);

}
