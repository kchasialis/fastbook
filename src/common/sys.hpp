#pragma once

#include <cerrno>
#include <cstdlib>
#include <format>
#include <memory>
#include <new>
#include <source_location>
#include <string_view>
#include <sys/mman.h>
#include <system_error>
#include <type_traits>
#include <unistd.h>
#include <utility>

inline constexpr size_t PAGE_BYTES = 4096;

struct FreeDeleter {
  void operator()(void *p) const noexcept { std::free(p); }
};

template <class T> using PrefaultedArray = std::unique_ptr<T[], FreeDeleter>;

template <class T>
  requires std::is_trivially_destructible_v<T>
PrefaultedArray<T> make_prefaulted_array(size_t n) {
  size_t bytes = (n * sizeof(T) + PAGE_BYTES - 1) / PAGE_BYTES * PAGE_BYTES;
  void *mem = std::aligned_alloc(PAGE_BYTES, bytes);
  if (mem == nullptr) {
    throw std::bad_alloc();
  }
  (void)::madvise(mem, bytes, MADV_POPULATE_WRITE);

  T *arr = static_cast<T *>(mem);
  std::uninitialized_value_construct_n(arr, n);
  return PrefaultedArray<T>(arr);
}

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

  int operator*() const noexcept { return fd_; }

  int val() const noexcept { return fd_; }
  int release() noexcept {
    int ret = fd_;
    fd_ = -1;

    return ret;
  }
};
