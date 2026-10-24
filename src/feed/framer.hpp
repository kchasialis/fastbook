#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

enum class StreamError : uint8_t { Malformed };
enum class SequencedError : uint8_t {
  Malformed,
  Gap,
  EndOfSession,
  SessionChanged
};

template <class S>
concept MessageSink = requires(S s, std::span<const std::byte> b) {
  { s.on_message(b) } -> std::same_as<void>;
};

struct AnyMessageSink {
  void on_message(std::span<const std::byte>);
};
static_assert(MessageSink<AnyMessageSink>);

template <class F>
concept Framer =
    requires(F f, std::span<const std::byte> data, AnyMessageSink &sink) {
      typename F::Error;
      {
        f.feed(data, sink)
      } -> std::same_as<std::expected<void, typename F::Error>>;
    };