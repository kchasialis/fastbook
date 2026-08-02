#pragma once

#include <cstdint>
#include <cstring>
#include <span>

class BufReader {
private:
  std::span<const std::byte> buf_;

  template <typename T> T from_big_endian(T value) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
      return std::byteswap(value);
    }
    return value;
  }

public:
  BufReader(std::span<const std::byte> buf) : buf_(buf) {}

  template <typename T> T read_num() noexcept {
    assert(buf_.size() >= sizeof(T));
    T value;
    std::memcpy(&value, buf_.data(), sizeof(T));
    buf_ = buf_.subspan(sizeof(T));
    return from_big_endian(value);
  }

  uint64_t read_timestamp() noexcept {
    uint64_t value{0};
    std::memcpy(reinterpret_cast<uint8_t *>(&value) + 2, buf_.data(), 6);
    buf_ = buf_.subspan(6);
    return from_big_endian(value);
  }

  template <size_t N> void read_chars(char (&dst)[N]) noexcept {
    assert(buf_.size() >= N);
    std::memcpy(dst, buf_.data(), N);
    buf_ = buf_.subspan(N);
  }

  bool has_bytes() const noexcept { return buf_.size() > 0; }
  std::size_t remaining() const noexcept { return buf_.size(); }
  const std::byte *data() const noexcept { return buf_.data(); }
  void set_buf(std::span<const std::byte> new_buf) { buf_ = new_buf; }
};