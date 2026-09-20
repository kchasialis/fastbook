#pragma once

#include "book_builder.hpp"
#include "buffer_pool.hpp"
#include "event_queue.hpp"
#include "thread_config.hpp"
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
  std::thread feed_thread_;
  std::vector<std::unique_ptr<BookBuilder>> book_builders_;
  std::vector<std::thread> bb_threads_;

  void init_and_pin_threads(const fs::path &thread_cfg_fpath) {
    // Parse and validate everything first, then create threads, so a bad
    // config never leaves a joinable std::thread behind.
    const ThreadConfig cfg = ThreadConfig::load(thread_cfg_fpath);

    feed_thread_ =
        std::thread(&VenueFeed<Src, F, P>::run, &feed_, cfg.feed_core);

    for (size_t i = 0; i < cfg.builder_cores.size(); i++) {
      bb_threads_.push_back(std::thread(
          &BookBuilder::run, book_builders_[i].get(), cfg.builder_cores[i]));
    }
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
      : src_(std::forward<Args>(args)...), feed_(src_, queues_, bp_) {
    book_builders_.reserve(N_SHARDS);
    for (size_t i = 0; i < N_SHARDS; i++) {
      book_builders_.emplace_back(
          std::make_unique<BookBuilder>(queues_[i].consumer(), bp_));
    }

    init_and_pin_threads(thread_cfg_fpath);
  }

  ~FeedHandler() { stop_and_wait(); }
};