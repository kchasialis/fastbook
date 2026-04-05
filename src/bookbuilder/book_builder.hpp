#pragma once

#include "itch_messages.hpp"
#include "order_book.hpp"
#include "spsc_queue.hpp"
#include <atomic>

using Queue = SPSCQueue<Message, 64>;
class BookBuilder {
private:
  Queue::SPSCConsumer &consumer_;
  OrderBook &book_;
  std::atomic<bool> stop_;

  void add_order_handler(const Message &msg) {
    book_.add_order(msg.add_order.order_ref_num, msg.add_order.shares,
                    msg.add_order.price,
                    msg.add_order.side == 'B' ? Side::BID : Side::ASK);
  }

  void add_order_mpid_handler(const Message &msg) {
    book_.add_order(msg.add_order_mpid.order_ref_num, msg.add_order_mpid.shares,
                    msg.add_order_mpid.price,
                    msg.add_order_mpid.side == 'B' ? Side::BID : Side::ASK);
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
  BookBuilder(Queue::SPSCConsumer &consumer, OrderBook &book)
      : consumer_(consumer), book_(book), stop_(false) {}

  void consume() {
    while (!stop_.load(std::memory_order_acquire)) {
      std::optional<Message> message;
      while ((message = consumer_.pop()) == std::nullopt) {
        if (stop_.load(std::memory_order_acquire)) {
          return;
        }
      }

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
};