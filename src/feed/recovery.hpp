#pragma once

#include "framer.hpp"

enum class RecoveryError : uint8_t { Timeout, SessionMismatch, Unsupported };

template <class R, class F>
concept Recoverer =
    Framer<F> && requires(R r, F &framer, AnyMessageSink &sink) {
      {
        r.recover(framer, sink)
      } -> std::same_as<std::expected<void, RecoveryError>>;
    };