#pragma once

#include <cerrno>
#include <format>
#include <source_location>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>

inline int check(int rc, std::string_view what,
                 std::source_location loc = std::source_location::current()) {
  if (rc < 0) {
    throw std::system_error(
        errno, std::system_category(),
        std::format("{} at {}:{}", what, loc.file_name(), loc.line()));
  }

  return rc;
}

inline int
check_neg(int rc, std::string_view what,
          std::source_location loc = std::source_location::current()) {
  if (rc < 0) {
    throw std::system_error(
        -rc, std::system_category(),
        std::format("{} at {}:{}", what, loc.file_name(), loc.line()));
  }

  return rc;
}

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
  FdWrapper &operator=(int fd) noexcept {
    if (fd_ >= 0) {
      close(fd_);
    }
    this->fd_ = fd;
    return *this;
  }

  int val() const noexcept { return fd_; }
  int release() noexcept {
    int ret = fd_;
    fd_ = -1;

    return ret;
  }
};
