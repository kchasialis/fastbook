#pragma once

#include "feed_handler.hpp"
#include "hash_map.hpp"
#include "itch_messages.hpp"
#include "matching_engine.hpp"
#include "order_book.hpp"
#include "spsc_queue.hpp"
#include <array>
#include <atomic>
#include <cassert>
#include <memory>
#include <vector>

class BookBuilder {
private:
  Queue::SPSCConsumer &consumer_;
  MatchingEngine &engine_;
  std::atomic<bool> stop_;
  std::atomic<uint64_t> msg_count_{0};
  HashMap<uint16_t, OrderBook *> symbol_map_;
  HashMap<uint64_t, OrderBook *> order_map_;
  std::vector<std::unique_ptr<OrderBook>> owned_books_;

  OrderBook *get_or_init_book(uint16_t stock_locate, uint32_t price) noexcept {
    OrderBook *book = symbol_map_.find(stock_locate);
    if (book == nullptr) [[unlikely]] {
      auto owned = std::make_unique<OrderBook>();
      book = owned.get();
      uint32_t base = price > 100 ? price - 100 : 0;
      book->reset(base, 1, 1024);
      owned_books_.push_back(std::move(owned));
      symbol_map_.insert(stock_locate, book);
    }
    return book;
  }

  void add_order_handler(const Message &msg) noexcept {
    OrderBook *book =
        get_or_init_book(msg.add_order.stock_locate, msg.add_order.price);
    Side side = msg.add_order.side == 'B' ? Side::BID : Side::ASK;
    book->add_order(msg.add_order.order_ref_num, msg.add_order.shares,
                    msg.add_order.price, side);
    order_map_.insert(msg.add_order.order_ref_num, book);

    std::array<Fill, 16> fills;
    engine_.match(*book, msg.add_order.order_ref_num, side, msg.add_order.price,
                  msg.add_order.shares, std::span<Fill>{fills});
  }

  void add_order_mpid_handler(const Message &msg) noexcept {
    OrderBook *book = get_or_init_book(msg.add_order_mpid.stock_locate,
                                       msg.add_order_mpid.price);
    Side side = msg.add_order_mpid.side == 'B' ? Side::BID : Side::ASK;
    book->add_order(msg.add_order_mpid.order_ref_num, msg.add_order_mpid.shares,
                    msg.add_order_mpid.price, side);
    order_map_.insert(msg.add_order_mpid.order_ref_num, book);

    std::array<Fill, 16> fills;
    engine_.match(*book, msg.add_order_mpid.order_ref_num, side,
                  msg.add_order_mpid.price, msg.add_order_mpid.shares,
                  std::span<Fill>{fills});
  }

  void cancel_order_handler(const Message &msg) noexcept {
    OrderBook *book = order_map_.find(msg.order_cancel.order_ref_num);
    if (book == nullptr) [[unlikely]]
      return;
    Order *order = book->get_order_by_oid(msg.order_cancel.order_ref_num);
    if (order == nullptr) [[unlikely]]
      return;
    if (order->shares == msg.order_cancel.cancelled_shares) {
      order_map_.erase(msg.order_cancel.order_ref_num);
    }
    book->reduce_order(msg.order_cancel.order_ref_num,
                       msg.order_cancel.cancelled_shares);
  }

  void delete_order_handler(const Message &msg) noexcept {
    OrderBook *book = order_map_.find(msg.order_delete.order_ref_num);
    if (book == nullptr) [[unlikely]]
      return;
    order_map_.erase(msg.order_delete.order_ref_num);
    book->cancel_order(msg.order_delete.order_ref_num);
  }

  void replace_order_handler(const Message &msg) noexcept {
    OrderBook *book = order_map_.find(msg.order_replace.orig_order_ref_num);
    if (book == nullptr) [[unlikely]]
      return;
    Order *order = book->get_order_by_oid(msg.order_replace.orig_order_ref_num);
    assert(order);
    Side side = order->side;

    order_map_.erase(msg.order_replace.orig_order_ref_num);
    book->cancel_order(msg.order_replace.orig_order_ref_num);

    book->add_order(msg.order_replace.new_order_ref_num,
                    msg.order_replace.shares, msg.order_replace.price, side);
    order_map_.insert(msg.order_replace.new_order_ref_num, book);

    std::array<Fill, 16> fills;
    engine_.match(*book, msg.order_replace.new_order_ref_num, side,
                  msg.order_replace.price, msg.order_replace.shares,
                  std::span<Fill>{fills});
  }

public:
  BookBuilder(Queue::SPSCConsumer &consumer, MatchingEngine &engine)
      : consumer_(consumer), engine_(engine), stop_(false),
        symbol_map_(1 << 13), order_map_(1 << 17) {
    owned_books_.reserve(8192);
  }

  void run() {
    while (!stop_.load(std::memory_order_acquire)) {
      std::optional<Message> message;
      while ((message = consumer_.pop()) == std::nullopt) {
        if (stop_.load(std::memory_order_acquire)) {
          return;
        }
      }

      msg_count_.fetch_add(1, std::memory_order_relaxed);
      const auto &msg = message.value();
      switch (msg.type) {
      case MsgType::AddOrder:
        add_order_handler(msg);
        break;
      case MsgType::AddOrderMPID:
        add_order_mpid_handler(msg);
        break;
      case MsgType::OrderCancel:
        cancel_order_handler(msg);
        break;
      case MsgType::OrderDelete:
        delete_order_handler(msg);
        break;
      case MsgType::OrderReplace:
        replace_order_handler(msg);
        break;
      default:
        break;
      }
    }
  }

  void stop() { stop_.store(true, std::memory_order_release); }
  uint64_t message_count() const {
    return msg_count_.load(std::memory_order_relaxed);
  }
};