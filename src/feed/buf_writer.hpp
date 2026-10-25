#pragma once

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>

class BufWriter {
private:
  std::span<std::byte> buf_;

  template <typename T> T to_big_endian(T value) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
      return std::byteswap(value);
    }
    return value;
  }

public:
  BufWriter(std::span<std::byte> buf) : buf_(buf) {}

  template <typename T> void write_num_be(T val) noexcept {
    assert(buf_.size() >= sizeof(T));
    val = to_big_endian(val);
    std::memcpy(buf_.data(), &val, sizeof(T));
    buf_ = buf_.subspan(sizeof(T));
  }

  template <typename T> void write_num_le(T val) noexcept {
    assert(buf_.size() >= sizeof(T));
    std::memcpy(buf_.data(), &val, sizeof(T));
    buf_ = buf_.subspan(sizeof(T));
  }

  void write_sv(std::string_view sv) noexcept {
    size_t n = sv.size();
    assert(buf_.size() >= n);
    std::memcpy(buf_.data(), sv.data(), n);
    buf_ = buf_.subspan(n);
  }
};