#pragma once

#include "feed_handler.hpp"
#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <sys/types.h>

template <typename Source>
concept Reader = requires(Source s, void *buf, size_t count) {
  { s.read(buf, count) } -> std::same_as<ssize_t>;
};

template <Reader Source> class FeedReader {
private:
  static constexpr size_t BUF_SIZE = 1024;

  Source &src_;
  FeedHandler<Queue> &handler_;
  std::atomic<bool> stop_;

public:
  FeedReader(Source &src, FeedHandler<Queue> &handler)
      : src_(src), handler_(handler), stop_(false) {}

  void run() {
    std::array<std::byte, BUF_SIZE> read_buf;
    while (!stop_.load(std::memory_order_acquire)) {
      auto bytes_read = src_.read(read_buf.data(), BUF_SIZE);
      if (bytes_read == 0 || bytes_read == -1) {
        stop();
        break;
      }
      std::span<std::byte> span{read_buf.data(), static_cast<size_t>(bytes_read)};
      handler_.feed(span);
    }
  }

  void stop() { stop_.store(true, std::memory_order_release); }
};