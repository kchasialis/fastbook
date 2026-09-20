#pragma once

#include "event_queue.hpp"
#include "hash_map.hpp"
#include "order_book.hpp"
#include "types.hpp"
#include <atomic>
#include <cassert>
#include <memory>
#include <vector>

class BookBuilder {
private:
  Consumer consumer_;
  std::atomic<bool> stop_;
  uint64_t msg_count_{0};
  HashMap<instrument_t, OrderBook *> instrument_map_;
  bufpool::BufferPool &bp_ref_;
  std::vector<std::unique_ptr<OrderBook>> owned_books_; // RAII

  OrderBook *get_or_init_book(instrument_t instr, uint32_t price) noexcept {
    OrderBook *book = instrument_map_.find(instr);
    if (book == nullptr) [[unlikely]] {
      auto owned = std::make_unique<OrderBook>(bp_ref_);
      book = owned.get();
      // need to fix that with actual configs fetched offline.
      uint32_t base = price > 100 ? price - 100 : 0;
      book->reset(base, 1, 1024);
      owned_books_.push_back(std::move(owned));
      [[maybe_unused]] bool inserted = instrument_map_.insert(instr, book);
      assert(inserted);
    }
    return book;
  }

  void add_order_handler(const MboEvent &mbo) noexcept {
    OrderBook *book = get_or_init_book(mbo.instrument, mbo.price);
    auto exp = book->add_order(mbo.oid, mbo.qty, mbo.price, mbo.side);
    if (!exp.has_value()) [[unlikely]] {
      // TODO(kostas): Add proper handling here.
      std::terminate();
      // switch (exp.error()) {
      // case OrderBook::AddOrderError::DuplicateOrderId: {
      // }
      // case OrderBook::AddOrderError::PriceOutOfWindow: {
      //   std::terminate();
      // }
    }
  }

  void cancel_order_handler(const MboEvent &mbo) noexcept {
    OrderBook *book = instrument_map_.find(mbo.instrument);
    if (book == nullptr) [[unlikely]] {
      return;
    }
    book->reduce_order(mbo.oid, mbo.qty);
  }

  void execute_order_handler(const MboEvent &mbo) noexcept {
    OrderBook *book = instrument_map_.find(mbo.instrument);
    if (book == nullptr) [[unlikely]] {
      return;
    }
    book->execute_order(mbo.oid, mbo.qty);
  }

  void delete_order_handler(const MboEvent &mbo) noexcept {
    OrderBook *book = instrument_map_.find(mbo.instrument);
    if (book == nullptr) [[unlikely]] {
      return;
    }
    book->cancel_order(mbo.oid);
  }

  void replace_order_handler(const MboEvent &mbo) noexcept {
    OrderBook *book = instrument_map_.find(mbo.instrument);
    if (book == nullptr) [[unlikely]] {
      return;
    }
    book->cancel_order(mbo.orig_oid);
    book->add_order(mbo.oid, mbo.qty, mbo.price, mbo.side);
  }

public:
  BookBuilder(Consumer consumer, bufpool::BufferPool &bp_ref)
      : consumer_(consumer), stop_(false), instrument_map_(1 << 13),
        bp_ref_(bp_ref) {
    owned_books_.reserve(8192);
  }

  void run(uint32_t core) {
    if (!pin_and_prioritize(pthread_self(), core)) {
      std::cerr << "[DEBUG] BookBuilder::run(): Failed to pin thread at core: "
                << core << std::endl;
      return;
    }

    std::string tname = "bbuilder_" + std::to_string(core);
    if (!pthread_setname_np(pthread_self(), tname.c_str()) != 0) {
      std::cerr << "[DEBUG] BookBuilder::run(): Failed to set thread name"
                << std::endl;
      return;
    }

    bufpool::thread_init();

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
  }

  void stop() { stop_.store(true, std::memory_order_relaxed); }

  uint64_t message_count() const { return msg_count_; }
};