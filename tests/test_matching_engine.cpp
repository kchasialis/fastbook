#include <gtest/gtest.h>

#include "matching_engine.hpp"

class MatchingEngineTest : public ::testing::Test {
protected:
  static constexpr uint32_t BASE = 1000;
  static constexpr uint32_t TICK = 1;
  static constexpr uint32_t WIN = 64;

  OrderBook book;
  MatchingEngine engine;

  void SetUp() override { book.reset(BASE, TICK, WIN); }
  Fill fills[256];
};

TEST_F(MatchingEngineTest, NoMatchWhenBookEmpty) {
  size_t n = engine.match(book, 1, Side::BID, 1010, 100, fills);
  EXPECT_EQ(n, 0);
}

TEST_F(MatchingEngineTest, NoMatchWhenPriceTooLow) {
  book.add_order(1, 100, 1015, Side::ASK);
  size_t n = engine.match(book, 99, Side::BID, 1010, 100, fills);
  EXPECT_EQ(n, 0);
}

TEST_F(MatchingEngineTest, FullFillSingleOrder) {
  book.add_order(1, 100, 1010, Side::ASK);
  size_t n = engine.match(book, 99, Side::BID, 1010, 100, fills);
  ASSERT_EQ(n, 1);
  EXPECT_EQ(fills[0].pass_order_id, 1);
  EXPECT_EQ(fills[0].agg_order_id, 99);
  EXPECT_EQ(fills[0].qty, 100);
  EXPECT_EQ(fills[0].price, 1010);
}

TEST_F(MatchingEngineTest, PartialFillSingleOrder) {
  book.add_order(1, 200, 1010, Side::ASK);
  size_t n = engine.match(book, 99, Side::BID, 1010, 50, fills);
  ASSERT_EQ(n, 1);
  EXPECT_EQ(fills[0].qty, 50);
}

TEST_F(MatchingEngineTest, FillAcrossMultipleOrders) {
  book.add_order(1, 60, 1010, Side::ASK);
  book.add_order(2, 60, 1010, Side::ASK);
  size_t n = engine.match(book, 99, Side::BID, 1010, 100, fills);
  ASSERT_EQ(n, 2);
  EXPECT_EQ(fills[0].qty, 60);
  EXPECT_EQ(fills[1].qty, 40);
}

TEST_F(MatchingEngineTest, FillAcrossMultiplePriceLevels) {
  book.add_order(1, 50, 1010, Side::ASK);
  book.add_order(2, 50, 1011, Side::ASK);
  size_t n = engine.match(book, 99, Side::BID, 1011, 100, fills);
  ASSERT_EQ(n, 2);
  EXPECT_EQ(fills[0].price, 1010);
  EXPECT_EQ(fills[1].price, 1011);
}

TEST_F(MatchingEngineTest, AskMatchesBid) {
  book.add_order(1, 100, 1010, Side::BID);
  size_t n = engine.match(book, 99, Side::ASK, 1010, 100, fills);
  ASSERT_EQ(n, 1);
  EXPECT_EQ(fills[0].qty, 100);
}