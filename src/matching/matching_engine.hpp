#pragma once

#include "order_book.hpp"
#include <algorithm>
#include <cstdint>
#include <span>

struct Fill {
  uint64_t agg_order_id;
  uint64_t pass_order_id;
  uint32_t price;
  uint32_t qty;
};

class MatchingEngine {
private:
  OrderBook &book_;

  void match_order(uint64_t agg_order_id, uint32_t best_price, uint32_t qty,
                   uint32_t &matched_qty, uint32_t &total_matched,
                   std::span<Fill> fills) noexcept {
    Order *order = book_.get_first_order(best_price);

    uint32_t executed_shares = std::min(qty - matched_qty, order->shares);

    matched_qty += executed_shares;
    auto &fill = fills[total_matched++];
    fill.agg_order_id = agg_order_id;
    fill.pass_order_id = order->order_num;
    fill.price = order->price;
    fill.qty = executed_shares;
    book_.execute_order(order->order_num, executed_shares);
  }

public:
  MatchingEngine(OrderBook &book) : book_(book) {}

  size_t match(uint64_t agg_order_id, Side side, uint32_t price, uint32_t qty,
               std::span<Fill> fills) noexcept {
    uint32_t total_matched = 0;
    uint32_t matched_qty = 0;
    while (matched_qty != qty) {
      uint32_t best_price =
          book_.best_price(side == Side::BID ? Side::ASK : Side::BID);
      if (best_price == 0) {
        break;
      }
      if ((side == Side::BID && price >= best_price) ||
          (side == Side::ASK && price <= best_price)) {
        match_order(agg_order_id, best_price, qty, matched_qty, total_matched,
                    fills);
      } else {
        break;
      }
    }

    return total_matched;
  }
};