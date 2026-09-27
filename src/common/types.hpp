#pragma once

#include <cctype>
#include <cstdint>
#include <optional>
#include <string_view>

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

enum class Exchange : uint8_t { NASDAQ, CME, CBOE, NYSE, EUREX };

inline std::optional<Exchange> str_to_exchange(std::string_view ex_sv) {
  auto iequals = [](std::string_view a, std::string_view b) -> bool {
    if (a.size() != b.size()) {
      return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
      if (std::toupper(a[i]) != std::toupper(b[i])) {
        return false;
      }
    }
    return true;
  };

  if (iequals(ex_sv, "nasdaq")) {
    return Exchange::NASDAQ;
  } else if (iequals(ex_sv, "cme")) {
    return Exchange::CME;
  } else if (iequals(ex_sv, "cboe")) {
    return Exchange::CBOE;
  } else if (iequals(ex_sv, "nyse")) {
    return Exchange::NYSE;
  } else if (iequals(ex_sv, "eurex")) {
    return Exchange::EUREX;
  }

  return std::nullopt;
}
