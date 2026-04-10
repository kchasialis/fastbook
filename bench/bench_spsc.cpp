#include "bench_utils.hpp"
#include "spsc_queue.hpp"
#include <benchmark/benchmark.h>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>

static void BM_SPSC_Throughput(benchmark::State &state) {
  pin_to_core(0);

  SPSCQueue<uint64_t, 65536> q;
  auto prod = q.producer();
  auto cons = q.consumer();
  const int64_t n = state.range(0);

  std::atomic<int64_t> to_produce{0};
  std::atomic<bool> done{false};

  std::thread producer([&] {
    pin_to_core(1);
    while (!done.load(std::memory_order_relaxed)) {
      int64_t batch = to_produce.exchange(0, std::memory_order_acquire);
      for (int64_t i = 0; i < batch; i++) {
        while (!prod.push(static_cast<uint64_t>(i))) {
        }
      }
    }
  });

  for (auto _ : state) {
    to_produce.store(n, std::memory_order_release);

    uint64_t sink = 0;
    for (int64_t i = 0; i < n; i++) {
      std::optional<uint64_t> v;
      while (!(v = cons.pop())) {
      }
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
  pin_to_core(0);

  SPSCQueue<uint64_t, 2> q_ping, q_pong;
  auto ping_prod = q_ping.producer();
  auto ping_cons = q_ping.consumer();
  auto pong_prod = q_pong.producer();
  auto pong_cons = q_pong.consumer();

  std::atomic<bool> done{false};
  std::thread responder([&] {
    pin_to_core(1);
    while (!done.load(std::memory_order_relaxed)) {
      auto v = ping_cons.pop();
      if (v)
        pong_prod.push(*v);
    }
  });

  std::vector<int64_t> latencies;
  latencies.reserve(state.max_iterations);

  for (auto _ : state) {
    auto t0 = now_ns();
    while (!ping_prod.push(1UL)) {
    }
    while (!pong_cons.pop()) {
    }
    latencies.push_back(now_ns() - t0);
  }

  done.store(true, std::memory_order_relaxed);
  responder.join();

  auto [p50, p99] = percentiles(latencies);
  state.counters["p50_ns"] = p50;
  state.counters["p99_ns"] = p99;
}
BENCHMARK(BM_SPSC_Latency)->UseRealTime();

static void BM_Mutex_Throughput(benchmark::State &state) {
  pin_to_core(0);

  std::queue<uint64_t> q;
  std::mutex mtx;
  const int64_t n = state.range(0);

  std::atomic<int64_t> to_produce{0};
  std::atomic<bool> done{false};

  std::thread producer([&] {
    pin_to_core(1);
    while (!done.load(std::memory_order_relaxed)) {
      int64_t batch = to_produce.exchange(0, std::memory_order_acquire);
      for (int64_t i = 0; i < batch; i++) {
        std::lock_guard<std::mutex> lk(mtx);
        q.push(static_cast<uint64_t>(i));
      }
    }
  });

  for (auto _ : state) {
    to_produce.store(n, std::memory_order_release);

    uint64_t sink = 0;
    for (int64_t i = 0; i < n; i++) {
      uint64_t v;
      while (true) {
        std::lock_guard<std::mutex> lk(mtx);
        if (!q.empty()) {
          v = q.front();
          q.pop();
          break;
        }
      }
      sink ^= v;
    }
    benchmark::DoNotOptimize(sink);
  }

  done.store(true, std::memory_order_relaxed);
  producer.join();
  state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_Mutex_Throughput)->Arg(1 << 16)->UseRealTime();

static void BM_Mutex_Latency(benchmark::State &state) {
  pin_to_core(0);

  std::queue<uint64_t> q_ping, q_pong;
  std::mutex mtx_ping, mtx_pong;

  std::atomic<bool> done{false};
  std::thread responder([&] {
    pin_to_core(1);
    while (!done.load(std::memory_order_relaxed)) {
      uint64_t v;
      {
        std::lock_guard<std::mutex> lk(mtx_ping);
        if (q_ping.empty())
          continue;
        v = q_ping.front();
        q_ping.pop();
      }
      std::lock_guard<std::mutex> lk(mtx_pong);
      q_pong.push(v);
    }
  });

  std::vector<int64_t> latencies;
  latencies.reserve(state.max_iterations);

  for (auto _ : state) {
    auto t0 = now_ns();
    {
      std::lock_guard<std::mutex> lk(mtx_ping);
      q_ping.push(1UL);
    }
    while (true) {
      std::lock_guard<std::mutex> lk(mtx_pong);
      if (!q_pong.empty()) {
        q_pong.pop();
        break;
      }
    }
    latencies.push_back(now_ns() - t0);
  }

  done.store(true, std::memory_order_relaxed);
  responder.join();

  auto [p50, p99] = percentiles(latencies);
  state.counters["p50_ns"] = p50;
  state.counters["p99_ns"] = p99;
}
BENCHMARK(BM_Mutex_Latency)->UseRealTime();
