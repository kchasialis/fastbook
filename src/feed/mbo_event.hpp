#pragma once

#include <cstdint>

using instrument_t = uint32_t;
using timestamp_t = uint64_t;
using oid_t = uint64_t;

enum class EventType : uint8_t { ADDED, EXECUTED, CANCELLED, DELETED };

enum class Side : uint8_t { ASK, BID };

struct MboEvent {
  EventType etype;
  Side side;
  instrument_t instrument;
  timestamp_t timestamp;
  oid_t oid;
  uint32_t price;
  uint32_t qty;
};