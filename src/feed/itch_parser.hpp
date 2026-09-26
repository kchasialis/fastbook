#pragma once

#include "buf_reader.hpp"
#include "types.hpp"
#include <array>
#include <cassert>
#include <span>

template <class S>
concept EventSink = requires(S s, const MboEvent &e) {
  { s.on_event(e) } -> std::same_as<void>;
};

class ITCHParser {
private:
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

  struct ItchMessage {
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

  static ItchMessage parse_msg(BufReader &reader) noexcept {
    ItchMessage msg;
    char msg_type = static_cast<char>(reader.read_num<uint8_t>());
    switch (msg_type) {
    case 'A': {
      parse_add_order_msg(reader, msg);
      break;
    }
    case 'F': {
      parse_add_order_mpid_msg(reader, msg);
      break;
    }
    case 'E': {
      parse_order_executed_msg(reader, msg);
      break;
    }
    case 'C': {
      parse_order_executed_price_msg(reader, msg);
      break;
    }
    case 'X': {
      parse_order_cancel_msg(reader, msg);
      break;
    }
    case 'D': {
      parse_order_delete_msg(reader, msg);
      break;
    }
    case 'U': {
      parse_order_replace_msg(reader, msg);
      break;
    }
    default: {
      break;
    }
    }

    return msg;
  }

  static void parse_add_order_msg(BufReader &reader,
                                  ItchMessage &msg) noexcept {
    msg.type = MsgType::AddOrder;
    msg.add_order.stock_locate = reader.read_num<uint16_t>();
    msg.add_order.tracking_number = reader.read_num<uint16_t>();
    msg.add_order.timestamp = reader.read_timestamp();
    msg.add_order.order_ref_num = reader.read_num<uint64_t>();
    msg.add_order.side = static_cast<char>(reader.read_num<uint8_t>());
    msg.add_order.shares = reader.read_num<uint32_t>();
    reader.read_chars(msg.add_order.stock);
    msg.add_order.price = reader.read_num<uint32_t>();
  }

  static void parse_add_order_mpid_msg(BufReader &reader,
                                       ItchMessage &msg) noexcept {
    msg.type = MsgType::AddOrderMPID;
    msg.add_order_mpid.stock_locate = reader.read_num<uint16_t>();
    msg.add_order_mpid.tracking_number = reader.read_num<uint16_t>();
    msg.add_order_mpid.timestamp = reader.read_timestamp();
    msg.add_order_mpid.order_ref_num = reader.read_num<uint64_t>();
    msg.add_order_mpid.side = static_cast<char>(reader.read_num<uint8_t>());
    msg.add_order_mpid.shares = reader.read_num<uint32_t>();
    reader.read_chars(msg.add_order_mpid.stock);
    msg.add_order_mpid.price = reader.read_num<uint32_t>();
    reader.read_chars(msg.add_order_mpid.attribution);
  }

  static void parse_order_executed_msg(BufReader &reader,
                                       ItchMessage &msg) noexcept {
    msg.type = MsgType::OrderExecuted;
    msg.order_executed.stock_locate = reader.read_num<uint16_t>();
    msg.order_executed.tracking_number = reader.read_num<uint16_t>();
    msg.order_executed.timestamp = reader.read_timestamp();
    msg.order_executed.order_ref_num = reader.read_num<uint64_t>();
    msg.order_executed.executed_shares = reader.read_num<uint32_t>();
    msg.order_executed.match_number = reader.read_num<uint64_t>();
  }

  static void parse_order_executed_price_msg(BufReader &reader,
                                             ItchMessage &msg) noexcept {
    msg.type = MsgType::OrderExecutedPrice;
    msg.order_executed_price.stock_locate = reader.read_num<uint16_t>();
    msg.order_executed_price.tracking_number = reader.read_num<uint16_t>();
    msg.order_executed_price.timestamp = reader.read_timestamp();
    msg.order_executed_price.order_ref_num = reader.read_num<uint64_t>();
    msg.order_executed_price.executed_shares = reader.read_num<uint32_t>();
    msg.order_executed_price.match_number = reader.read_num<uint64_t>();
    msg.order_executed_price.printable =
        static_cast<char>(reader.read_num<uint8_t>());
    msg.order_executed_price.execution_price = reader.read_num<uint32_t>();
  }

  static void parse_order_cancel_msg(BufReader &reader,
                                     ItchMessage &msg) noexcept {
    msg.type = MsgType::OrderCancel;
    msg.order_cancel.stock_locate = reader.read_num<uint16_t>();
    msg.order_cancel.tracking_number = reader.read_num<uint16_t>();
    msg.order_cancel.timestamp = reader.read_timestamp();
    msg.order_cancel.order_ref_num = reader.read_num<uint64_t>();
    msg.order_cancel.cancelled_shares = reader.read_num<uint32_t>();
  }

  static void parse_order_delete_msg(BufReader &reader,
                                     ItchMessage &msg) noexcept {
    msg.type = MsgType::OrderDelete;
    msg.order_delete.stock_locate = reader.read_num<uint16_t>();
    msg.order_delete.tracking_number = reader.read_num<uint16_t>();
    msg.order_delete.timestamp = reader.read_timestamp();
    msg.order_delete.order_ref_num = reader.read_num<uint64_t>();
  }

  static void parse_order_replace_msg(BufReader &reader,
                                      ItchMessage &msg) noexcept {
    msg.type = MsgType::OrderReplace;
    msg.order_replace.stock_locate = reader.read_num<uint16_t>();
    msg.order_replace.tracking_number = reader.read_num<uint16_t>();
    msg.order_replace.timestamp = reader.read_timestamp();
    msg.order_replace.orig_order_ref_num = reader.read_num<uint64_t>();
    msg.order_replace.new_order_ref_num = reader.read_num<uint64_t>();
    msg.order_replace.shares = reader.read_num<uint32_t>();
    msg.order_replace.price = reader.read_num<uint32_t>();
  }

  static inline MboEvent
  create_mbo_from_add_order(const ItchMessage &msg) noexcept {
    assert(msg.type == MsgType::AddOrder);
    assert(msg.add_order.side == 'B' || msg.add_order.side == 'S');

    return MboEvent{.etype = EventType::ADDED,
                    .side = msg.add_order.side == 'B' ? Side::BID : Side::ASK,
                    .printable = true,
                    .instrument = msg.add_order.stock_locate,
                    .timestamp = msg.add_order.timestamp,
                    .oid = msg.add_order.order_ref_num,
                    .price = msg.add_order.price,
                    .qty = msg.add_order.shares};
  }

  static inline MboEvent
  create_mbo_from_add_order_mpid(const ItchMessage &msg) noexcept {
    assert(msg.type == MsgType::AddOrderMPID);

    return MboEvent{.etype = EventType::ADDED,
                    .side =
                        msg.add_order_mpid.side == 'B' ? Side::BID : Side::ASK,
                    .printable = true,
                    .instrument = msg.add_order_mpid.stock_locate,
                    .timestamp = msg.add_order_mpid.timestamp,
                    .oid = msg.add_order_mpid.order_ref_num,
                    .price = msg.add_order_mpid.price,
                    .qty = msg.add_order_mpid.shares};
  }

  static inline MboEvent
  create_mbo_from_order_exec(const ItchMessage &msg) noexcept {
    assert(msg.type == MsgType::OrderExecuted);

    return MboEvent{.etype = EventType::EXECUTED,
                    .printable = true,
                    .instrument = msg.order_executed.stock_locate,
                    .timestamp = msg.order_executed.timestamp,
                    .oid = msg.order_executed.order_ref_num,
                    .qty = msg.order_executed.executed_shares};
  }

  static inline MboEvent
  create_mbo_from_order_exec_price(const ItchMessage &msg) noexcept {
    assert(msg.type == MsgType::OrderExecutedPrice);

    return MboEvent{.etype = EventType::EXECUTED,
                    .printable = msg.order_executed_price.printable == 'Y',
                    .instrument = msg.order_executed_price.stock_locate,
                    .timestamp = msg.order_executed_price.timestamp,
                    .oid = msg.order_executed_price.order_ref_num,
                    .price = msg.order_executed_price.execution_price,
                    .qty = msg.order_executed_price.executed_shares};
  }

  static inline MboEvent
  create_mbo_from_order_cancel(const ItchMessage &msg) noexcept {
    assert(msg.type == MsgType::OrderCancel);

    return MboEvent{.etype = EventType::CANCELLED,
                    .printable = true,
                    .instrument = msg.order_cancel.stock_locate,
                    .timestamp = msg.order_cancel.timestamp,
                    .oid = msg.order_cancel.order_ref_num,
                    .qty = msg.order_cancel.cancelled_shares};
  }

  static inline MboEvent
  create_mbo_from_order_delete(const ItchMessage &msg) noexcept {
    assert(msg.type == MsgType::OrderDelete);

    return MboEvent{
        .etype = EventType::DELETED,
        .printable = true,
        .instrument = msg.order_delete.stock_locate,
        .timestamp = msg.order_delete.timestamp,
        .oid = msg.order_delete.order_ref_num,
    };
  }

  static inline MboEvent
  create_mbo_from_order_replace(const ItchMessage &msg) noexcept {
    assert(msg.type == MsgType::OrderReplace);

    return MboEvent{.etype = EventType::REPLACED,
                    .printable = true,
                    .instrument = msg.order_replace.stock_locate,
                    .timestamp = msg.order_replace.timestamp,
                    .oid = msg.order_replace.new_order_ref_num,
                    .orig_oid = msg.order_replace.orig_order_ref_num,
                    .price = msg.order_replace.price,
                    .qty = msg.order_replace.shares};
  }

public:
  template <EventSink Sink>
  static void parse(std::span<const std::byte> data, Sink &sink) noexcept {
    BufReader reader(data);

    ItchMessage msg = parse_msg(reader);
    switch (msg.type) {
    case MsgType::AddOrder:
      sink.on_event(create_mbo_from_add_order(msg));
      break;
    case MsgType::AddOrderMPID:
      sink.on_event(create_mbo_from_add_order_mpid(msg));
      break;
    case MsgType::OrderExecuted:
      sink.on_event(create_mbo_from_order_exec(msg));
      break;
    case MsgType::OrderExecutedPrice:
      sink.on_event(create_mbo_from_order_exec_price(msg));
      break;
    case MsgType::OrderCancel:
      sink.on_event(create_mbo_from_order_cancel(msg));
      break;
    case MsgType::OrderDelete:
      sink.on_event(create_mbo_from_order_delete(msg));
      break;
    case MsgType::OrderReplace:
      sink.on_event(create_mbo_from_order_replace(msg));
      break;
    default:
      break;
    }
  }
};
