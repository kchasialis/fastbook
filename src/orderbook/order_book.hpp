#pragma once

#include <cstdint>

struct Order {
  uint64_t order_num;
  uint32_t shares;
  uint32_t price;
  Order *next;
  Order *prev;
};
