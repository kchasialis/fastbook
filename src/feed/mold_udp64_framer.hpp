#pragma once

#include "buf_reader.hpp"
#include "framer.hpp"
#include "mold_udp64.hpp"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

class MoldUDP64Framer {
private:
  char last_session_[mold::SESSION_SIZE];
  bool has_session_{false};
  uint64_t exp_seq_{1};
  uint64_t gap_end_{0};

public:
  using Error = SequencedError;

  std::string_view session() const noexcept {
    return {last_session_, mold::SESSION_SIZE};
  }

  template <MessageSink Sink>
  std::expected<void, Error> feed(std::span<const std::byte> data,
                                  Sink &sink) noexcept {
    BufReader reader(data);

    auto hdr_opt = mold::read_header(reader);
    if (!hdr_opt.has_value()) [[unlikely]] {
      return std::unexpected(SequencedError::Malformed);
    }
    auto hdr = hdr_opt.value();

    if (!has_session_) [[unlikely]] {
      std::memcpy(last_session_, hdr.session.data(), mold::SESSION_SIZE);
      has_session_ = true;
    }

    if (std::memcmp(last_session_, hdr.session.data(), mold::SESSION_SIZE) != 0)
        [[unlikely]] {
      std::memcpy(last_session_, hdr.session.data(), mold::SESSION_SIZE);
      exp_seq_ = 1;
      return std::unexpected(SequencedError::SessionChanged);
    }

    if (hdr.seq_num > exp_seq_) [[unlikely]] {
      gap_end_ = hdr.seq_num;
      return std::unexpected{SequencedError::Gap};
    }

    if (hdr.msg_count == mold::END_OF_SESSION) [[unlikely]] {
      return std::unexpected(SequencedError::EndOfSession);
    }

    if (hdr.seq_num + hdr.msg_count <= exp_seq_) [[unlikely]] {
      return {};
    }

    uint64_t current_seq = hdr.seq_num;
    for (uint16_t i = 0; i < hdr.msg_count; i++, current_seq++) {
      if (reader.remaining() < 2) [[unlikely]] {
        return std::unexpected(SequencedError::Malformed);
      }
      auto msg_length = reader.read_num<uint16_t>();
      if (reader.remaining() < msg_length) [[unlikely]] {
        return std::unexpected(SequencedError::Malformed);
      }

      auto s = reader.take_n(static_cast<size_t>(msg_length));
      if (current_seq < exp_seq_) {
        continue;
      }
      sink.on_message(s);
      exp_seq_++;
    }

    return {};
  }

  [[nodiscard]] uint64_t expected() const noexcept { return exp_seq_; }

  [[nodiscard]] uint64_t gap_end() const noexcept { return gap_end_; }
};