#pragma once

#include "event_queue.hpp"
#include "hash_map.hpp"
#include "logger.hpp"
#include "order_book.hpp"
#include "threading.hpp"
#include "types.hpp"
#include <atomic>
#include <cassert>
#include <memory>
#include <vector>

class BookBuilder {
private:
  using Book = OrderBook<8, 256>;

  Consumer consumer_;
  std::atomic<bool> stop_;
  uint64_t msg_count_{0};
  HashMap<instrument_t, Book *> instrument_map_;
  bufpool::BufferPool &bp_ref_;
  std::vector<std::unique_ptr<Book>> owned_books_; // RAII

  Book *get_or_init_book(instrument_t instr) noexcept {
    Book *book = instrument_map_.find(instr);
    if (book == nullptr) [[unlikely]] {
      auto owned = std::make_unique<Book>(bp_ref_);
      book = owned.get();
      owned_books_.push_back(std::move(owned));
      [[maybe_unused]] bool inserted = instrument_map_.insert(instr, book);
      assert(inserted);
    }
    return book;
  }

  void add_order_handler(const MboEvent &mbo) noexcept {
    Book *book = get_or_init_book(mbo.instrument);
    auto exp = book->add_order(mbo.oid, mbo.qty, mbo.price, mbo.side);
    if (!exp.has_value()) [[unlikely]] {
      if (exp.error() == Book::AddOrderError::DuplicateOrderId) {
        LOG_WARN("add dropped, duplicate oid: instrument={} oid={}",
                 mbo.instrument, mbo.oid);
      } else {
        LOG_WARN("add dropped, levels full: instrument={} oid={} price={}",
                 mbo.instrument, mbo.oid, mbo.price);
      }
    }
  }

  void cancel_order_handler(const MboEvent &mbo) noexcept {
    Book *book = instrument_map_.find(mbo.instrument);
    if (book == nullptr || !book->reduce_order(mbo.oid, mbo.qty)) [[unlikely]] {
      LOG_DEBUG("cancel dropped, unknown oid: instrument={} oid={}",
                mbo.instrument, mbo.oid);
    }
  }

  void execute_order_handler(const MboEvent &mbo) noexcept {
    Book *book = instrument_map_.find(mbo.instrument);
    if (book == nullptr || !book->execute_order(mbo.oid, mbo.qty))
        [[unlikely]] {
      LOG_DEBUG("execute dropped, unknown oid: instrument={} oid={}",
                mbo.instrument, mbo.oid);
    }
  }

  void delete_order_handler(const MboEvent &mbo) noexcept {
    Book *book = instrument_map_.find(mbo.instrument);
    if (book == nullptr || !book->cancel_order(mbo.oid)) [[unlikely]] {
      LOG_DEBUG("delete dropped, unknown oid: instrument={} oid={}",
                mbo.instrument, mbo.oid);
    }
  }

  void replace_order_handler(const MboEvent &mbo) noexcept {
    Book *book = instrument_map_.find(mbo.instrument);
    if (book == nullptr || !book->cancel_order(mbo.orig_oid)) [[unlikely]] {
      LOG_DEBUG("replace, unknown orig oid: instrument={} oid={}",
                mbo.instrument, mbo.orig_oid);
    }
    add_order_handler(mbo);
  }

public:
  BookBuilder(Consumer consumer, bufpool::BufferPool &bp_ref)
      : consumer_(consumer), stop_(false), instrument_map_(1 << 13),
        bp_ref_(bp_ref) {
    owned_books_.reserve(8192);
  }

  void run(uint32_t core, StartGate &gate) {
    std::string tname = "bbuilder_" + std::to_string(core);
    if (!pin_and_prioritize(pthread_self(), core)) {
      LOG_ERROR("Failed to pin thread at core: {}", core);
      goto failed;
    }

    if (pthread_setname_np(pthread_self(), tname.c_str()) != 0) {
      LOG_ERROR("Failed to set thread name {} for thread: {}", tname, core);
      goto failed;
    }

    bufpool::thread_init();
    logger::thread_init();

    gate.ready.count_down();
    gate.go.wait();

    while (!stop_.load(std::memory_order_relaxed)) {
      std::optional<MboEvent> mbo_opt;
      while ((mbo_opt = consumer_.pop()) == std::nullopt) {
        if (stop_.load(std::memory_order_relaxed)) {
          return;
        }
      }

      msg_count_++;
      const auto &mbo = mbo_opt.value();
      switch (mbo.etype) {
      case EventType::ADDED:
        add_order_handler(mbo);
        break;
      case EventType::EXECUTED:
        execute_order_handler(mbo);
        break;
      case EventType::REPLACED:
        replace_order_handler(mbo);
        break;
      case EventType::CANCELLED:
        cancel_order_handler(mbo);
        break;
      case EventType::DELETED:
        delete_order_handler(mbo);
        break;
      default:
        break;
      }
    }

    return;

  failed:
    gate.failed.fetch_add(1, std::memory_order_relaxed);
    gate.ready.count_down();
  }

  void stop() { stop_.store(true, std::memory_order_relaxed); }

  [[nodiscard]] uint64_t message_count() const { return msg_count_; }
};