#include "exif.h"

#include <string.h>

#include <algorithm>

namespace wave {

namespace {

enum Type : uint16_t { BYTE = 1, ASCII = 2, SHORT = 3, LONG = 4, RATIONAL = 5, UNDEFINED = 7,
                       SRATIONAL = 10 };

class TiffWriter {
public:
    std::vector<uint8_t>& out() { return out_; }
    size_t pos() const { return out_.size(); }

    void u16(uint16_t v) { put(&v, 2); }
    void u32(uint32_t v) { put(&v, 4); }
    void put(const void* p, size_t n)
    {
        auto b = static_cast<const uint8_t*>(p);
        out_.insert(out_.end(), b, b + n);
    }
    void patch32(size_t at, uint32_t v) { memcpy(&out_[at], &v, 4); }

private:
    std::vector<uint8_t> out_;
};

struct Entry {
    uint16_t tag;
    Type type;
    uint32_t count;
    std::vector<uint8_t> value;
};

Entry shortEntry(uint16_t tag, uint16_t v)
{
    Entry e{tag, SHORT, 1, {}};
    e.value.resize(2);
    memcpy(e.value.data(), &v, 2);
    return e;
}

Entry longEntry(uint16_t tag, uint32_t v)
{
    Entry e{tag, LONG, 1, {}};
    e.value.resize(4);
    memcpy(e.value.data(), &v, 4);
    return e;
}

Entry asciiEntry(uint16_t tag, const std::string& s)
{
    Entry e{tag, ASCII, static_cast<uint32_t>(s.size() + 1), {}};
    e.value.assign(s.begin(), s.end());
    e.value.push_back(0);
    return e;
}

Entry bytesEntry(uint16_t tag, Type type, const void* p, size_t n)
{
    Entry e{tag, type, static_cast<uint32_t>(n), {}};
    auto b = static_cast<const uint8_t*>(p);
    e.value.assign(b, b + n);
    return e;
}

template <typename R>
Entry rationalEntry(uint16_t tag, Type type, const R* r, uint32_t count)
{
    Entry e{tag, type, count, {}};
    for (uint32_t i = 0; i < count; i++) {
        uint8_t b[8];
        memcpy(b, &r[i].num, 4);
        memcpy(b + 4, &r[i].den, 4);
        e.value.insert(e.value.end(), b, b + 8);
    }
    return e;
}

size_t writeIfd(TiffWriter& w, std::vector<Entry>& entries)
{
    w.u16(static_cast<uint16_t>(entries.size()));
    size_t entriesAt = w.pos();
    size_t nextAt = entriesAt + entries.size() * 12;
    uint32_t data = nextAt + 4;
    for (auto& e : entries) {
        w.u16(e.tag);
        w.u16(e.type);
        w.u32(e.count);
        if (e.value.size() <= 4) {
            uint8_t inl[4] = {};
            memcpy(inl, e.value.data(), e.value.size());
            w.put(inl, 4);
        } else {
            w.u32(data);
            data += (e.value.size() + 1) & ~size_t(1);
        }
    }
    w.u32(0);
    for (auto& e : entries) {
        if (e.value.size() > 4) {
            w.put(e.value.data(), e.value.size());
            if (e.value.size() & 1)
                w.put("", 1);
        }
    }
    return nextAt;
}

}

std::vector<uint8_t> makeExifApp1(const ExifAttributes& a, std::span<const uint8_t> thumb)
{
    constexpr uint16_t kExifIfdPointer = 0x8769, kGpsIfdPointer = 0x8825;

    TiffWriter w;
    const uint8_t header[8] = {'I', 'I', 0x2a, 0x00, 0x08, 0x00, 0x00, 0x00};
    w.put(header, sizeof(header));

    std::vector<Entry> ifd0 = {
        longEntry(0x0100, a.width),
        longEntry(0x0101, a.height),
        asciiEntry(0x010f, a.maker),
        asciiEntry(0x0110, a.model),
        shortEntry(0x0112, a.orientation),
        asciiEntry(0x0131, a.software),
        asciiEntry(0x0132, a.dateTime),
        shortEntry(0x0213, 1),
        longEntry(kExifIfdPointer, 0),
    };
    if (a.gps)
        ifd0.push_back(longEntry(kGpsIfdPointer, 0));
    size_t ifd0At = w.pos();
    size_t ifd0Next = writeIfd(w, ifd0);
    auto pointerField = [&](size_t index) { return ifd0At + 2 + index * 12 + 8; };

    static const char kVersion[4] = {'0', '2', '2', '0'};
    static const char kAsciiPrefix[8] = {'A', 'S', 'C', 'I', 'I', 0, 0, 0};
    const char comment[] = "User comments";
    std::vector<uint8_t> userComment(kAsciiPrefix, kAsciiPrefix + 8);
    userComment.insert(userComment.end(), comment, comment + sizeof(comment));
    std::vector<Entry> exif = {
        rationalEntry(0x829a, RATIONAL, &a.exposureTime, 1),
        rationalEntry(0x829d, RATIONAL, &a.fnumber, 1),
        shortEntry(0x8822, 3),
        shortEntry(0x8827, a.isoSpeedRating),
        bytesEntry(0x9000, UNDEFINED, kVersion, 4),
        asciiEntry(0x9003, a.dateTime),
        asciiEntry(0x9004, a.dateTime),
        rationalEntry(0x9201, SRATIONAL, &a.shutterSpeed, 1),
        rationalEntry(0x9202, RATIONAL, &a.aperture, 1),
        rationalEntry(0x9203, SRATIONAL, &a.brightness, 1),
        rationalEntry(0x9204, SRATIONAL, &a.exposureBias, 1),
        rationalEntry(0x9205, RATIONAL, &a.maxAperture, 1),
        shortEntry(0x9207, a.meteringMode),
        shortEntry(0x9209, a.flash),
        rationalEntry(0x920a, RATIONAL, &a.focalLength, 1),
        bytesEntry(0x9286, UNDEFINED, userComment.data(), userComment.size()),
        shortEntry(0xa001, 1),
        longEntry(0xa002, a.width),
        longEntry(0xa003, a.height),
        shortEntry(0xa402, 0),
        shortEntry(0xa403, a.whiteBalance),
        shortEntry(0xa406, a.sceneCaptureType),
    };
    w.patch32(pointerField(8), static_cast<uint32_t>(w.pos()));
    writeIfd(w, exif);

    if (a.gps) {
        static const uint8_t kGpsVersion[4] = {2, 2, 0, 0};
        char latRef[2] = {a.latitudeRef, 0}, lonRef[2] = {a.longitudeRef, 0};
        std::vector<Entry> gps = {
            bytesEntry(0x0000, BYTE, kGpsVersion, 4),
            bytesEntry(0x0001, ASCII, latRef, 2),
            rationalEntry(0x0002, RATIONAL, a.latitude, 3),
            bytesEntry(0x0003, ASCII, lonRef, 2),
            rationalEntry(0x0004, RATIONAL, a.longitude, 3),
            bytesEntry(0x0005, BYTE, &a.altitudeRef, 1),
            rationalEntry(0x0006, RATIONAL, &a.altitude, 1),
            rationalEntry(0x0007, RATIONAL, a.timestamp, 3),
        };
        if (!a.processingMethod.empty()) {
            std::vector<uint8_t> method(kAsciiPrefix, kAsciiPrefix + 8);
            method.insert(method.end(), a.processingMethod.begin(),
                          a.processingMethod.begin() +
                              std::min<size_t>(a.processingMethod.size(), 100));
            gps.push_back(bytesEntry(0x001b, UNDEFINED, method.data(), method.size()));
        }
        gps.push_back(asciiEntry(0x001d, a.datestamp));
        w.patch32(pointerField(9), static_cast<uint32_t>(w.pos()));
        writeIfd(w, gps);
    }

    constexpr size_t kIfd1Size = 2 + 9 * 12 + 4 + 2 * 8;
    constexpr size_t kMaxTiff = 0xffff - 2 - 6;
    if (!thumb.empty() && a.thumbWidth && w.pos() + kIfd1Size + thumb.size() <= kMaxTiff) {
        Rational res{72, 1};
        std::vector<Entry> ifd1 = {
            longEntry(0x0100, a.thumbWidth),
            longEntry(0x0101, a.thumbHeight),
            shortEntry(0x0103, 6),
            shortEntry(0x0112, a.orientation),
            rationalEntry(0x011a, RATIONAL, &res, 1),
            rationalEntry(0x011b, RATIONAL, &res, 1),
            shortEntry(0x0128, 2),
            longEntry(0x0201, 0),
            longEntry(0x0202, static_cast<uint32_t>(thumb.size())),
        };
        size_t ifd1At = w.pos();
        w.patch32(ifd0Next, static_cast<uint32_t>(ifd1At));
        writeIfd(w, ifd1);
        w.patch32(ifd1At + 2 + 7 * 12 + 8, static_cast<uint32_t>(w.pos()));
        w.put(thumb.data(), thumb.size());
    }

    if (w.pos() > kMaxTiff)
        return {};

    std::vector<uint8_t> app1;
    size_t len = 2 + 6 + w.pos();
    app1.reserve(2 + len);
    const uint8_t head[10] = {0xff, 0xe1, uint8_t(len >> 8), uint8_t(len & 0xff),
                              'E', 'x', 'i', 'f', 0, 0};
    app1.insert(app1.end(), head, head + sizeof(head));
    app1.insert(app1.end(), w.out().begin(), w.out().end());
    return app1;
}

}
