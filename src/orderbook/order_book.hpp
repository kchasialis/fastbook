#pragma once

#include "buffer_pool.hpp"
#include "hash_map.hpp"
#include "object_pool.hpp"
#include "two_tier_levels.hpp"
#include "types.hpp"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>

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

template <size_t K, size_t ColdCap> class OrderBook {
public:
  enum class AddOrderError { DuplicateOrderId, LevelsFull };

private:
  static constexpr uint32_t MAX_ORDERS = 1 << 12; // 4096

  TwoTierLevels<Side::BID, K, ColdCap> bids_;
  TwoTierLevels<Side::ASK, K, ColdCap> asks_;
  uint32_t max_orders_{};
  ObjectPool<Order> orders_op_;
  HashMap<oid_t, Order *> orders_;
  bufpool::BufferPool &allocator_;

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

  LevelData *find_level(Side side, uint32_t price) noexcept {
    return side == Side::BID ? bids_.find(price) : asks_.find(price);
  }

  LevelData *find_or_insert_level(Side side, uint32_t price) noexcept {
    return side == Side::BID ? bids_.find_or_insert(price)
                             : asks_.find_or_insert(price);
  }

  void erase_level(Side side, uint32_t price) noexcept {
    if (side == Side::BID) {
      bids_.erase(price);
    } else {
      asks_.erase(price);
    }
  }

  bool add_to_price_queue(Order *order) noexcept {
    LevelData *level = find_or_insert_level(order->side, order->price);
    if (level == nullptr) [[unlikely]] {
      return false;
    }

    if (level->head == nullptr) {
      level->head = order;
    } else {
      level->tail->next = order;
      order->prev = level->tail;
    }
    level->tail = order;
    level->total_qty += order->shares;

    return true;
  }

  bool remove_from_price_queue(Order *order) noexcept {
    LevelData *level = find_level(order->side, order->price);
    if (level == nullptr) [[unlikely]] {
      return false;
    }

    if (order->prev != nullptr) {
      order->prev->next = order->next;
    } else {
      level->head = order->next;
    }
    if (order->next != nullptr) {
      order->next->prev = order->prev;
    } else {
      level->tail = order->prev;
    }
    level->total_qty -= order->shares;

    if (level->head == nullptr) {
      erase_level(order->side, order->price);
    }
    return true;
  }

public:
  OrderBook(bufpool::BufferPool &allocator)
      : max_orders_(MAX_ORDERS), orders_op_(max_orders_), orders_(max_orders_),
        allocator_(allocator) {}

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
      return std::unexpected(AddOrderError::LevelsFull);
    }

    orders_.insert(oid, new_order);

    return {};
  }

  bool cancel_order(oid_t oid) noexcept {
    Order *order = orders_.find(oid);
    if (order == nullptr) [[unlikely]] {
      return false;
    }

    if (!remove_from_price_queue(order)) {
      return false;
    }

    [[maybe_unused]] bool erased = orders_.erase(oid);
    assert(erased);

    orders_op_.restore(order);
    return true;
  }

  bool execute_order(oid_t oid, uint32_t executed_shares) noexcept {
    Order *order = orders_.find(oid);
    if (order == nullptr) [[unlikely]] {
      return false;
    }

    assert(executed_shares <= order->shares);

    if (executed_shares >= order->shares) {
      return cancel_order(oid);
    }

    LevelData *level = find_level(order->side, order->price);
    assert(level != nullptr);

    order->shares -= executed_shares;
    level->total_qty -= executed_shares;

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

  [[nodiscard]] Order *get_first_order(Side side, uint32_t price) noexcept {
    LevelData *level = find_level(side, price);
    return level != nullptr ? level->head : nullptr;
  }

  [[nodiscard]] uint32_t best_price(Side side) const noexcept {
    return side == Side::BID ? bids_.best_price() : asks_.best_price();
  }
};