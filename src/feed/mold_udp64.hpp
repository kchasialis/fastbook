#pragma once

#include "buf_reader.hpp"
#include "buf_writer.hpp"
#include <cstdint>
#include <optional>
#include <string_view>

namespace mold {

inline constexpr size_t SESSION_SIZE = 10;
inline constexpr size_t HEADER_SIZE =
    SESSION_SIZE + sizeof(uint64_t) + sizeof(uint16_t);
inline constexpr uint16_t HEARTBEAT = 0x0000;
inline constexpr uint16_t END_OF_SESSION = 0xFFFF;

struct Header {
  std::string_view session;
  uint64_t seq_num;
  uint16_t msg_count;
};

inline std::optional<Header> read_header(BufReader &reader) noexcept {
  if (reader.remaining() < HEADER_SIZE) [[unlikely]] {
    return std::nullopt;
  }

  return Header{.session = reader.take_n_sv(SESSION_SIZE),
                .seq_num = reader.read_num<uint64_t>(),
                .msg_count = reader.read_num<uint16_t>()};
}

inline void write_header(BufWriter &writer, const Header &hdr) noexcept {
  writer.write_sv(hdr.session);
  writer.write_num_be(hdr.seq_num);
  writer.write_num_be(hdr.msg_count);
}

} // namespace mold
