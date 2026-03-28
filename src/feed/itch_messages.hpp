#pragma once

#include <cstdint>

namespace fastbook::feed {

struct AddOrder {
  uint16_t stock_locate;
  uint16_t tracking_number;
  uint64_t timestamp;
  uint64_t order_ref_num;
  char side;
  uint32_t shares;
  char stock[8];
  uint32_t price;
};

struct AddOrderMPID {
  uint16_t stock_locate;
  uint16_t tracking_number;
  uint64_t timestamp;
  uint64_t order_ref_num;
  char side;
  uint32_t shares;
  char stock[8];
  uint32_t price;
  char attribution[4];
};

struct OrderExecuted {
  uint16_t stock_locate;
  uint16_t tracking_number;
  uint64_t timestamp;
  uint64_t order_ref_num;
  uint32_t executed_shares;
  uint64_t match_number;
};

struct OrderExecutedWithPrice {
  uint16_t stock_locate;
  uint16_t tracking_number;
  uint64_t timestamp;
  uint64_t order_ref_num;
  uint32_t executed_shares;
  uint64_t match_number;
  char printable;
  uint32_t execution_price;
};

struct OrderCancel {
  uint16_t stock_locate;
  uint16_t tracking_number;
  uint64_t timestamp;
  uint64_t order_ref_num;
  uint32_t cancelled_shares;
};

struct OrderDelete {
  uint16_t stock_locate;
  uint16_t tracking_number;
  uint64_t timestamp;
  uint64_t order_ref_num;
};

struct OrderReplace {
  uint16_t stock_locate;
  uint16_t tracking_number;
  uint64_t timestamp;
  uint64_t orig_order_ref_num;
  uint64_t new_order_ref_num;
  uint32_t shares;
  uint32_t price;
};

enum class MsgType : char {
  AddOrder = 'A',
  AddOrderMPID = 'F',
  OrderExecuted = 'E',
  OrderExecutedPrice = 'C',
  OrderCancel = 'X',
  OrderDelete = 'D',
  OrderReplace = 'U',
  Unknown = '\0',
};

struct Message {
  MsgType type = MsgType::Unknown;
  union {
    AddOrder add_order;
    AddOrderMPID add_order_mpid;
    OrderExecuted order_executed;
    OrderExecutedWithPrice order_executed_price;
    OrderCancel order_cancel;
    OrderDelete order_delete;
    OrderReplace order_replace;
  };
};

} // namespace fastbook::feed