#pragma once

#include "buffer_pool.hpp"
#include "event_queue.hpp"
#include "itch_parser.hpp"
#include "length_prefix_framer.hpp"
#include "logger.hpp"
#include "threading.hpp"
#include "transport.hpp"
#include "types.hpp"
#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <pthread.h>
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

template <Transport Src, Framer F, Parser P> class VenueFeed {
private:
  Src &src_;
  std::vector<Producer> producers_;
  F framer_;
  std::atomic<bool> stop_requested_;
  bufpool::BufferPool &bp_ref_;

public:
  VenueFeed(Src &src, std::array<Queue, N_SHARDS> &queues,
            bufpool::BufferPool &bp_ref)
      : src_(src), stop_requested_(false), bp_ref_(bp_ref) {
    static_assert(MessageSink<VenueFeed>);
    static_assert(EventSink<VenueFeed>);

    producers_.reserve(queues.size());
    for (size_t i = 0; i < queues.size(); i++) {
      producers_.emplace_back(queues[i].producer());
    }
  }

  void on_event(const MboEvent &e) {
    static_assert(std::has_single_bit(N_SHARDS));
    producers_[e.instrument & (N_SHARDS - 1)].push(e);
  }

  void on_message(std::span<const std::byte> b) noexcept { P::parse(b, *this); }

  void on_error(FramingError e) noexcept { (void)e; }

  void run(uint32_t core, StartGate &gate) noexcept {
    std::string tname = "feed_" + std::to_string(core);
    if (!pin_and_prioritize(pthread_self(), core)) {
      LOG_ERROR("Failed to pin thread to core: {}", core);
      goto failed;
    }

    if (pthread_setname_np(pthread_self(), tname.c_str()) != 0) {
      LOG_ERROR("Failed to set thread name {} for core: {}", tname.c_str(),
                core);
      goto failed;
    }

    bufpool::thread_init();
    logger::thread_init();

    gate.ready.count_down();
    gate.go.wait();

    while (!stop_requested_.load(std::memory_order_relaxed)) {
      auto buf = src_.next();
      if (!buf.has_value()) {
        auto err = buf.error();
        switch (err) {
        case Status::WouldBlock:
          break;
        case Status::Eof:
        case Status::Error:
          stop();
          break;
        default:
          __builtin_unreachable();
          break;
        }
        continue;
      }
      framer_.feed(buf->bytes(), *this);
    }

    return;

  failed:
    gate.failed.fetch_add(1, std::memory_order_relaxed);
    gate.ready.count_down();
  }

  void stop() noexcept {
    stop_requested_.store(true, std::memory_order_relaxed);
  }
};