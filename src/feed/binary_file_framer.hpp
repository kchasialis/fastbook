#pragma once

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

#include "buf_reader.hpp"

class BinaryFileFramer {
private:
  static constexpr size_t MAX_MSG_SIZE = 50;
  std::byte reassembly_buffer_[MAX_MSG_SIZE];
  uint32_t leftover_bytes_;

  bool handle_leftover_bytes(std::span<const std::byte> &data) noexcept {
    if (leftover_bytes_ < 2) {
      assert(leftover_bytes_ == 1);
      reassembly_buffer_[leftover_bytes_++] = data[0];
      data = data.subspan(1);
    }

    BufReader reader{
        std::span<const std::byte>(reassembly_buffer_, leftover_bytes_)};
    uint16_t msg_size = reader.template read_num<uint16_t>();
    size_t needed = (sizeof(msg_size) + msg_size) - leftover_bytes_;
    if (data.size() >= needed) [[likely]] {
      memcpy(reassembly_buffer_ + leftover_bytes_, data.data(), needed);
    } else {
      memcpy(reassembly_buffer_ + leftover_bytes_, data.data(), data.size());
      leftover_bytes_ += data.size();
      data = data.subspan(data.size());
      return false;
    }

    reader.set_buf(std::span<const std::byte>(
        reassembly_buffer_ + sizeof(msg_size), msg_size));
    data = data.subspan(needed);
    leftover_bytes_ = 0;

    return true;
  }

public:
  BinaryFileFramer() {}

  void feed(std::span<const std::byte> data) noexcept {
    if (leftover_bytes_ > 0) {
      std::optional<Message> leftover_msg = handle_leftover_bytes(data);
      if (leftover_msg.has_value()) [[likely]] {
        producer_.push(leftover_msg.value());
      } else {
        return;
      }
    }

    BufReader reader{data};

    while (reader.has_bytes()) {
      const std::byte *save_bytes = reader.data();
      uint32_t rem = static_cast<uint32_t>(reader.remaining());
      if (rem < 2) {
        std::memcpy(reassembly_buffer_, save_bytes, rem);
        leftover_bytes_ = rem;
        break;
      }

      uint16_t msg_length = reader.template read_num<uint16_t>();

      if (reader.remaining() < msg_length) {
        std::memcpy(reassembly_buffer_, save_bytes, rem);
        leftover_bytes_ += rem;
        break;
      }
    }
  }
};
