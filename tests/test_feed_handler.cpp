#include <gtest/gtest.h>

#include "feed_handler.hpp"
#include "spsc_queue.hpp"

using Queue = SPSCQueue<Message, 64>;

template <typename T> static void write_be(std::vector<std::byte> &buf, T val) {
  for (int i = static_cast<int>(sizeof(T)) - 1; i >= 0; i--) {
    buf.push_back(static_cast<std::byte>((val >> (8 * i)) & 0xFF));
  }
}

static void write_timestamp(std::vector<std::byte> &buf, uint64_t ts = 0) {
  for (int i = 5; i >= 0; i--) {
    buf.push_back(static_cast<std::byte>((ts >> (8 * i)) & 0xFF));
  }
}

static std::vector<std::byte> make_add_order(uint64_t order_ref, char side,
                                             uint32_t shares, uint32_t price) {
  std::vector<std::byte> msg;
  std::vector<std::byte> payload;
  payload.push_back(static_cast<std::byte>('A'));
  write_be<uint16_t>(payload, 1);
  write_be<uint16_t>(payload, 0);
  write_timestamp(payload);
  write_be<uint64_t>(payload, order_ref);
  payload.push_back(static_cast<std::byte>(side));
  write_be<uint32_t>(payload, shares);
  for (int i = 0; i < 8; i++)
    payload.push_back(static_cast<std::byte>('X'));
  write_be<uint32_t>(payload, price);

  write_be<uint16_t>(msg, static_cast<uint16_t>(payload.size()));
  msg.insert(msg.end(), payload.begin(), payload.end());
  return msg;
}

static std::vector<std::byte> make_order_delete(uint64_t order_ref) {
  std::vector<std::byte> msg;
  std::vector<std::byte> payload;
  payload.push_back(static_cast<std::byte>('D'));
  write_be<uint16_t>(payload, 1);
  write_be<uint16_t>(payload, 0);
  write_timestamp(payload);
  write_be<uint64_t>(payload, order_ref);

  write_be<uint16_t>(msg, static_cast<uint16_t>(payload.size()));
  msg.insert(msg.end(), payload.begin(), payload.end());
  return msg;
}

class FeedHandlerTest : public ::testing::Test {
protected:
  Queue queue;
  FeedHandler<Queue> handler{queue.producer()};
};

TEST_F(FeedHandlerTest, ParseAddOrder) {
  auto buf = make_add_order(42, 'B', 100, 10050);
  handler.feed({buf.data(), buf.size()});

  auto msg = queue.consumer().pop();
  ASSERT_TRUE(msg.has_value());
  EXPECT_EQ(msg->type, MsgType::AddOrder);
  EXPECT_EQ(msg->add_order.order_ref_num, 42u);
  EXPECT_EQ(msg->add_order.side, 'B');
  EXPECT_EQ(msg->add_order.shares, 100u);
  EXPECT_EQ(msg->add_order.price, 10050u);
}

TEST_F(FeedHandlerTest, ParseOrderDelete) {
  auto buf = make_order_delete(99);
  handler.feed({buf.data(), buf.size()});

  auto msg = queue.consumer().pop();
  ASSERT_TRUE(msg.has_value());
  EXPECT_EQ(msg->type, MsgType::OrderDelete);
  EXPECT_EQ(msg->order_delete.order_ref_num, 99u);
}

TEST_F(FeedHandlerTest, UnknownMsgTypeIgnored) {
  std::vector<std::byte> buf;
  write_be<uint16_t>(buf, 1);
  buf.push_back(static_cast<std::byte>('Z'));

  handler.feed({buf.data(), buf.size()});
  EXPECT_FALSE(queue.consumer().pop().has_value());
}

TEST_F(FeedHandlerTest, MultipleMessagesInOneFeed) {
  auto a = make_add_order(1, 'B', 50, 1000);
  auto d = make_order_delete(1);

  std::vector<std::byte> combined;
  combined.insert(combined.end(), a.begin(), a.end());
  combined.insert(combined.end(), d.begin(), d.end());

  handler.feed({combined.data(), combined.size()});

  auto m1 = queue.consumer().pop();
  auto m2 = queue.consumer().pop();
  ASSERT_TRUE(m1.has_value());
  ASSERT_TRUE(m2.has_value());
  EXPECT_EQ(m1->type, MsgType::AddOrder);
  EXPECT_EQ(m2->type, MsgType::OrderDelete);
}

TEST_F(FeedHandlerTest, MessageSplitAcrossTwoFeeds) {
  auto buf = make_add_order(7, 'S', 200, 5000);

  size_t split = buf.size() / 2;
  handler.feed({buf.data(), split});
  EXPECT_FALSE(queue.consumer().pop().has_value());

  handler.feed({buf.data() + split, buf.size() - split});
  auto msg = queue.consumer().pop();
  ASSERT_TRUE(msg.has_value());
  EXPECT_EQ(msg->type, MsgType::AddOrder);
  EXPECT_EQ(msg->add_order.order_ref_num, 7u);
}
