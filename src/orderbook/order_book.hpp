#pragma once

#include "buffer_pool.hpp"
#include "hash_map.hpp"
#include "object_pool.hpp"
#include "types.hpp"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>

struct Order {
  uint64_t order_num;
  uint32_t shares;
  uint32_t price;
  Side side;
  Order *next;
  Order *prev;

  struct Reset {
    static void operator()(Order &o) noexcept { std::memset(&o, 0, sizeof(o)); }
  };
};

struct PriceLevel {
  Order *head;
  Order *tail;
  size_t total_qty;
  uint32_t price;
};

class OrderBook {
public:
  enum class AddOrderError { DuplicateOrderId, PriceOutOfWindow };

private:
  static constexpr uint32_t MAX_ORDERS = 2 << 12; // 4096

  std::unique_ptr<PriceLevel[]> levels_;
  uint32_t base_price_;
  uint32_t tick_size_;
  uint32_t window_size_;
  uint32_t best_bid_slot_;
  uint32_t best_ask_slot_;
  uint32_t max_orders_;
  ObjectPool<Order> orders_op_;
  HashMap<oid_t, Order *> orders_;
  BufferPool &allocator_;

  bool grow() noexcept {
    size_t n = max_orders_;
    size_t alloc_bytes = n * sizeof(Order);
    void *mem = allocator_.alloc(alloc_bytes);
    if (!mem) {
      return false;
    }

    if (!orders_op_.add_chunk(
            std::span<std::byte>(reinterpret_cast<std::byte *>(mem),
                                 alloc_bytes),
            n)) {
      return false;
    }

    if (!orders_.rehash(max_orders_ + n)) {
      return false;
    }
    max_orders_ += n;

    return true;
  }

  bool add_to_price_queue(Order *order) noexcept {
    uint32_t price = order->price;
    uint32_t shares = order->shares;

    if (price < base_price_) [[unlikely]] {
      return false;
    }

    uint32_t price_slot = ((price - base_price_) / tick_size_) % window_size_;
    PriceLevel &level = levels_[price_slot];
    if (level.price != price || level.head == nullptr) {
      level.head = order;
      level.tail = order;
      level.price = price;
      level.total_qty = 0;
    } else {
      level.tail->next = order;
      order->prev = level.tail;
      level.tail = order;
    }

    switch (order->side) {
    case Side::ASK:
      if (best_ask_slot_ == window_size_) {
        best_ask_slot_ = price_slot;
      } else {
        if (levels_[price_slot].price < levels_[best_ask_slot_].price) {
          best_ask_slot_ = price_slot;
        }
      }
      break;
    case Side::BID:
      if (best_bid_slot_ == window_size_) {
        best_bid_slot_ = price_slot;
      } else {
        if (levels_[best_bid_slot_].price < levels_[price_slot].price) {
          best_bid_slot_ = price_slot;
        }
      }
      break;
    default:
      break;
    }

    level.total_qty += shares;

    return true;
  }

  void update_best_bid_ask_slots(uint32_t price_slot) noexcept {
    if (price_slot == best_ask_slot_) {
      uint32_t best_slot = window_size_;
      for (uint32_t i = 1; i < window_size_; i++) {
        uint32_t slot = (price_slot + i) % window_size_;
        if (levels_[slot].head != nullptr &&
            levels_[slot].head->side == Side::ASK &&
            (best_slot == window_size_ ||
             levels_[slot].price < levels_[best_slot].price)) {
          best_slot = slot;
        }
      }
      best_ask_slot_ = best_slot;
    }

    if (price_slot == best_bid_slot_) {
      uint32_t best_slot = window_size_;
      for (uint32_t i = 1; i < window_size_; i++) {
        uint32_t slot = (price_slot + i) % window_size_;
        if (levels_[slot].head != nullptr &&
            levels_[slot].head->side == Side::BID &&
            (best_slot == window_size_ ||
             levels_[slot].price > levels_[best_slot].price)) {
          best_slot = slot;
        }
      }
      best_bid_slot_ = best_slot;
    }
  }

  bool remove_from_price_queue(Order *order) noexcept {
    uint32_t price = order->price;
    uint32_t shares = order->shares;

    if (price < base_price_) [[unlikely]] {
      return false;
    }

    uint32_t price_slot = ((price - base_price_) / tick_size_) % window_size_;
    PriceLevel &level = levels_[price_slot];
    if (level.price != price) {
      return false;
    }

    level.total_qty -= shares;

    if (level.head == level.tail) {
      update_best_bid_ask_slots(price_slot);
      level.head = level.tail = nullptr;
      return true;
    }
    if (level.head == order) {
      order->next->prev = nullptr;
      level.head = level.head->next;
      return true;
    }
    if (level.tail == order) {
      order->prev->next = nullptr;
      level.tail = order->prev;
      return true;
    }

    order->prev->next = order->next;
    order->next->prev = order->prev;

    return true;
  }

public:
  OrderBook(BufferPool &allocator)
      : max_orders_(MAX_ORDERS), orders_op_(max_orders_), orders_(max_orders_),
        allocator_(allocator) {}

  void reset(uint32_t base_price, uint32_t tick_size, uint32_t window_size) {
    base_price_ = base_price;
    tick_size_ = tick_size;
    window_size_ = window_size;
    levels_ = std::make_unique<PriceLevel[]>(window_size);
    best_bid_slot_ = window_size;
    best_ask_slot_ = window_size;
  }

  [[nodiscard]] std::expected<void, AddOrderError>
  add_order(oid_t oid, uint32_t shares, uint32_t price, Side side) noexcept {
    if (orders_.find(oid) != nullptr) [[unlikely]] {
      return std::unexpected(AddOrderError::DuplicateOrderId);
    }

    Order *new_order = orders_op_.get();
    if (new_order == nullptr) [[unlikely]] {
      /* Probably not the best way to handle this.
        I assume real systems would just alarm and exit.
        For academic purposes only.    */
      if (!grow()) [[unlikely]] {
        // System is out of memory, terminate is the only viable option here.
        std::terminate();
      }
      new_order = orders_op_.get();
      assert(new_order);
    }

    new_order->order_num = oid;
    new_order->price = price;
    new_order->shares = shares;
    new_order->side = side;
    new_order->next = nullptr;
    new_order->prev = nullptr;

    if (!add_to_price_queue(new_order)) {
      orders_op_.restore(new_order);
      return std::unexpected(AddOrderError::PriceOutOfWindow);
    }

    orders_.insert(oid, new_order);

    return {};
  }

  bool cancel_order(oid_t oid) noexcept {
    Order *order;
    if ((order = orders_.find(oid)) == nullptr) [[unlikely]] {
      return false;
    }

    if (!remove_from_price_queue(order)) {
      return false;
    }

    assert(orders_.erase(oid));

    orders_op_.restore(order);
    return true;
  }

  bool execute_order(oid_t oid, uint32_t executed_shares) noexcept {
    Order *order;
    if ((order = orders_.find(oid)) == nullptr) [[unlikely]] {
      return false;
    }

    assert(executed_shares <= order->shares);

    uint32_t price_slot =
        ((order->price - base_price_) / tick_size_) % window_size_;
    PriceLevel &level = levels_[price_slot];

    order->shares -= executed_shares;
    level.total_qty -= executed_shares;
    if (order->shares == 0) {
      cancel_order(oid);
    }

    return true;
  }

  bool reduce_order(oid_t oid, uint32_t cancelled_shares) noexcept {
    return execute_order(oid, cancelled_shares);
  }

  Order *get_order_by_oid(oid_t oid) {
    Order *order;
    if ((order = orders_.find(oid)) == nullptr) [[unlikely]] {
      return nullptr;
    }

    return order;
  }

  Order *get_first_order(uint32_t price) const noexcept {
    uint32_t price_slot = ((price - base_price_) / tick_size_) % window_size_;
    PriceLevel &level = levels_[price_slot];

    assert(level.head);

    return level.head;
  }

  uint32_t best_price(Side side) const noexcept {
    switch (side) {
    case Side::ASK:
      return best_ask_slot_ != window_size_ ? levels_[best_ask_slot_].price : 0;
    case Side::BID:
      return best_bid_slot_ != window_size_ ? levels_[best_bid_slot_].price : 0;
    default:
      return 0;
    }
  }
};