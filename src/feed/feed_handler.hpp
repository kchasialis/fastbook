#pragma once

#include "itch_parser.hpp"
#include "length_prefix_framer.hpp"
#include "mbo_event.hpp"
#include "spsc_queue.hpp"
#include "transport.hpp"
#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <sys/types.h>
#include <vector>

struct AnyMessageSink {
  void on_message(std::span<const std::byte>);
  void on_error(FramingError);
};
static_assert(MessageSink<AnyMessageSink>);

struct AnyEventSink {
  void on_event(const MboEvent &);
};
static_assert(EventSink<AnyEventSink>);

template <class F>
concept Framer =
    requires(F f, std::span<const std::byte> data, AnyMessageSink &sink) {
      { f.feed(data, sink) } -> std::same_as<void>;
    };

template <class P>
concept Parser = requires(std::span<const std::byte> b, AnyEventSink &sink) {
  { P::parse(b, sink) } -> std::same_as<void>;
};

template <Transport Src, Framer F, Parser P> class FeedHandler {
private:
  // TODO(kostas): Maybe move this declaration somewhere else.
  using Queue = SPSCQueue<MboEvent, 1024>;
  using Producer = SPSCQueue<MboEvent, 1024>::SPSCProducer;
  static constexpr size_t N_SHARDS = 1024;

  static_assert(std::has_single_bit(N_SHARDS));

  Src &src_;
  std::vector<Producer> producers_;
  F framer_;
  std::atomic<bool> stop_requested_;

public:
  FeedHandler(Src &src, std::array<Queue, N_SHARDS> &queues)
      : src_(src), stop_requested_(false) {
    static_assert(MessageSink<FeedHandler>);
    static_assert(EventSink<FeedHandler>);

    producers_.reserve(queues.size());
    for (size_t i = 0; i < queues.size(); i++) {
      producers_.emplace_back(queues[i].producer());
    }
  }

  void on_event(const MboEvent &e) {
    producers_[e.instrument & (N_SHARDS - 1)].push(e);
  }

  void on_message(std::span<const std::byte> b) noexcept { P::parse(b, *this); }

  void on_error(FramingError e) {}

  void run() noexcept {
    while (!stop_requested_.load(std::memory_order_relaxed)) {
      auto buf = src_.next();
      if (!buf.has_value()) {
        // TODO: handle error path
        continue;
      }
      framer_.feed(buf->bytes(), *this);
    }
  }

  void stop() noexcept {
    stop_requested_.store(true, std::memory_order_relaxed);
  }
};