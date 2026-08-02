#pragma once

#include <concepts>
#include <expected>
#include <span>

enum class Status { WouldBlock, Eof, Error };

template <class S>
concept Transport = requires(S s, typename S::Buffer b) {
  typename S::Buffer;
  typename std::bool_constant<S::messages_may_straddle>;
  { s.next() } -> std::same_as<std::expected<typename S::Buffer, Status>>;
  { b.bytes() } -> std::same_as<std::span<const std::byte>>;
};