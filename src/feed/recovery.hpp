#pragma once

#include "framer.hpp"

enum class RecoveryError : uint8_t {
  RetriesExhausted, // MAX_ATTEMPTS requests in a row with no progress
  Timeout,
  TransportError, // the unicast socket failed
  SessionChanged, // a reply belonged to a different session
  SessionEnded,   // a reply was end-of-session; the gap can't be filled
  Unsupported     // NoRecoverer
};

template <class R, class F>
concept Recoverer =
    Framer<F> && requires(R r, F &framer, AnyMessageSink &sink) {
      {
        r.recover(framer, sink)
      } -> std::same_as<std::expected<void, RecoveryError>>;
    };

struct NoRecoverer {
  template <Framer F, MessageSink Sink>
  std::expected<void, RecoveryError> recover(F &, Sink &) noexcept {
    return std::unexpected(RecoveryError::Unsupported);
  }
};