#include "hash_map.hpp"
#include "order_book.hpp"
#include <algorithm>
#include <benchmark/benchmark.h>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <unordered_map>
#include <vector>

static constexpr size_t OB_POOL_SIZE = 4096;
static constexpr size_t OB_LIVE_COUNT = OB_POOL_SIZE / 2;

static std::vector<uint64_t> make_ob_pool() {
  std::mt19937_64 rng(42);
  std::vector<uint64_t> pool(OB_POOL_SIZE);
  for (auto &v : pool) {
    v = rng();
  }
  return pool;
}

static const std::vector<uint64_t> OB_POOL = make_ob_pool();

static uint32_t pool_price(size_t idx) {
  return 100 + static_cast<uint32_t>(OB_POOL[idx] % 512);
}

struct StlOrder {
  uint64_t order_num;
  uint32_t shares;
  uint32_t price;
  Side side;
};

class StlOrderBook {
  std::unordered_map<uint64_t, StlOrder *> orders_;
  std::map<uint32_t, std::deque<StlOrder *>> bids_;
  std::map<uint32_t, std::deque<StlOrder *>> asks_;
  mutable std::mutex mtx_;

public:
  bool add_order(uint64_t oid, uint32_t shares, uint32_t price, Side side) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (orders_.count(oid)) {
      return false;
    }
    auto *o = new StlOrder{oid, shares, price, side};
    orders_.emplace(oid, o);
    if (side == Side::BID) {
      bids_[price].push_back(o);
    } else {
      asks_[price].push_back(o);
    }
    return true;
  }

  bool cancel_order(uint64_t oid) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = orders_.find(oid);
    if (it == orders_.end()) {
      return false;
    }
    StlOrder *o = it->second;

    auto &levels = (o->side == Side::BID) ? bids_ : asks_;
    auto level_it = levels.find(o->price);
    auto &dq = level_it->second;
    dq.erase(std::find(dq.begin(), dq.end(), o));
    if (dq.empty()) {
      levels.erase(level_it);
    }

    orders_.erase(it);
    delete o;
    return true;
  }

  uint32_t best_price(Side side) const {
    std::lock_guard<std::mutex> lk(mtx_);
    if (side == Side::BID) {
      return bids_.empty() ? 0 : bids_.rbegin()->first;
    }
    return asks_.empty() ? 0 : asks_.begin()->first;
  }
};

static void BM_OrderBook_SteadyState(benchmark::State &state) {
  OrderBook book;
  book.reset(100, 1, 1024);
  for (size_t i = 0; i < OB_LIVE_COUNT; i++) {
    book.add_order(OB_POOL[i], 100, pool_price(i), Side::BID);
  }

  size_t i = 0;
  for (auto _ : state) {
    book.cancel_order(OB_POOL[i % OB_POOL_SIZE]);
    size_t next = (i + OB_LIVE_COUNT) % OB_POOL_SIZE;
    book.add_order(OB_POOL[next], 100, pool_price(next), Side::BID);
    i++;
  }
  benchmark::DoNotOptimize(i);
}
BENCHMARK(BM_OrderBook_SteadyState);

static void BM_OrderBook_BestPrice(benchmark::State &state) {
  OrderBook book;
  book.reset(100, 1, 1024);
  for (size_t i = 0; i < OB_LIVE_COUNT; i++) {
    book.add_order(OB_POOL[i], 100, pool_price(i), Side::BID);
  }

  for (auto _ : state) {
    benchmark::DoNotOptimize(book.best_price(Side::BID));
  }
}
BENCHMARK(BM_OrderBook_BestPrice);

static void BM_StlOrderBook_SteadyState(benchmark::State &state) {
  StlOrderBook book;
  for (size_t i = 0; i < OB_LIVE_COUNT; i++) {
    book.add_order(OB_POOL[i], 100, pool_price(i), Side::BID);
  }

  size_t i = 0;
  for (auto _ : state) {
    book.cancel_order(OB_POOL[i % OB_POOL_SIZE]);
    size_t next = (i + OB_LIVE_COUNT) % OB_POOL_SIZE;
    book.add_order(OB_POOL[next], 100, pool_price(next), Side::BID);
    i++;
  }
  benchmark::DoNotOptimize(i);
}
BENCHMARK(BM_StlOrderBook_SteadyState);

static void BM_StlOrderBook_BestPrice(benchmark::State &state) {
  StlOrderBook book;
  for (size_t i = 0; i < OB_LIVE_COUNT; i++) {
    book.add_order(OB_POOL[i], 100, pool_price(i), Side::BID);
  }

  for (auto _ : state) {
    benchmark::DoNotOptimize(book.best_price(Side::BID));
  }
}
BENCHMARK(BM_StlOrderBook_BestPrice);