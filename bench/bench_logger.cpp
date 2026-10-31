#include "bench_utils.hpp"
#include "logger.hpp"
#include "profiling.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <format>
#include <fstream>
#include <getopt.h>
#include <optional>
#include <print>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

struct ToPrint {
  int x;
  double y;
};

constexpr size_t MAX_ELEM = 4096;
constexpr const char *FMT_C = "%s:%d fastbook: %d, %f\n";
constexpr size_t BATCH = 512;
constexpr size_t WARMUP = 1000;
constexpr double PAUSE_NS = 5e6;

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

FILE *SINK_FILE = nullptr;
int SINK_FD = -1;

bool open_sink(const char *path) {
  SINK_FD = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (SINK_FD < 0) {
    return false;
  }

  SINK_FILE = ::fdopen(::dup(SINK_FD), "a");

  return SINK_FILE != nullptr && logger::open_output(path);
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

uint64_t timer_overhead() {
  std::vector<uint64_t> v(1 << 20);
  for (auto &x : v) {
    uint64_t t0 = profiling::read_tsc<true>();
    uint64_t t1 = profiling::read_tsc<true>();
    x = t1 - t0;
  }
  std::ranges::sort(v);
  return v[v.size() / 2];
}

uint64_t ns_to_ticks(double ns) {
  return static_cast<uint64_t>(ns / profiling::ns_per_tick());
}

void spin_until(uint64_t tsc) {
  while (profiling::read_tsc() < tsc) {
  }
}

using Pmu = profiling::PMUGroup<profiling::PMUMode::READ, 5>;

constexpr std::array<profiling::Event, 5> PMU_EVENTS{
    profiling::Event::CYCLES, profiling::Event::INSTRUCTIONS,
    profiling::Event::AMD_L2_FILLS, profiling::Event::AMD_L3_FILLS,
    profiling::Event::AMD_DRAM_FILLS};

struct Run {
  profiling::Histogram hist;
  uint64_t ticks = 0;
  uint64_t calls = 0;
  uint64_t drops = 0;
  uint64_t elapsed = 0;
  Pmu::Counts counts{};
  bool exact = true;

  void add(const Pmu::Reading &before, const Pmu::Reading &after) {
    for (size_t i = 0; i < counts.size(); i++) {
      counts[i] += after.counts[i] - before.counts[i];
    }
    exact = exact && before.counting && after.counting &&
            before.lost_time == after.lost_time;
  }
};

Pmu::Reading read(Pmu *pmu) { return pmu ? pmu->read() : Pmu::Reading{}; }

template <class F>
void timed_call(F &body, uint64_t overhead, Run &r) {
  const ToPrint &e = ELEMS[r.calls & (MAX_ELEM - 1)];
  uint64_t t0 = profiling::read_tsc<true>();
  body(e);
  uint64_t t1 = profiling::read_tsc<true>();
  uint64_t d = t1 - t0 > overhead ? t1 - t0 - overhead : 0;
  r.hist.record(d);
  r.ticks += d;
  r.calls++;
}

template <class F>
Run measure_paced(F &&body, uint64_t samples, uint64_t overhead, Pmu *pmu) {
  uint64_t pause = ns_to_ticks(PAUSE_NS);
  for (size_t i = 0; i < WARMUP; i++) {
    body(ELEMS[i & (MAX_ELEM - 1)]);
  }
  spin_until(profiling::read_tsc() + pause);

  Run r;
  uint64_t drops0 = total_dropped();
  while (r.calls < samples) {
    uint64_t n = std::min<uint64_t>(BATCH, samples - r.calls);
    Pmu::Reading before = read(pmu);
    uint64_t t0 = profiling::read_tsc();
    for (uint64_t i = 0; i < n; i++) {
      timed_call(body, overhead, r);
    }
    r.elapsed += profiling::read_tsc() - t0;
    r.add(before, read(pmu));
    spin_until(profiling::read_tsc() + pause);
  }
  r.drops = total_dropped() - drops0;

  return r;
}

Run measure_rate(uint64_t rate, double seconds, uint64_t overhead, Pmu *pmu) {
  auto body = [](const ToPrint &e) { LOG_INFO("fastbook: {}, {}", e.x, e.y); };
  for (size_t i = 0; i < WARMUP; i++) {
    body(ELEMS[i & (MAX_ELEM - 1)]);
  }
  spin_until(profiling::read_tsc() + ns_to_ticks(PAUSE_NS));

  uint64_t gap = rate ? ns_to_ticks(1e9 / static_cast<double>(rate)) : 0;
  Run r;
  uint64_t drops0 = total_dropped();
  Pmu::Reading before = read(pmu);
  uint64_t start = profiling::read_tsc();
  uint64_t end = start + ns_to_ticks(seconds * 1e9);
  for (uint64_t next = start; profiling::read_tsc() < end; next += gap) {
    spin_until(next);
    timed_call(body, overhead, r);
  }
  r.elapsed = profiling::read_tsc() - start;
  r.add(before, read(pmu));
  r.drops = total_dropped() - drops0;

  return r;
}

struct Options {
  std::string mode = "compare";
  const char *log_file = "bench_log.txt";
  const char *perf_csv = nullptr;
  int hot_cpu = 3;
  int log_cpu = 4;
  uint64_t samples = 1'000'000;
  uint64_t rate = 0;
  double seconds = 2.0;
};

void row(const Options &o, std::string_view name, Run &r, uint64_t overhead) {
  double ns_per_tick = profiling::ns_per_tick();
  double achieved = static_cast<double>(r.calls) /
                    (static_cast<double>(r.elapsed) * ns_per_tick * 1e-9);
  std::println("{},{},{},{},{},{:.0f},{},{},{:.2f},{:.0f},{:.0f},{:.0f},{},{},{:."
               "4f}",
               o.mode, name, o.hot_cpu, o.log_cpu, o.rate, achieved, r.calls,
               r.drops,
               static_cast<double>(r.ticks) / static_cast<double>(r.calls),
               r.hist.percentile<50.0>(), r.hist.percentile<99.0>(),
               r.hist.percentile<99.9>(), r.hist.max(), overhead, ns_per_tick);
}

void write_counters(const Options &o, std::string_view name, const Run &r) {
  std::FILE *f = std::fopen(o.perf_csv, "a");
  if (f == nullptr) {
    std::println(stderr, "cannot open {}", o.perf_csv);
    return;
  }
  for (size_t i = 0; i < PMU_EVENTS.size(); i++) {
    const char *event =
        profiling::EVENTS[std::to_underlying(PMU_EVENTS[i])].name;
    std::println(f, "{},{},{},{},{},{},{},{}", o.mode, name, o.log_cpu, o.rate,
                 r.calls, event, r.counts[i], r.exact ? "exact" : "scaled");
  }
  std::fclose(f);
}

void usage(const char *prog) {
  std::println(stderr,
               "Usage: {} [--mode compare|rate] [--samples N] [--rate CALLS_PER_S]"
               " [--seconds S] [--hot-cpu C] [--log-cpu C] [--log-file PATH]"
               " [--perf-csv FILE] [--header]",
               prog);
}

} // namespace

int main(int argc, char **argv) {
  Options o;
  static option long_opts[] = {{"mode", required_argument, nullptr, 'm'},
                               {"samples", required_argument, nullptr, 'n'},
                               {"rate", required_argument, nullptr, 'r'},
                               {"seconds", required_argument, nullptr, 's'},
                               {"hot-cpu", required_argument, nullptr, 'h'},
                               {"log-cpu", required_argument, nullptr, 'l'},
                               {"log-file", required_argument, nullptr, 'f'},
                               {"perf-csv", required_argument, nullptr, 'P'},
                               {"header", no_argument, nullptr, 'H'},
                               {nullptr, 0, nullptr, 0}};
  int c;
  while ((c = getopt_long(argc, argv, "", long_opts, nullptr)) != -1) {
    switch (c) {
    case 'm':
      o.mode = optarg;
      break;
    case 'n':
      o.samples = std::strtoull(optarg, nullptr, 10);
      break;
    case 'r':
      o.rate = std::strtoull(optarg, nullptr, 10);
      break;
    case 's':
      o.seconds = std::strtod(optarg, nullptr);
      break;
    case 'h':
      o.hot_cpu = std::atoi(optarg);
      break;
    case 'l':
      o.log_cpu = std::atoi(optarg);
      break;
    case 'f':
      o.log_file = optarg;
      break;
    case 'P':
      o.perf_csv = optarg;
      break;
    case 'H':
      std::println("mode,contender,hot_cpu,log_cpu,offered_rate,achieved_rate,"
                   "calls,drops,mean_ticks,p50_ticks,p99_ticks,p999_ticks,"
                   "max_ticks,overhead_ticks,ns_per_tick");
      return 0;
    default:
      usage(argv[0]);
      return 1;
    }
  }
  if (o.mode != "compare" && o.mode != "rate") {
    usage(argv[0]);
    return 1;
  }

  pin_to_core(o.hot_cpu);
  if (!open_sink(o.log_file)) {
    std::println(stderr, "cannot open {}", o.log_file);
    return 1;
  }

  std::jthread backend([cpu = o.log_cpu](std::stop_token st) {
    pin_to_core(cpu);
    logger::logger_thread(st);
  });
  logger::thread_init();

  profiling::ns_per_tick();
  uint64_t overhead = timer_overhead();

  std::optional<Pmu> pmu;
  if (o.perf_csv != nullptr) {
    pmu.emplace(PMU_EVENTS);
  }
  Pmu *p = pmu ? &*pmu : nullptr;

  std::vector<std::pair<std::string_view, Run>> runs;
  if (o.mode == "compare") {
    std::ofstream log_stream(o.log_file, std::ios::app);
    std::string out;
    out.reserve(256);
    const std::string prerendered =
        std::format("{}:{} fastbook: pre-rendered\n", __FILE__, __LINE__);

    runs.emplace_back("empty",
                      measure_paced([](const ToPrint &) {}, o.samples,
                                    overhead, p));
    runs.emplace_back(
        "logger",
        measure_paced(
            [](const ToPrint &e) { LOG_INFO("fastbook: {}, {}", e.x, e.y); },
            o.samples, overhead, p));
    runs.emplace_back(
        "write", measure_paced(
                     [&](const ToPrint &) {
                       std::ignore = ::write(SINK_FD, prerendered.data(),
                                             prerendered.size());
                     },
                     o.samples, overhead, p));
    runs.emplace_back(
        "fprintf", measure_paced(
                       [](const ToPrint &e) {
                         std::fprintf(SINK_FILE, FMT_C, __FILE__, __LINE__,
                                      e.x, e.y);
                       },
                       o.samples, overhead, p));
    runs.emplace_back(
        "ofstream", measure_paced(
                        [&](const ToPrint &e) {
                          log_stream << __FILE__ << ':' << __LINE__
                                     << " fastbook: " << e.x << ", " << e.y
                                     << '\n';
                        },
                        o.samples, overhead, p));
    runs.emplace_back(
        "format_to", measure_paced(
                         [&](const ToPrint &e) {
                           out.clear();
                           std::format_to(std::back_inserter(out),
                                          "{}:{} fastbook: {}, {}\n", __FILE__,
                                          __LINE__, e.x, e.y);
                         },
                         o.samples, overhead, p));
  } else {
    runs.emplace_back("logger", measure_rate(o.rate, o.seconds, overhead, p));
  }

  backend.request_stop();
  backend.join();

  for (auto &[name, r] : runs) {
    row(o, name, r, overhead);
    if (p != nullptr) {
      write_counters(o, name, r);
    }
  }

  logger::close_output();
  std::fclose(SINK_FILE);
  ::close(SINK_FD);
  ::unlink(o.log_file);

  return 0;
}
