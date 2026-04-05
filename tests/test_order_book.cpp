#include <gtest/gtest.h>

#include "order_book.hpp"

class OrderBookTest : public ::testing::Test {
protected:
  static constexpr uint32_t BASE = 1000;
  static constexpr uint32_t TICK = 1;
  static constexpr uint32_t WIN = 64;

  OrderBook book{BASE, TICK, WIN};
};

TEST_F(OrderBookTest, AddOrderSucceeds) {
  EXPECT_TRUE(book.add_order(1, 100, 1010, Side::BID));
}

TEST_F(OrderBookTest, AddDuplicateOrderIdFails) {
  book.add_order(1, 100, 1010, Side::BID);
  EXPECT_FALSE(book.add_order(1, 50, 1010, Side::BID));
}

TEST_F(OrderBookTest, AddOrderBelowBasePriceFails) {
  EXPECT_FALSE(book.add_order(1, 100, 999, Side::BID));
}

TEST_F(OrderBookTest, AddMultipleOrdersSamePriceLevel) {
  EXPECT_TRUE(book.add_order(1, 100, 1010, Side::BID));
  EXPECT_TRUE(book.add_order(2, 200, 1010, Side::BID));
  EXPECT_TRUE(book.add_order(3, 50, 1010, Side::BID));
}

TEST_F(OrderBookTest, CancelUnknownOrderFails) {
  EXPECT_FALSE(book.cancel_order(99));
}

TEST_F(OrderBookTest, CancelOnlyOrder) {
  book.add_order(1, 100, 1010, Side::BID);
  EXPECT_TRUE(book.cancel_order(1));
  EXPECT_FALSE(book.cancel_order(1));
}

TEST_F(OrderBookTest, CancelHeadOfQueue) {
  book.add_order(1, 100, 1010, Side::BID);
  book.add_order(2, 200, 1010, Side::BID);
  EXPECT_TRUE(book.cancel_order(1));
  EXPECT_FALSE(book.cancel_order(1));
  EXPECT_TRUE(book.cancel_order(2));
}

TEST_F(OrderBookTest, CancelTailOfQueue) {
  book.add_order(1, 100, 1010, Side::BID);
  book.add_order(2, 200, 1010, Side::BID);
  EXPECT_TRUE(book.cancel_order(2));
  EXPECT_TRUE(book.cancel_order(1));
}

TEST_F(OrderBookTest, CancelMiddleOfQueue) {
  book.add_order(1, 100, 1010, Side::BID);
  book.add_order(2, 200, 1010, Side::BID);
  book.add_order(3, 50, 1010, Side::BID);
  EXPECT_TRUE(book.cancel_order(2));
  EXPECT_TRUE(book.cancel_order(1));
  EXPECT_TRUE(book.cancel_order(3));
}

TEST_F(OrderBookTest, ExecuteUnknownOrderFails) {
  EXPECT_FALSE(book.execute_order(99, 10));
}

TEST_F(OrderBookTest, PartialExecuteReducesShares) {
  book.add_order(1, 100, 1010, Side::ASK);
  EXPECT_TRUE(book.execute_order(1, 40));
  EXPECT_TRUE(book.cancel_order(1));
}

TEST_F(OrderBookTest, FullExecuteRemovesOrder) {
  book.add_order(1, 100, 1010, Side::ASK);
  EXPECT_TRUE(book.execute_order(1, 100));
  EXPECT_FALSE(book.cancel_order(1));
}

TEST_F(OrderBookTest, BestPriceEmptyBookReturnsZero) {
  EXPECT_EQ(book.best_price(Side::BID), 0);
  EXPECT_EQ(book.best_price(Side::ASK), 0);
}

TEST_F(OrderBookTest, BestBidIsHighestBidPrice) {
  book.add_order(1, 100, 1005, Side::BID);
  book.add_order(2, 100, 1010, Side::BID);
  book.add_order(3, 100, 1003, Side::BID);
  EXPECT_EQ(book.best_price(Side::BID), 1010);
}

TEST_F(OrderBookTest, BestAskIsLowestAskPrice) {
  book.add_order(1, 100, 1020, Side::ASK);
  book.add_order(2, 100, 1015, Side::ASK);
  book.add_order(3, 100, 1025, Side::ASK);
  EXPECT_EQ(book.best_price(Side::ASK), 1015);
}

TEST_F(OrderBookTest, BestBidUpdatesAfterCancelOfBest) {
  book.add_order(1, 100, 1010, Side::BID);
  book.add_order(2, 100, 1005, Side::BID);
  book.cancel_order(1);
  EXPECT_EQ(book.best_price(Side::BID), 1005);
}

TEST_F(OrderBookTest, BestAskUpdatesAfterCancelOfBest) {
  book.add_order(1, 100, 1015, Side::ASK);
  book.add_order(2, 100, 1020, Side::ASK);
  book.cancel_order(1);
  EXPECT_EQ(book.best_price(Side::ASK), 1020u);
}

TEST_F(OrderBookTest, BestBidBecomesZeroAfterAllCancelled) {
  book.add_order(1, 100, 1010, Side::BID);
  book.cancel_order(1);
  EXPECT_EQ(book.best_price(Side::BID), 0u);
}

TEST_F(OrderBookTest, BestPriceUpdatesAfterFullExecute) {
  book.add_order(1, 100, 1010, Side::BID);
  book.add_order(2, 100, 1005, Side::BID);
  book.execute_order(1, 100);
  EXPECT_EQ(book.best_price(Side::BID), 1005);
}

TEST_F(OrderBookTest, SlotReusedAfterWindowWrap) {
  book.add_order(1, 100, BASE, Side::BID);
  book.cancel_order(1);
  EXPECT_TRUE(book.add_order(2, 50, BASE + WIN, Side::BID));
}
