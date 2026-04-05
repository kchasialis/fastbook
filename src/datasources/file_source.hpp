#pragma once

#include <cstddef>
#include <fcntl.h>
#include <format>
#include <stdexcept>
#include <unistd.h>

class FileSource {
private:
  int fd_;

public:
  FileSource(const char *fpath) {
    if ((fd_ = open(fpath, O_RDONLY)) < 0) {
      throw std::runtime_error(
          std::format("Failed to open fpath: {} for reading", fpath));
    }
  }

  ~FileSource() { close(fd_); }

  FileSource() = delete;
  FileSource(const FileSource &) = delete;
  FileSource(FileSource &&) = delete;
  FileSource &operator=(const FileSource &) = delete;
  FileSource &operator=(FileSource &&) = delete;

  ssize_t read(void *buf, size_t count) { return ::read(fd_, buf, count); }
};