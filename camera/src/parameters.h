#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace wave {

struct Size {
    int width;
    int height;
    bool operator==(const Size&) const = default;
};

class Parameters {
public:
    Parameters() = default;
    explicit Parameters(std::string_view flat) { unflatten(flat); }

    void unflatten(std::string_view flat);
    std::string flatten() const;

    void set(std::string_view key, std::string_view value);
    void set(std::string_view key, int value);
    void remove(std::string_view key);
    const char* get(std::string_view key) const;
    int getInt(std::string_view key) const;

    Size previewSize() const { return getSize("preview-size"); }
    void setPreviewSize(Size s) { setSize("preview-size", s); }
    Size pictureSize() const { return getSize("picture-size"); }
    void setPictureSize(Size s) { setSize("picture-size", s); }
    const char* previewFormat() const { return get("preview-format"); }
    const char* pictureFormat() const { return get("picture-format"); }
    int previewFrameRate() const { return getInt("preview-frame-rate"); }
    void previewFpsRange(int& min, int& max) const;
    std::vector<Size> sizes(std::string_view key) const;

private:
    void store(std::string_view key, std::string_view value);
    Size getSize(std::string_view key) const;
    void setSize(std::string_view key, Size s);

    std::vector<std::pair<std::string, std::string>> entries_;
};

bool listContains(const char* list, const char* value);

}
