#include "spsc_queue.hpp"
#include <benchmark/benchmark.h>
#include <optional>
#include <thread>

static void BM_SPSC_Throughput(benchmark::State &state) {
  SPSCQueue<uint64_t, 4096> q;
  auto prod = q.producer();
  auto cons = q.consumer();
  const int64_t n = state.range(0);

  std::atomic<int64_t> to_produce{0};
  std::atomic<bool> done{false};

  std::thread producer([&] {
    while (!done.load(std::memory_order_relaxed)) {
      int64_t batch = to_produce.exchange(0, std::memory_order_acquire);
      for (int64_t i = 0; i < batch; ++i)
        while (!prod.push(static_cast<uint64_t>(i)))
          ;
    }
  });

  for (auto _ : state) {
    to_produce.store(n, std::memory_order_release);

    uint64_t sink = 0;
    for (int64_t i = 0; i < n; ++i) {
      std::optional<uint64_t> v;
      while (!(v = cons.pop()))
        ;
      sink ^= *v;
    }
    benchmark::DoNotOptimize(sink);
  }

  done.store(true, std::memory_order_relaxed);
  producer.join();
  state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_SPSC_Throughput)->Arg(1 << 16)->UseRealTime();

static void BM_SPSC_Latency(benchmark::State &state) {
  SPSCQueue<uint64_t, 2> q_ping, q_pong;
  auto ping_prod = q_ping.producer();
  auto ping_cons = q_ping.consumer();
  auto pong_prod = q_pong.producer();
  auto pong_cons = q_pong.consumer();

  std::atomic<bool> done{false};
  std::thread responder([&] {
    while (!done.load(std::memory_order_relaxed)) {
      auto v = ping_cons.pop();
      if (v)
        pong_prod.push(*v);
    }
  });

  for (auto _ : state) {
    while (!ping_prod.push(1UL))
      ;
    while (!pong_cons.pop())
      ;
  }

  done.store(true, std::memory_order_relaxed);
  responder.join();
}
BENCHMARK(BM_SPSC_Latency)->UseRealTime();
