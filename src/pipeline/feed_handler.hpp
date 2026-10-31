#pragma once

#include "book_builder.hpp"
#include "buffer_pool.hpp"
#include "event_queue.hpp"
#include "logger.hpp"
#include "profiling.hpp"
#include "thread_config.hpp"
#include "threading.hpp"
#include "timing.hpp"
#include "transport.hpp"
#include "venue_feed.hpp"
#include <concepts>
#include <filesystem>
#include <fstream>
#include <memory>
#include <pthread.h>
#include <sched.h>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

template <Transport Src, Framer F, Parser P, Recoverer<F> R = NoRecoverer>
class FeedHandler {
private:
  bufpool::BufferPool bp_;
  Src src_;
  R recoverer_;
  std::array<Queue, N_SHARDS> queues_;
  VenueFeed<Src, F, P, R> feed_;
  fs::path thread_cfg_fpath_;
  StartGate gate_;
  bool started_;
  bool start_ok_;
  bool stopped_;
  timing::MonoClock::time_point go_ts_;
  std::thread feed_thread_;
  std::vector<std::unique_ptr<BookBuilder>> book_builders_;
  std::vector<std::thread> bb_threads_;

  bool init_and_pin_threads() {
    bool ok = true;

    auto stop_fn = [&](const char *what) {
      ok = false;
      feed_.stop();
      for (const auto &bb_builder : book_builders_) {
        bb_builder->stop();
      }
      gate_.go.count_down();
      if (what) {
        LOG_ERROR("Startup threads failed to init, stopping the pipeline. "
                  "(exception: {})",
                  what);
      } else {
        LOG_ERROR("Startup threads failed to init, stopping the pipeline");
      }
    };

    try {
      const ThreadConfig cfg = ThreadConfig::load(thread_cfg_fpath_);
      feed_thread_ = std::thread(&VenueFeed<Src, F, P, R>::run, &feed_,
                                 cfg.feed_core, cfg.sched, std::ref(gate_));

      bb_threads_.reserve(cfg.builder_cores.size());
      for (size_t i = 0; i < cfg.builder_cores.size(); i++) {
        bb_threads_.push_back(
            std::thread(&BookBuilder::run, book_builders_[i].get(),
                        cfg.builder_cores[i], cfg.sched, std::ref(gate_)));
      }
    } catch (const std::exception &e) {
      stop_fn(e.what());
      return ok;
    }

    gate_.ready.wait();
    if (gate_.failed.load(std::memory_order_relaxed) > 0) {
      stop_fn(nullptr);
      return ok;
    }

    go_ts_ = timing::MonoClock::now();
    gate_.go.count_down();

    return ok;
  }

  void report() const {
    const auto ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            timing::MonoClock::now() - go_ts_)
            .count());

    uint64_t total = 0;
    for (size_t i = 0; i < book_builders_.size(); i++) {
      const BookBuilder::Stats &st = book_builders_[i]->stats();
      total += st.msgs;
      LOG_INFO("builder {}: msgs={} books={} dropped_adds={} unknown_oids={}",
               i, st.msgs, book_builders_[i]->book_count(), st.dropped_adds,
               st.unknown_oids);
    }

    profiling::Histogram tick_to_book;
    for (const auto &bb_builder : book_builders_) {
      tick_to_book.merge(bb_builder->tick_to_book());
    }
    tick_to_book.report("tick_to_book_ns", profiling::ns_per_tick());

    const uint64_t rate = ms > 0 ? total * 1000 / ms : 0;
    LOG_INFO("{} msgs in {} ms ({} msgs/s), feed stalled on a full queue {} "
             "times, dropped {} malformed datagrams",
             total, ms, rate, feed_.queue_full_count(),
             feed_.malformed_count());
  }

public:
  template <typename... Args>
    requires std::default_initializable<R>
  FeedHandler(const fs::path &thread_cfg_fpath, Args &&...args)
      : FeedHandler(thread_cfg_fpath, R{}, std::forward<Args>(args)...) {}

  template <typename... Args>
  FeedHandler(const fs::path &thread_cfg_fpath, R recoverer, Args &&...args)
      : src_(std::forward<Args>(args)...), recoverer_(std::move(recoverer)),
        feed_(src_, recoverer_, queues_, bp_),
        thread_cfg_fpath_(thread_cfg_fpath), gate_(N_SHARDS + 1),
        started_(false), start_ok_(false), stopped_(false) {}

  ~FeedHandler() { stop_and_wait(); }

  void stop_and_wait() {
    if (stopped_) {
      return;
    }
    stopped_ = true;

    feed_.stop();
    if (feed_thread_.joinable()) {
      feed_thread_.join();
    }

    for (const auto &bb_builder : book_builders_) {
      bb_builder->stop();
    }

    for (auto &bb_thread : bb_threads_) {
      if (bb_thread.joinable()) {
        bb_thread.join();
      }
    }

    if (start_ok_) {
      report();
    }
  }

  [[nodiscard]] bool feed_finished() const noexcept { return feed_.finished(); }

  [[nodiscard]] bool start() {
    if (started_) {
      LOG_WARN("FeedHandler::start() is called twice, this is a no-op");
      return start_ok_;
    }

    started_ = true;

    book_builders_.reserve(N_SHARDS);
    for (size_t i = 0; i < N_SHARDS; i++) {
      book_builders_.emplace_back(
          std::make_unique<BookBuilder>(queues_[i].consumer(), bp_));
    }

    start_ok_ = init_and_pin_threads();
    return start_ok_;
  }
};