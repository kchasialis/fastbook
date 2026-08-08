#pragma once

#include <concepts>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <system_error>

enum class Status { WouldBlock, Eof, Error };

template <class S>
concept Transport = requires(S s, typename S::Buffer b) {
  typename S::Buffer;
  typename std::bool_constant<S::messages_may_straddle>;
  { s.next() } -> std::same_as<std::expected<typename S::Buffer, Status>>;
  { b.bytes() } -> std::same_as<std::span<const std::byte>>;
};

template <class S>
concept BidirectionalTransport =
    requires(S s, typename S::Buffer b, std::span<const std::byte> d) {
      typename S::Buffer;
      typename std::bool_constant<S::messages_may_straddle>;
      { s.next() } -> std::same_as<std::expected<typename S::Buffer, Status>>;
      { s.send(d) } -> std::same_as<std::expected<void, Status>>;
      { b.bytes() } -> std::same_as<std::span<const std::byte>>;
    };