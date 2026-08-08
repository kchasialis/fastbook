#pragma once

#include <unistd.h>
#include <utility>

class FdWrapper {
private:
  int fd_;

public:
  FdWrapper() : fd_(-1) {}
  explicit FdWrapper(int fd) : fd_(fd) {}
  ~FdWrapper() {
    if (fd_ >= 0) {
      close(fd_);
    }
  }

  FdWrapper(const FdWrapper &) = delete;
  FdWrapper(FdWrapper &&other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
  FdWrapper &operator=(const FdWrapper &) = delete;
  FdWrapper &operator=(FdWrapper &&rhs) noexcept {
    std::swap(this->fd_, rhs.fd_);
    return *this;
  }

  int val() const noexcept { return fd_; }
  int release() noexcept {
    int ret = fd_;
    fd_ = -1;

    return ret;
  }
};