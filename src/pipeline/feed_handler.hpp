#pragma once

#include "book_builder.hpp"
#include "buffer_pool.hpp"
#include "event_queue.hpp"
#include "logger.hpp"
#include "thread_config.hpp"
#include "threading.hpp"
#include "transport.hpp"
#include "venue_feed.hpp"
#include <filesystem>
#include <fstream>
#include <memory>
#include <pthread.h>
#include <sched.h>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

template <Transport Src, Framer F, Parser P> class FeedHandler {
private:
  bufpool::BufferPool bp_;
  Src src_;
  std::array<Queue, N_SHARDS> queues_;
  VenueFeed<Src, F, P> feed_;
  fs::path thread_cfg_fpath_;
  StartGate gate_;
  bool started_;
  bool start_ok_;
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
      feed_thread_ = std::thread(&VenueFeed<Src, F, P>::run, &feed_,
                                 cfg.feed_core, std::ref(gate_));

      bb_threads_.reserve(cfg.builder_cores.size());
      for (size_t i = 0; i < cfg.builder_cores.size(); i++) {
        bb_threads_.push_back(
            std::thread(&BookBuilder::run, book_builders_[i].get(),
                        cfg.builder_cores[i], std::ref(gate_)));
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

    gate_.go.count_down();

    return ok;
  }

  void stop_and_wait() {
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
  }

public:
  template <typename... Args>
  FeedHandler(const fs::path &thread_cfg_fpath, Args &&...args)
      : src_(std::forward<Args>(args)...), feed_(src_, queues_, bp_),
        thread_cfg_fpath_(thread_cfg_fpath), gate_(N_SHARDS + 1),
        started_(false), start_ok_(false) {}

  ~FeedHandler() { stop_and_wait(); }

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