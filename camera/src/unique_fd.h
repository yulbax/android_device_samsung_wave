#pragma once

#include <unistd.h>

#include <utility>

namespace wave {

class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) : fd_(fd) {}
    UniqueFd(UniqueFd&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    UniqueFd& operator=(UniqueFd&& o) noexcept
    {
        if (this != &o)
            reset(std::exchange(o.fd_, -1));
        return *this;
    }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    ~UniqueFd() { reset(); }

    int get() const { return fd_; }
    explicit operator bool() const { return fd_ >= 0; }
    void reset(int fd = -1)
    {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

class Mapping {
public:
    Mapping() = default;
    Mapping(void* addr, size_t size) : addr_(addr), size_(size) {}
    Mapping(Mapping&& o) noexcept
        : addr_(std::exchange(o.addr_, nullptr)), size_(std::exchange(o.size_, 0)) {}
    Mapping& operator=(Mapping&& o) noexcept
    {
        if (this != &o) {
            reset();
            addr_ = std::exchange(o.addr_, nullptr);
            size_ = std::exchange(o.size_, 0);
        }
        return *this;
    }
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    ~Mapping() { reset(); }

    static Mapping map(int fd, size_t size, off_t offset = 0);

    unsigned char* data() const { return static_cast<unsigned char*>(addr_); }
    size_t size() const { return size_; }
    explicit operator bool() const { return addr_ != nullptr; }
    void reset();

private:
    void* addr_ = nullptr;
    size_t size_ = 0;
};

}
