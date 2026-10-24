#include <gtest/gtest.h>

#include "two_tier_levels.hpp"

#include <initializer_list>
#include <random>
#include <set>

namespace {

template <typename L> void insert_new(L &levels, uint32_t price) {
  LevelData *d = levels.find_or_insert(price);
  ASSERT_NE(d, nullptr);
  EXPECT_EQ(d->head, nullptr);
  EXPECT_EQ(d->tail, nullptr);
  EXPECT_EQ(d->total_qty, 0u);
  d->total_qty = price;
}

template <typename L>
void expect_present(L &levels, std::initializer_list<uint32_t> prices) {
  for (uint32_t p : prices) {
    const LevelData *d = levels.find(p);
    ASSERT_NE(d, nullptr) << "price " << p;
    EXPECT_EQ(d->total_qty, p) << "price " << p;
  }
}

template <typename L>
void expect_absent(L &levels, std::initializer_list<uint32_t> prices) {
  for (uint32_t p : prices) {
    EXPECT_EQ(levels.find(p), nullptr) << "price " << p;
  }
}

template <Side S> void run_against_model(uint64_t seed) {
  constexpr uint32_t MAX_PRICE = 48;
  TwoTierLevels<S, 4, 64> levels;
  std::set<uint32_t> model;
  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<uint32_t> dist(1, MAX_PRICE);

  for (int step = 0; step < 20000; step++) {
    const uint32_t price = dist(rng);
    if (model.contains(price)) {
      levels.erase(price);
      model.erase(price);
    } else {
      LevelData *d = levels.find_or_insert(price);
      ASSERT_NE(d, nullptr) << "step " << step;
      ASSERT_EQ(d->total_qty, 0u) << "step " << step;
      d->total_qty = price;
      model.insert(price);
    }

    uint32_t expected = decltype(levels)::EMPTY;
    if (!model.empty()) {
      expected = S == Side::BID ? *model.rbegin() : *model.begin();
    }
    ASSERT_EQ(levels.best_price(), expected) << "step " << step;
    if (model.empty()) {
      ASSERT_EQ(levels.best(), nullptr) << "step " << step;
    } else {
      ASSERT_NE(levels.best(), nullptr) << "step " << step;
      ASSERT_EQ(levels.best()->total_qty, expected) << "step " << step;
    }

    for (uint32_t p = 1; p <= MAX_PRICE; p++) {
      const LevelData *d = levels.find(p);
      if (model.contains(p)) {
        ASSERT_NE(d, nullptr) << "step " << step << " price " << p;
        ASSERT_EQ(d->total_qty, p) << "step " << step << " price " << p;
      } else {
        ASSERT_EQ(d, nullptr) << "step " << step << " price " << p;
      }
    }
  }
}

} // namespace

TEST(TwoTierLevels, BestAndFindOnBothSides) {
  TwoTierLevels<Side::BID, 2, 8> bids;
  TwoTierLevels<Side::ASK, 2, 8> asks;

  EXPECT_EQ(bids.best_price(), 0u);
  EXPECT_EQ(asks.best_price(), UINT32_MAX);
  EXPECT_EQ(bids.best(), nullptr);
  EXPECT_EQ(asks.best(), nullptr);

  for (uint32_t p : {100u, 99u, 98u, 101u, 97u, 102u}) {
    insert_new(bids, p);
    insert_new(asks, p);
  }

  EXPECT_EQ(bids.best_price(), 102u);
  EXPECT_EQ(asks.best_price(), 97u);
  EXPECT_EQ(bids.best()->total_qty, 102u);
  EXPECT_EQ(asks.best()->total_qty, 97u);
  expect_present(bids, {97, 98, 99, 100, 101, 102});
  expect_present(asks, {97, 98, 99, 100, 101, 102});
  expect_absent(bids, {96, 103});
  expect_absent(asks, {96, 103});
}

TEST(TwoTierLevels, FindOrInsertKeepsExistingData) {
  TwoTierLevels<Side::BID, 4, 8> bids;
  insert_new(bids, 100);
  bids.find(100)->head = reinterpret_cast<Order *>(0x1000);

  LevelData *d = bids.find_or_insert(100);
  ASSERT_NE(d, nullptr);
  EXPECT_EQ(d->total_qty, 100u);
  EXPECT_EQ(d->head, reinterpret_cast<Order *>(0x1000));
}

TEST(TwoTierLevels, FullSideRejectsNewPricesUntilOneIsErased) {
  TwoTierLevels<Side::BID, 2, 2> bids;
  for (uint32_t p : {100u, 99u, 98u, 97u}) {
    insert_new(bids, p);
  }

  EXPECT_EQ(bids.find_or_insert(96), nullptr);
  EXPECT_EQ(bids.find_or_insert(101), nullptr);
  EXPECT_EQ(bids.best_price(), 100u);
  for (uint32_t p : {97u, 98u, 99u, 100u}) {
    LevelData *d = bids.find_or_insert(p);
    ASSERT_NE(d, nullptr) << "price " << p;
    EXPECT_EQ(d->total_qty, p);
  }

  bids.erase(98);
  insert_new(bids, 95);
  expect_present(bids, {95, 97, 99, 100});
  expect_absent(bids, {96, 98, 101});
}

TEST(TwoTierLevels, MatchesModel) {
  run_against_model<Side::BID>(1);
  run_against_model<Side::ASK>(2);
}
