#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

#include "itch_messages.hpp"

namespace fastbook::feed {
template <typename T> T from_big_endian(T value) noexcept {
  if constexpr (std::endian::native == std::endian::little) {
    return std::byteswap(value);
  }
  return value;
}

template <typename Queue> class FeedHandler {
private:
  using Producer = typename Queue::SPSCProducer;

  static constexpr size_t MAX_MSG_SIZE = 50;
  std::byte reassembly_buffer_[MAX_MSG_SIZE];
  uint32_t leftover_bytes_;
  Producer producer_;

  struct BufReader {
    std::span<const std::byte> buf_;

    template <typename T> T read_num() noexcept {
      assert(buf_.size() >= sizeof(T));
      T value;
      std::memcpy(&value, buf_.data(), sizeof(T));
      buf_ = buf_.subspan(sizeof(T));
      return from_big_endian(value);
    }

    uint64_t read_timestamp() noexcept {
      uint64_t value{0};
      std::memcpy(reinterpret_cast<uint8_t *>(&value) + 2, buf_.data(), 6);
      buf_ = buf_.subspan(6);
      return from_big_endian(value);
    }

    template <size_t N> void read_chars(char (&dst)[N]) noexcept {
      assert(buf_.size() >= N);
      std::memcpy(dst, buf_.data(), N);
      buf_ = buf_.subspan(N);
    }

    bool has_bytes() const noexcept { return buf_.size() > 0; }
    std::size_t remaining() const noexcept { return buf_.size(); }
    const std::byte *data() const noexcept { return buf_.data(); }
    void set_buf(std::span<const std::byte> new_buf) { buf_ = new_buf; }
  };

  std::optional<Message>
  handle_leftover_bytes(std::span<const std::byte> &data) noexcept {
    if (leftover_bytes_ < 2) {
      assert(leftover_bytes_ == 1);
      reassembly_buffer_[leftover_bytes_++] = data[0];
      data = data.subspan(1);
    }

    BufReader reader{
        std::span<const std::byte>(reassembly_buffer_, leftover_bytes_)};
    uint16_t msg_size = reader.template read_num<uint16_t>();
    size_t needed = (sizeof(msg_size) + msg_size) - leftover_bytes_;
    if (data.size() >= needed) [[likely]] {
      memcpy(reassembly_buffer_ + leftover_bytes_, data.data(), needed);
    } else {
      memcpy(reassembly_buffer_ + leftover_bytes_, data.data(), data.size());
      leftover_bytes_ += data.size();
      data = data.subspan(data.size());
      return std::nullopt;
    }

    reader.set_buf(std::span<const std::byte>(
        reassembly_buffer_ + sizeof(msg_size), msg_size));
    data = data.subspan(needed);
    leftover_bytes_ = 0;
    return parse_msg(reader);
  }

  Message parse_msg(BufReader &reader) noexcept {
    Message msg;
    char msg_type = static_cast<char>(reader.template read_num<uint8_t>());
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

  void parse_add_order_msg(BufReader &reader, Message &msg) noexcept {
    msg.type = MsgType::AddOrder;
    msg.add_order.stock_locate = reader.template read_num<uint16_t>();
    msg.add_order.tracking_number = reader.template read_num<uint16_t>();
    msg.add_order.timestamp = reader.read_timestamp();
    msg.add_order.order_ref_num = reader.template read_num<uint64_t>();
    msg.add_order.side = static_cast<char>(reader.template read_num<uint8_t>());
    msg.add_order.shares = reader.template read_num<uint32_t>();
    reader.read_chars(msg.add_order.stock);
    msg.add_order.price = reader.template read_num<uint32_t>();
  }

  void parse_add_order_mpid_msg(BufReader &reader, Message &msg) noexcept {
    msg.type = MsgType::AddOrderMPID;
    msg.add_order_mpid.stock_locate = reader.template read_num<uint16_t>();
    msg.add_order_mpid.tracking_number = reader.template read_num<uint16_t>();
    msg.add_order_mpid.timestamp = reader.read_timestamp();
    msg.add_order_mpid.order_ref_num = reader.template read_num<uint64_t>();
    msg.add_order_mpid.side =
        static_cast<char>(reader.template read_num<uint8_t>());
    msg.add_order_mpid.shares = reader.template read_num<uint32_t>();
    reader.read_chars(msg.add_order_mpid.stock);
    msg.add_order_mpid.price = reader.template read_num<uint32_t>();
    reader.read_chars(msg.add_order_mpid.attribution);
  }

  void parse_order_executed_msg(BufReader &reader, Message &msg) noexcept {
    msg.type = MsgType::OrderExecuted;
    msg.order_executed.stock_locate = reader.template read_num<uint16_t>();
    msg.order_executed.tracking_number = reader.template read_num<uint16_t>();
    msg.order_executed.timestamp = reader.read_timestamp();
    msg.order_executed.order_ref_num = reader.template read_num<uint64_t>();
    msg.order_executed.executed_shares = reader.template read_num<uint32_t>();
    msg.order_executed.match_number = reader.template read_num<uint64_t>();
  }

  void parse_order_executed_price_msg(BufReader &reader,
                                      Message &msg) noexcept {
    msg.type = MsgType::OrderExecutedPrice;
    msg.order_executed_price.stock_locate =
        reader.template read_num<uint16_t>();
    msg.order_executed_price.tracking_number =
        reader.template read_num<uint16_t>();
    msg.order_executed_price.timestamp = reader.read_timestamp();
    msg.order_executed_price.order_ref_num =
        reader.template read_num<uint64_t>();
    msg.order_executed_price.executed_shares =
        reader.template read_num<uint32_t>();
    msg.order_executed_price.match_number =
        reader.template read_num<uint64_t>();
    msg.order_executed_price.printable =
        static_cast<char>(reader.template read_num<uint8_t>());
    msg.order_executed_price.execution_price =
        reader.template read_num<uint32_t>();
  }

  void parse_order_cancel_msg(BufReader &reader, Message &msg) noexcept {
    msg.type = MsgType::OrderCancel;
    msg.order_cancel.stock_locate = reader.template read_num<uint16_t>();
    msg.order_cancel.tracking_number = reader.template read_num<uint16_t>();
    msg.order_cancel.timestamp = reader.read_timestamp();
    msg.order_cancel.order_ref_num = reader.template read_num<uint64_t>();
    msg.order_cancel.cancelled_shares = reader.template read_num<uint32_t>();
  }

  void parse_order_delete_msg(BufReader &reader, Message &msg) noexcept {
    msg.type = MsgType::OrderDelete;
    msg.order_delete.stock_locate = reader.template read_num<uint16_t>();
    msg.order_delete.tracking_number = reader.template read_num<uint16_t>();
    msg.order_delete.timestamp = reader.read_timestamp();
    msg.order_delete.order_ref_num = reader.template read_num<uint64_t>();
  }

  void parse_order_replace_msg(BufReader &reader, Message &msg) noexcept {
    msg.type = MsgType::OrderReplace;
    msg.order_replace.stock_locate = reader.template read_num<uint16_t>();
    msg.order_replace.tracking_number = reader.template read_num<uint16_t>();
    msg.order_replace.timestamp = reader.read_timestamp();
    msg.order_replace.orig_order_ref_num = reader.template read_num<uint64_t>();
    msg.order_replace.new_order_ref_num = reader.template read_num<uint64_t>();
    msg.order_replace.shares = reader.template read_num<uint32_t>();
    msg.order_replace.price = reader.template read_num<uint32_t>();
  }

public:
  FeedHandler(Producer producer) : leftover_bytes_(0), producer_(producer) {}

  void feed(std::span<const std::byte> data) noexcept {
    if (leftover_bytes_ > 0) {
      std::optional<Message> leftover_msg = handle_leftover_bytes(data);
      if (leftover_msg.has_value()) [[likely]] {
        producer_.push(leftover_msg.value());
      } else {
        return;
      }
    }

    BufReader reader{data};

    while (reader.has_bytes()) {
      const std::byte *save_bytes = reader.data();
      size_t rem = reader.remaining();
      if (rem < 2) {
        std::memcpy(reassembly_buffer_, save_bytes, rem);
        leftover_bytes_ = rem;
        break;
      }

      uint16_t msg_length = reader.template read_num<uint16_t>();

      if (reader.remaining() < msg_length) {
        std::memcpy(reassembly_buffer_, save_bytes, rem);
        leftover_bytes_ += rem;
        break;
      }

      Message msg = parse_msg(reader);
      if (msg.type != MsgType::Unknown) [[likely]] {
        producer_.push(msg);
      }
    }
  }
};

} // namespace fastbook::feed