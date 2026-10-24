#pragma once

#include <concepts>
#include <cstdint>
#include <expected>
#include <span>

enum class TransportStatus : uint8_t { WouldBlock, Eof, Error };

template <class S>
concept Transport = requires(S s, typename S::Buffer b) {
  typename S::Buffer;
  typename std::bool_constant<S::messages_may_straddle>;
  {
    s.next()
  } -> std::same_as<std::expected<typename S::Buffer, TransportStatus>>;
  { b.bytes() } -> std::same_as<std::span<const std::byte>>;
};

template <class S>
concept BidirectionalTransport =
    Transport<S> && requires(S s, std::span<const std::byte> d) {
      { s.send(d) } -> std::same_as<std::expected<void, TransportStatus>>;
    };