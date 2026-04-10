#include "bench_utils.hpp"
#include "hash_map.hpp"
#include <benchmark/benchmark.h>
#include <ctime>
#include <random>
#include <unordered_map>
#include <vector>

static constexpr size_t POOL_SIZE = 4096;
static constexpr size_t LIVE_COUNT = POOL_SIZE / 2;

static std::vector<uint64_t> make_pool() {
  std::mt19937_64 rng(static_cast<uint64_t>(std::time(nullptr)));
  std::vector<uint64_t> pool(POOL_SIZE);
  for (auto &v : pool) {
    v = rng();
  }
  return pool;
}

static const std::vector<uint64_t> POOL = make_pool();

static int dummy = 0;

static void BM_HashMap_Find(benchmark::State &state) {
  HashMap<uint64_t, int *> map(POOL_SIZE);
  for (size_t i = 0; i < LIVE_COUNT; i++) {
    map.insert(POOL[i], &dummy);
  }

  size_t i = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(map.find(POOL[i % LIVE_COUNT]));
    i++;
  }
}
BENCHMARK(BM_HashMap_Find);

static void BM_UnorderedMap_Find(benchmark::State &state) {
  std::unordered_map<uint64_t, int *> map;
  map.reserve(POOL_SIZE);
  for (size_t i = 0; i < LIVE_COUNT; i++) {
    map.emplace(POOL[i], &dummy);
  }

  size_t i = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(map.find(POOL[i % LIVE_COUNT]));
    i++;
  }
}
BENCHMARK(BM_UnorderedMap_Find);

static void BM_HashMap_SteadyState(benchmark::State &state) {
  HashMap<uint64_t, int *> map(POOL_SIZE);
  for (size_t i = 0; i < LIVE_COUNT; i++) {
    map.insert(POOL[i], &dummy);
  }

  size_t i = 0;
  for (auto _ : state) {
    map.erase(POOL[i % POOL_SIZE]);
    map.insert(POOL[(i + LIVE_COUNT) % POOL_SIZE], &dummy);
    i++;
  }
  benchmark::DoNotOptimize(i);
}
BENCHMARK(BM_HashMap_SteadyState);

static void BM_UnorderedMap_SteadyState(benchmark::State &state) {
  std::unordered_map<uint64_t, int *> map;
  map.reserve(POOL_SIZE);
  for (size_t i = 0; i < LIVE_COUNT; i++) {
    map.emplace(POOL[i], &dummy);
  }

  size_t i = 0;
  for (auto _ : state) {
    map.erase(POOL[i % POOL_SIZE]);
    map.emplace(POOL[(i + LIVE_COUNT) % POOL_SIZE], &dummy);
    i++;
  }
  benchmark::DoNotOptimize(i);
}
BENCHMARK(BM_UnorderedMap_SteadyState);