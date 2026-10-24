#include <gtest/gtest.h>

#include "order_book.hpp"

#include <algorithm>
#include <map>
#include <random>
#include <vector>

namespace {

bufpool::BufferPool &pool() {
  static bufpool::BufferPool p;
  return p;
}

constexpr uint32_t NO_BID = 0;
constexpr uint32_t NO_ASK = UINT32_MAX;

} // namespace

class OrderBookTest : public ::testing::Test {
protected:
  using Book = OrderBook<4, 16>;

  Book book{pool()};

  bool add(oid_t oid, uint32_t shares, uint32_t price, Side side) {
    return book.add_order(oid, shares, price, side).has_value();
  }

  oid_t head(Side side, uint32_t price) {
    Order *o = book.get_first_order(side, price);
    return o == nullptr ? 0 : o->order_num;
  }
};

TEST_F(OrderBookTest, BestPricesOnBothSides) {
  EXPECT_EQ(book.best_price(Side::BID), NO_BID);
  EXPECT_EQ(book.best_price(Side::ASK), NO_ASK);

  ASSERT_TRUE(add(1, 100, 1000500, Side::BID));
  ASSERT_TRUE(add(2, 100, 1026100, Side::BID));
  ASSERT_TRUE(add(3, 100, 500000, Side::BID));
  ASSERT_TRUE(add(4, 100, 1030000, Side::ASK));
  ASSERT_TRUE(add(5, 100, 1029900, Side::ASK));

  EXPECT_EQ(book.best_price(Side::BID), 1026100u);
  EXPECT_EQ(book.best_price(Side::ASK), 1029900u);
}

TEST_F(OrderBookTest, DuplicateOrderIdIsRejected) {
  ASSERT_TRUE(add(1, 100, 1010, Side::BID));

  auto res = book.add_order(1, 50, 1020, Side::ASK);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error(), Book::AddOrderError::DuplicateOrderId);
}

TEST_F(OrderBookTest, CancelKeepsTimePriority) {
  for (oid_t oid = 1; oid <= 3; oid++) {
    ASSERT_TRUE(add(oid, 100, 1010, Side::BID));
  }
  ASSERT_TRUE(add(4, 100, 1005, Side::BID));

  EXPECT_TRUE(book.cancel_order(2));
  EXPECT_EQ(head(Side::BID, 1010), 1u);
  EXPECT_TRUE(book.cancel_order(1));
  EXPECT_EQ(head(Side::BID, 1010), 3u);
  EXPECT_TRUE(book.cancel_order(3));
  EXPECT_EQ(head(Side::BID, 1010), 0u);
  EXPECT_EQ(book.best_price(Side::BID), 1005u);

  EXPECT_FALSE(book.cancel_order(3));
  EXPECT_FALSE(book.cancel_order(99));
}

TEST_F(OrderBookTest, ExecuteReducesThenRemoves) {
  ASSERT_TRUE(add(1, 100, 1010, Side::ASK));
  ASSERT_TRUE(add(2, 100, 1010, Side::ASK));
  ASSERT_TRUE(add(3, 100, 1020, Side::ASK));

  EXPECT_TRUE(book.execute_order(1, 40));
  EXPECT_EQ(book.get_order_by_oid(1)->shares, 60u);
  EXPECT_EQ(head(Side::ASK, 1010), 1u);

  EXPECT_TRUE(book.reduce_order(1, 10));
  EXPECT_EQ(book.get_order_by_oid(1)->shares, 50u);

  EXPECT_TRUE(book.execute_order(1, 50));
  EXPECT_EQ(head(Side::ASK, 1010), 2u);
  EXPECT_TRUE(book.execute_order(2, 100));
  EXPECT_EQ(book.best_price(Side::ASK), 1020u);

  EXPECT_FALSE(book.execute_order(1, 1));
  EXPECT_FALSE(book.execute_order(99, 1));
}

TEST_F(OrderBookTest, FullSideRejectsNewLevelsUntilOneFrees) {
  for (uint32_t i = 0; i < 20; i++) {
    ASSERT_TRUE(add(i + 1, 100, 1000 - i, Side::BID)) << "level " << i;
  }

  for (uint32_t price : {900u, 2000u}) {
    auto res = book.add_order(100, 100, price, Side::BID);
    ASSERT_FALSE(res.has_value()) << "price " << price;
    EXPECT_EQ(res.error(), Book::AddOrderError::LevelsFull);
  }
  EXPECT_FALSE(book.cancel_order(100));
  EXPECT_EQ(book.best_price(Side::BID), 1000u);

  EXPECT_TRUE(add(101, 100, 1000, Side::BID));
  EXPECT_TRUE(add(102, 100, 1500, Side::ASK));

  ASSERT_TRUE(book.cancel_order(20));
  EXPECT_TRUE(add(103, 100, 900, Side::BID));
}

TEST(OrderBookModel, RandomOperationsMatchModel) {
  OrderBook<4, 64> book{pool()};
  std::map<uint32_t, std::vector<oid_t>> bids;
  std::map<uint32_t, std::vector<oid_t>> asks;
  std::map<oid_t, std::pair<Side, uint32_t>> live;
  std::mt19937_64 rng(7);
  oid_t next_oid = 1;

  for (int step = 0; step < 20000; step++) {
    const uint64_t action = rng() % 3;
    if (action == 0 || live.empty()) {
      const Side side = rng() % 2 == 0 ? Side::BID : Side::ASK;
      const uint32_t price = static_cast<uint32_t>(
          side == Side::BID ? 1000 + rng() % 40 : 1040 + rng() % 40);
      const oid_t oid = next_oid++;
      ASSERT_TRUE(book.add_order(oid, 10, price, side).has_value())
          << "step " << step;
      (side == Side::BID ? bids : asks)[price].push_back(oid);
      live[oid] = {side, price};
    } else {
      auto it = live.begin();
      std::advance(it, static_cast<long>(rng() % live.size()));
      const oid_t oid = it->first;
      const auto [side, price] = it->second;
      if (action == 1) {
        ASSERT_TRUE(book.cancel_order(oid)) << "step " << step;
      } else {
        ASSERT_TRUE(book.execute_order(oid, 10)) << "step " << step;
      }
      auto &levels = side == Side::BID ? bids : asks;
      auto &queue = levels[price];
      queue.erase(std::find(queue.begin(), queue.end(), oid));
      if (queue.empty()) {
        levels.erase(price);
      }
      live.erase(it);
    }

    const uint32_t best_bid = bids.empty() ? NO_BID : bids.rbegin()->first;
    const uint32_t best_ask = asks.empty() ? NO_ASK : asks.begin()->first;
    ASSERT_EQ(book.best_price(Side::BID), best_bid) << "step " << step;
    ASSERT_EQ(book.best_price(Side::ASK), best_ask) << "step " << step;
    if (!bids.empty()) {
      ASSERT_EQ(book.get_first_order(Side::BID, best_bid)->order_num,
                bids.rbegin()->second.front())
          << "step " << step;
    }
    if (!asks.empty()) {
      ASSERT_EQ(book.get_first_order(Side::ASK, best_ask)->order_num,
                asks.begin()->second.front())
          << "step " << step;
    }
  }
}
