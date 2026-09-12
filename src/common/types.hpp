#pragma once

#include <cstdint>

using instrument_t = uint32_t;
using timestamp_t = uint64_t;
using oid_t = uint64_t;

enum class Side : uint8_t { BID, ASK };

enum class EventType : uint8_t {
  ADDED = 0,
  EXECUTED,
  REPLACED,
  CANCELLED,
  DELETED
};

struct MboEvent {
  EventType etype{};
  Side side{};
  bool printable{};
  instrument_t instrument{};
  timestamp_t timestamp{};
  oid_t oid{};
  oid_t orig_oid{};
  uint32_t price{};
  uint32_t qty{};
};
