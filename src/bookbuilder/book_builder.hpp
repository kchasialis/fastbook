#pragma once

#include "feed_handler.hpp"
#include "itch_messages.hpp"
#include "matching_engine.hpp"
#include "order_book.hpp"
#include "spsc_queue.hpp"
#include <array>
#include <atomic>

class BookBuilder {
private:
  Queue::SPSCConsumer &consumer_;
  OrderBook &book_;
  MatchingEngine &engine_;
  std::atomic<bool> stop_;
  std::atomic<uint64_t> msg_count_{0};
  bool initialized_ = false;

  void init_book(uint32_t price) {
    if (!initialized_) [[unlikely]] {
      uint32_t base = price > 100 ? price - 100 : 0;
      book_.reset(base, 1, 1024);
      initialized_ = true;
    }
  }

  void add_order_handler(const Message &msg) {
    init_book(msg.add_order.price);
    Side side = msg.add_order.side == 'B' ? Side::BID : Side::ASK;
    book_.add_order(msg.add_order.order_ref_num, msg.add_order.shares,
                    msg.add_order.price, side);
    std::array<Fill, 16> fills;
    engine_.match(msg.add_order.order_ref_num, side, msg.add_order.price,
                  msg.add_order.shares, std::span<Fill>{fills});
  }

  void add_order_mpid_handler(const Message &msg) {
    init_book(msg.add_order_mpid.price);
    Side side = msg.add_order_mpid.side == 'B' ? Side::BID : Side::ASK;
    book_.add_order(msg.add_order_mpid.order_ref_num, msg.add_order_mpid.shares,
                    msg.add_order_mpid.price, side);
    std::array<Fill, 16> fills;
    engine_.match(msg.add_order_mpid.order_ref_num, side,
                  msg.add_order_mpid.price, msg.add_order_mpid.shares,
                  std::span<Fill>{fills});
  }

  void cancel_order_handler(const Message &msg) {
    book_.reduce_order(msg.order_cancel.order_ref_num,
                       msg.order_cancel.cancelled_shares);
  }

  void delete_order_handler(const Message &msg) {
    book_.cancel_order(msg.order_delete.order_ref_num);
  }

  void execute_order_handler(const Message &msg) {
    book_.execute_order(msg.order_executed.order_ref_num,
                        msg.order_executed.executed_shares);
  }

  void execute_order_priced_handler(const Message &msg) {
    book_.execute_order(msg.order_executed_price.order_ref_num,
                        msg.order_executed_price.executed_shares);
  }

  void replace_order_handler(const Message &msg) {
    auto *order = book_.get_order_by_oid(msg.order_replace.orig_order_ref_num);
    assert(order);
    auto side = order->side;
    book_.cancel_order(msg.order_replace.orig_order_ref_num);
    book_.add_order(msg.order_replace.new_order_ref_num,
                    msg.order_replace.shares, msg.order_replace.price, side);
  }

public:
  BookBuilder(Queue::SPSCConsumer &consumer, OrderBook &book,
              MatchingEngine &engine)
      : consumer_(consumer), book_(book), engine_(engine), stop_(false) {}

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
      case MsgType::OrderExecuted:
        execute_order_handler(msg);
        break;
      case MsgType::OrderExecutedPrice:
        execute_order_priced_handler(msg);
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
  uint64_t message_count() const { return msg_count_.load(std::memory_order_relaxed); }
};