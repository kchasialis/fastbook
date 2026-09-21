#include "bench_utils.hpp"
#include "logger.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <fstream>
#include <print>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <vector>

namespace {

struct ToPrint {
  int x;
  double y;
};

constexpr size_t MAX_ELEM = 4096;
constexpr const char *FMT_C = "%s:%d fastbook: %d, %f\n";

std::vector<ToPrint> make_elems() {
  std::mt19937 rng(1234);
  std::uniform_real_distribution<double> dist(0.0, 1.0);

  std::vector<ToPrint> elems;
  elems.reserve(MAX_ELEM);
  for (size_t i = 0; i < MAX_ELEM; i++) {
    elems.push_back(
        ToPrint{static_cast<int>(i), static_cast<double>(i) + dist(rng)});
  }

  return elems;
}

const std::vector<ToPrint> ELEMS = make_elems();

constexpr const char *LOG_PATH = "bench_log.txt";

FILE *SINK_FILE = nullptr;
int SINK_FD = -1;

bool open_sink() {
  SINK_FD = ::open(LOG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (SINK_FD < 0) {
    return false;
  }

  SINK_FILE = ::fdopen(::dup(SINK_FD), "a");

  return SINK_FILE != nullptr && logger::open_output(LOG_PATH);
}

uint64_t total_dropped() {
  uint64_t total = 0;
  uint32_t n = std::min(logger::slots_so_far.load(std::memory_order_relaxed),
                        static_cast<uint32_t>(logger::N_SLOTS));
  for (uint32_t i = 0; i < n; i++) {
    total += logger::slots[i].dropped.load(std::memory_order_relaxed);
  }

  return total;
}

double measure_tsc_hz() {
  using clock = std::chrono::steady_clock;
  const auto wall_start = clock::now();
  const uint64_t tsc_start = __rdtsc();

  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  const uint64_t tsc_end = __rdtsc();
  const auto wall_end = clock::now();

  const double seconds =
      std::chrono::duration<double>(wall_end - wall_start).count();

  return static_cast<double>(tsc_end - tsc_start) / seconds;
}

int64_t measure_pair_cost(size_t n) {
  int64_t best = INT64_MAX;
  for (size_t i = 0; i < n; i++) {
    auto [t0, aux0] = rdtscp_start();
    auto [t1, aux1] = rdtscp_end();
    if (aux0 == aux1) {
      best = std::min(best, static_cast<int64_t>(t1 - t0));
    }
  }

  return best == INT64_MAX ? 0 : best;
}

struct Stats {
  size_t n{0};
  int64_t min{0};
  int64_t p50{0};
  int64_t p90{0};
  int64_t p99{0};
  int64_t p999{0};
  int64_t max{0};
  double mean{0.0};
  uint64_t drops{0};
};

int64_t quantile(const std::vector<int64_t> &sorted, double q) {
  if (sorted.empty()) {
    return 0;
  }
  const size_t idx =
      static_cast<size_t>(static_cast<double>(sorted.size() - 1) * q);
  return sorted[idx];
}

constexpr size_t BATCH = 512;
constexpr auto DRAIN_PAUSE = std::chrono::milliseconds(5);

template <typename F>
Stats measure(F &&body, size_t n_samples, int64_t pair_cost,
              std::vector<int64_t> &samples) {
  samples.clear();

  const uint64_t drops_before = total_dropped();

  for (size_t done = 0; done < n_samples; done += BATCH) {
    const size_t n = std::min(BATCH, n_samples - done);
    for (size_t i = 0; i < n; i++) {
      auto [t0, aux0] = rdtscp_start();
      body(ELEMS[(done + i) & (MAX_ELEM - 1)]);
      auto [t1, aux1] = rdtscp_end();

      if (aux0 == aux1) {
        samples.push_back(static_cast<int64_t>(t1 - t0) - pair_cost);
      }
    }

    std::this_thread::sleep_for(DRAIN_PAUSE);
  }

  std::sort(samples.begin(), samples.end());

  Stats s;
  s.n = samples.size();
  s.drops = total_dropped() - drops_before;
  if (samples.empty()) {
    return s;
  }

  s.min = samples.front();
  s.p50 = quantile(samples, 0.50);
  s.p90 = quantile(samples, 0.90);
  s.p99 = quantile(samples, 0.99);
  s.p999 = quantile(samples, 0.999);
  s.max = samples.back();

  int64_t sum = 0;
  for (int64_t v : samples) {
    sum += v;
  }
  s.mean = static_cast<double>(sum) / static_cast<double>(samples.size());

  return s;
}

void print_row(std::string_view name, const Stats &s, double ns_per_cycle) {
  std::print("{:<12} {:>8} {:>8} {:>8} {:>8} {:>9} {:>9} {:>10.1f} {:>8}\n",
             name, s.min, s.p50, s.p90, s.p99, s.p999, s.max,
             static_cast<double>(s.p50) * ns_per_cycle, s.drops);
}

bool parse_uint(std::string_view v, uint64_t &out) {
  const char *end = v.data() + v.size();
  auto [ptr, ec] = std::from_chars(v.data(), end, out);
  return ec == std::errc{} && ptr == end;
}

} // namespace

int main(int argc, char **argv) {
  uint64_t n_samples = 200000;

  for (int i = 1; i < argc - 1; i++) {
    const std::string_view arg = argv[i];
    if (arg == "--samples" && !parse_uint(argv[i + 1], n_samples)) {
      std::println(stdout, "bad --samples");
      return 1;
    }
  }

  if (!open_sink()) {
    std::println(stdout, "cannot open {}", LOG_PATH);
    return 1;
  }

  pin_to_core(1);

  // Cold thread on core 0, this one on core 1: neither preempts the other.
  std::thread backend(logger::logger_thread, 0);
  logger::thread_init();

  const double tsc_hz = measure_tsc_hz();
  const double ns_per_cycle = 1e9 / tsc_hz;
  const int64_t pair_cost = measure_pair_cost(100000);

  std::ofstream log_stream(LOG_PATH, std::ios::app);

  std::string out;
  out.reserve(256);
  const std::string prerendered =
      std::format("{}:{} fastbook: pre-rendered\n", __FILE__, __LINE__);

  std::vector<int64_t> samples;
  samples.reserve(n_samples);

  // Warm up
  for (size_t i = 0; i < 1000; i++) {
    const ToPrint &e = ELEMS[i & (MAX_ELEM - 1)];
    LOG_INFO("fastbook: {}, {}", e.x, e.y);
    std::ignore = ::write(SINK_FD, prerendered.data(), prerendered.size());
    std::fprintf(SINK_FILE, FMT_C, __FILE__, __LINE__, e.x, e.y);
    log_stream << "warmup " << e.x << '\n';
    out.clear();
    std::format_to(std::back_inserter(out), "{}:{} fastbook: {}, {}\n",
                   __FILE__, __LINE__, e.x, e.y);
  }

  const Stats s_logger =
      measure([](const ToPrint &e) { LOG_INFO("fastbook: {}, {}", e.x, e.y); },
              n_samples, pair_cost, samples);

  const Stats s_write = measure(
      [&](const ToPrint &) {
        std::ignore = ::write(SINK_FD, prerendered.data(), prerendered.size());
      },
      n_samples, pair_cost, samples);

  const Stats s_fprintf = measure(
      [](const ToPrint &e) {
        std::fprintf(SINK_FILE, FMT_C, __FILE__, __LINE__, e.x, e.y);
      },
      n_samples, pair_cost, samples);

  const Stats s_stream = measure(
      [&](const ToPrint &e) {
        log_stream << __FILE__ << ':' << __LINE__ << " fastbook: " << e.x
                   << ", " << e.y << '\n';
      },
      n_samples, pair_cost, samples);

  const Stats s_format = measure(
      [&](const ToPrint &e) {
        out.clear();
        std::format_to(std::back_inserter(out), "{}:{} fastbook: {}, {}\n",
                       __FILE__, __LINE__, e.x, e.y);
      },
      n_samples, pair_cost, samples);

  logger::stop.store(true, std::memory_order_release);
  backend.join();

  std::print(stdout,
             "\nsamples={} batch={} tsc={:.3f} GHz clock_pair={} cycles\n\n",
             n_samples, BATCH, tsc_hz / 1e9, pair_cost);
  std::print(stdout,
             "{:<12} {:>8} {:>8} {:>8} {:>8} {:>9} {:>9} {:>10} {:>8}\n",
             "contender", "min", "p50", "p90", "p99", "p99.9", "max",
             "p50 (ns)", "drops");

  print_row("logger", s_logger, ns_per_cycle);
  print_row("write", s_write, ns_per_cycle);
  print_row("fprintf", s_fprintf, ns_per_cycle);
  print_row("ofstream", s_stream, ns_per_cycle);
  print_row("format_to", s_format, ns_per_cycle);

  logger::close_output();
  std::fclose(SINK_FILE);
  ::close(SINK_FD);
  ::unlink(LOG_PATH);

  return 0;
}
