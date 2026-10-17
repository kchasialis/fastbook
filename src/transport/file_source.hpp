#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <expected>
#include <fcntl.h>
#include <format>
#include <span>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

#include "transport.hpp"
#include "sys.hpp"

class FileSource {
private:
  std::byte *start_;
  std::span<const std::byte> remaining_;
  size_t length_;

public:
  static constexpr bool messages_may_straddle = true;
  static constexpr size_t SLICE_SIZE = 1 << 20;

  class Buffer {
    std::span<const std::byte> bytes_;

  public:
    explicit Buffer(std::span<const std::byte> bytes) : bytes_(bytes) {}
    std::span<const std::byte> bytes() const noexcept { return bytes_; }
  };

  FileSource() : start_(nullptr), length_(0) {}
  explicit FileSource(const char *fpath) : start_(nullptr), length_(0) {
    FdWrapper fd(open(fpath, O_RDONLY));

    check(fd.val(), "open");

    struct stat st;
    check(fstat(fd.val(), &st), "fstat");
    if (st.st_size == 0) {
      return;
    }
    length_ = static_cast<size_t>(st.st_size);
    start_ = static_cast<std::byte *>(
        mmap(NULL, length_, PROT_READ, MAP_PRIVATE, fd.val(), 0));
    if (start_ == MAP_FAILED) {
      throw std::runtime_error(std::format("Failed to mmap file: {}, error: {}",
                                           fpath, strerror(errno)));
    }
    madvise(start_, length_, MADV_SEQUENTIAL);
    remaining_ = std::span<const std::byte>(start_, length_);
  }

  ~FileSource() {
    if (start_ != nullptr) {
      munmap(start_, length_);
    }
  }

  FileSource(const FileSource &) = delete;
  FileSource(FileSource &&other) noexcept
      : start_(other.start_), remaining_(other.remaining_),
        length_(other.length_) {
    other.start_ = nullptr;
    other.remaining_ = {};
    other.length_ = 0;
  }
  FileSource &operator=(const FileSource &) = delete;
  FileSource &operator=(FileSource &&rhs) noexcept {
    std::swap(start_, rhs.start_);
    std::swap(remaining_, rhs.remaining_);
    std::swap(length_, rhs.length_);
    return *this;
  }

  std::expected<FileSource::Buffer, Status> next() noexcept {
    if (remaining_.empty()) {
      return std::unexpected(Status::Eof);
    }

    const size_t n = std::min(remaining_.size(), SLICE_SIZE);
    Buffer b{remaining_.first(n)};
    remaining_ = remaining_.subspan(n);

    return b;
  }
};

static_assert(Transport<FileSource>);
