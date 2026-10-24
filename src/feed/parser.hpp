#pragma once

#include "types.hpp"
#include <concepts>
#include <span>

template <class S>
concept EventSink = requires(S s, const MboEvent &e) {
  { s.on_event(e) } -> std::same_as<void>;
};

struct AnyEventSink {
  void on_event(const MboEvent &);
};
static_assert(EventSink<AnyEventSink>);

template <class P>
concept Parser = requires(std::span<const std::byte> b, AnyEventSink &sink) {
  { P::parse(b, sink) } -> std::same_as<void>;
};
