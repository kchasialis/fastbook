#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

#include "buf_reader.hpp"
#include "framer.hpp"

template <bool, size_t MaxFrame> struct Reassembly {
  std::byte buf[MaxFrame];
  uint32_t leftover{0};
};

template <size_t MaxFrame> struct Reassembly<false, MaxFrame> {};

template <bool MayStraddle, size_t MaxPayload> class LengthPrefixFramer {
private:
  static constexpr size_t MAX_FRAME_SIZE = MaxPayload + sizeof(uint16_t);

  [[no_unique_address]] Reassembly<MayStraddle, MAX_FRAME_SIZE> reasm_;

  std::optional<std::span<const std::byte>>
  handle_leftover_bytes_(std::span<const std::byte> &data,
                         bool &malformed) noexcept {
    if (reasm_.leftover < 2) {
      assert(reasm_.leftover == 1);
      if (data.empty()) [[unlikely]] {
        return std::nullopt;
      }
      reasm_.buf[reasm_.leftover++] = data[0];
      data = data.subspan(1);
    }

    BufReader reader{std::span<const std::byte>(reasm_.buf, reasm_.leftover)};
    uint16_t msg_size = reader.template read_num<uint16_t>();

    if (msg_size > MaxPayload) [[unlikely]] {
      malformed = true;
      return std::nullopt;
    }

    size_t needed = (sizeof(msg_size) + msg_size) - reasm_.leftover;
    if (data.size() >= needed) [[likely]] {
      memcpy(reasm_.buf + reasm_.leftover, data.data(), needed);
    } else {
      memcpy(reasm_.buf + reasm_.leftover, data.data(), data.size());
      reasm_.leftover += static_cast<uint32_t>(data.size());
      data = data.subspan(data.size());
      return std::nullopt;
    }

    data = data.subspan(needed);
    reasm_.leftover = 0;

    return std::span<const std::byte>(reasm_.buf + sizeof(msg_size), msg_size);
  }

public:
  using Error = StreamError;

  template <MessageSink Sink>
  std::expected<void, Error> feed(std::span<const std::byte> data,
                                  Sink &sink) noexcept {
    if constexpr (MayStraddle) {
      if (reasm_.leftover > 0) {
        bool malformed = false;
        auto s = handle_leftover_bytes_(data, malformed);
        if (malformed) [[unlikely]] {
          reasm_.leftover = 0;
          return std::unexpected(StreamError::Malformed);
        }
        if (s.has_value()) {
          sink.on_message(s.value());
        } else {
          return {};
        }
      }
    }

    BufReader reader{data};

    while (reader.has_bytes()) {
      [[maybe_unused]] const std::byte *save_bytes = reader.data();
      uint32_t rem = static_cast<uint32_t>(reader.remaining());
      if constexpr (MayStraddle) {
        if (rem < 2) {
          std::memcpy(reasm_.buf, save_bytes, rem);
          reasm_.leftover = rem;
          break;
        }
      } else {
        if (rem < sizeof(uint16_t)) [[unlikely]] {
          return std::unexpected(StreamError::Malformed);
        }
      }

      uint16_t msg_length = reader.template read_num<uint16_t>();

      if (msg_length > MaxPayload) [[unlikely]] {
        if constexpr (MayStraddle) {
          reasm_.leftover = 0;
        }
        return std::unexpected(StreamError::Malformed);
      }

      if constexpr (MayStraddle) {
        if (reader.remaining() < msg_length) {
          std::memcpy(reasm_.buf, save_bytes, rem);
          reasm_.leftover = rem;
          break;
        }
      } else {
        if (reader.remaining() < msg_length) [[unlikely]] {
          return std::unexpected(StreamError::Malformed);
        }
      }

      sink.on_message(reader.take_n(static_cast<size_t>(msg_length)));
    }

    return {};
  }
};
