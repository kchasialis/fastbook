#pragma once

#include "buffer_pool.hpp"
#include "event_queue.hpp"
#include "framer.hpp"
#include "logger.hpp"
#include "parser.hpp"
#include "recovery.hpp"
#include "threading.hpp"
#include "transport.hpp"
#include "types.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <pthread.h>
#include <span>
#include <sys/types.h>
#include <vector>

template <Transport Src, Framer F, Parser P, Recoverer<F> R> class VenueFeed {
private:
  Src &src_;
  std::vector<Producer> producers_;
  F framer_;
  R recoverer_;
  std::atomic<bool> stop_requested_;
  std::atomic<bool> finished_;
  uint64_t queue_full_{0};
  bufpool::BufferPool &bp_ref_;

  void on_framer_error([[maybe_unused]] StreamError e) noexcept {
    // TODO(kostas): what to do here? idk yet, just return for now.
  }

  void on_framer_error(SequencedError e) noexcept {
    switch (e) {
    case SequencedError::Gap: {
      recoverer_.recover(framer_, *this);
      break;
    }
    case SequencedError::Malformed: {
      break;
    }
    case SequencedError::SessionChanged: {
      break;
    }
    case SequencedError::EndOfSession: {
      break;
    }
    default:
      __builtin_unreachable();
      break;
    }
  }

public:
  VenueFeed(Src &src, std::array<Queue, N_SHARDS> &queues,
            bufpool::BufferPool &bp_ref)
      : src_(src), stop_requested_(false), finished_(false), bp_ref_(bp_ref) {
    static_assert(MessageSink<VenueFeed>);
    static_assert(EventSink<VenueFeed>);

    producers_.reserve(queues.size());
    for (auto &queue : queues) {
      producers_.emplace_back(queue.producer());
    }
  }

  void on_event(const MboEvent &e) noexcept {
    static_assert(std::has_single_bit(N_SHARDS));
    Producer &producer = producers_[e.instrument & (N_SHARDS - 1)];
    if (producer.push(e)) [[likely]] {
      return;
    }

    queue_full_++;
    while (!producer.push(e)) {
      if (stop_requested_.load(std::memory_order_relaxed)) {
        return;
      }
    }
  }

  void on_message(std::span<const std::byte> b) noexcept { P::parse(b, *this); }

  void run(uint32_t core, SchedPolicy sched, StartGate &gate) noexcept {
    std::string tname = "feed_" + std::to_string(core);
    if (!pin_and_prioritize(pthread_self(), core, sched)) {
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
        case TransportStatus::WouldBlock:
          break;
        case TransportStatus::Eof:
        case TransportStatus::Error:
          stop();
          break;
        default:
          __builtin_unreachable();
          break;
        }
        continue;
      }
      auto ret = framer_.feed(buf->bytes(), *this);
      if (!ret.has_value()) [[unlikely]] {
        on_framer_error(ret.error());
      }
    }

    finished_.store(true, std::memory_order_relaxed);
    return;

  failed:
    gate.failed.fetch_add(1, std::memory_order_relaxed);
    gate.ready.count_down();
  }

  void stop() noexcept {
    stop_requested_.store(true, std::memory_order_relaxed);
  }

  [[nodiscard]] bool finished() const noexcept {
    return finished_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] uint64_t queue_full_count() const noexcept {
    return queue_full_;
  }
};