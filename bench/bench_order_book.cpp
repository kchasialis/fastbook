#include "order_book.hpp"
#include <benchmark/benchmark.h>

static void BM_OrderBook_AddCancel(benchmark::State &state) {
  OrderBook book;
  book.reset(100, 1, 1024);

  uint64_t oid = 0;
  for (auto _ : state) {
    book.add_order(oid, 100, 100 + (oid % 512), Side::BID);
    book.cancel_order(oid++);
  }
  benchmark::DoNotOptimize(oid);
}
BENCHMARK(BM_OrderBook_AddCancel);

static void BM_OrderBook_BestPrice(benchmark::State &state) {
  OrderBook book;
  book.reset(100, 1, 1024);
  for (uint64_t i = 0; i < 512; ++i)
    book.add_order(i, 100, 100 + i, Side::BID);

  for (auto _ : state)
    benchmark::DoNotOptimize(book.best_price(Side::BID));
}
BENCHMARK(BM_OrderBook_BestPrice);
