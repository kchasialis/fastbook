#include "itch_parser.hpp"
#include "linear_probing_table.hpp"
#include "profiling.hpp"
#include "swiss_table.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <getopt.h>
#include <malloc.h>
#include <map>
#include <optional>
#include <print>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace {

enum class OpKind : uint8_t { Insert, Find, Erase };
constexpr std::array<std::string_view, 3> OP_NAMES{"insert", "find", "erase"};

struct Op {
  uint64_t key;
  OpKind kind;
};

struct Trace {
  std::vector<Op> warmup;
  std::vector<Op> measured;
  size_t live = 0;
};

int dummy;
using Val = int *;

struct StdUnorderedMap {
  std::unordered_map<uint64_t, Val> m;
  explicit StdUnorderedMap(size_t live) { m.reserve(live); }
  bool insert(uint64_t k) { return m.emplace(k, &dummy).second; }
  Val find(uint64_t k) {
    auto it = m.find(k);
    return it == m.end() ? nullptr : it->second;
  }
  bool erase(uint64_t k) { return m.erase(k) != 0; }
};

struct StdMap {
  std::map<uint64_t, Val> m;
  explicit StdMap(size_t) {}
  bool insert(uint64_t k) { return m.emplace(k, &dummy).second; }
  Val find(uint64_t k) {
    auto it = m.find(k);
    return it == m.end() ? nullptr : it->second;
  }
  bool erase(uint64_t k) { return m.erase(k) != 0; }
};

template <EraseMode M> struct Linear {
  LinearProbingTable<uint64_t, Val, FibHash<uint64_t>, M> t;
  explicit Linear(size_t live) : t(std::bit_ceil(live)) {}
  bool insert(uint64_t k) { return t.insert(k, &dummy); }
  Val find(uint64_t k) { return t.find(k); }
  bool erase(uint64_t k) { return t.erase(k); }
};

template <swiss::Group G> struct Swiss {
  SwissTable<uint64_t, Val, FibHash<uint64_t>, G> t;
  explicit Swiss(size_t live) : t(2 * std::bit_ceil(live)) {}
  bool insert(uint64_t k) { return t.insert(k, &dummy); }
  Val find(uint64_t k) { return t.find(k).value_or(nullptr); }
  bool erase(uint64_t k) { return t.erase(k); }
};

template <class T> size_t apply(T &t, const Op &op) {
  switch (op.kind) {
  case OpKind::Insert:
    return t.insert(op.key);
  case OpKind::Find:
    return t.find(op.key) != nullptr;
  case OpKind::Erase:
    return t.erase(op.key);
  }
  __builtin_unreachable();
}

using Clock = std::chrono::steady_clock;
constexpr size_t CHUNK = 1 << 16;

template <class T>
bool run_plain(T &t, std::span<const Op> ops, Clock::time_point deadline,
               uint64_t &ok) {
  for (size_t i = 0; i < ops.size(); i += CHUNK) {
    if (Clock::now() > deadline) {
      return false;
    }
    size_t end = std::min(ops.size(), i + CHUNK);
    for (size_t j = i; j < end; j++) {
      ok += apply(t, ops[j]);
    }
  }
  return true;
}

struct OpLatency {
  profiling::Histogram hist;
  uint64_t ticks = 0;
};

using Latencies = std::array<OpLatency, 3>;

template <class T>
bool run_timed(T &t, std::span<const Op> ops, Clock::time_point deadline,
               uint64_t overhead, uint64_t &ok, Latencies &lat) {
  for (size_t i = 0; i < ops.size(); i += CHUNK) {
    if (Clock::now() > deadline) {
      return false;
    }
    size_t end = std::min(ops.size(), i + CHUNK);
    for (size_t j = i; j < end; j++) {
      uint64_t t0 = profiling::read_tsc<true>();
      ok += apply(t, ops[j]);
      uint64_t t1 = profiling::read_tsc<true>();
      uint64_t d = t1 - t0 > overhead ? t1 - t0 - overhead : 0;
      OpLatency &l = lat[static_cast<size_t>(ops[j].kind)];
      l.hist.record(d);
      l.ticks += d;
    }
  }
  return true;
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

using Pmu = profiling::PMUGroup<profiling::PMUMode::READ, 7>;

constexpr std::array<profiling::Event, 7> PMU_EVENTS{
    profiling::Event::CYCLES,         profiling::Event::INSTRUCTIONS,
    profiling::Event::AMD_L2_FILLS,   profiling::Event::AMD_L3_FILLS,
    profiling::Event::AMD_DRAM_FILLS, profiling::Event::AMD_DTLB_MISSES,
    profiling::Event::PAGE_FAULTS};

size_t heap_bytes() {
  struct mallinfo2 mi = mallinfo2();
  return mi.uordblks + mi.hblkhd;
}

Trace make_churn(size_t live, size_t steps, uint64_t seed) {
  std::mt19937_64 rng(seed);
  uint64_t next = 0;
  auto new_key = [&] {
    next += 1 + (rng() & 7);
    return next;
  };

  Trace tr;
  tr.live = live;
  std::vector<uint64_t> keys(live);
  for (auto &k : keys) {
    k = new_key();
    tr.warmup.push_back({k, OpKind::Insert});
  }

  auto step = [&](std::vector<Op> &out) {
    out.push_back({keys[rng() % live], OpKind::Find});
    size_t victim = rng() % live;
    out.push_back({keys[victim], OpKind::Erase});
    keys[victim] = new_key();
    out.push_back({keys[victim], OpKind::Insert});
  };

  size_t warm_steps = std::min(2 * live, size_t{1} << 22);
  for (size_t i = 0; i < warm_steps; i++) {
    step(tr.warmup);
  }
  tr.measured.reserve(3 * steps);
  for (size_t i = 0; i < steps; i++) {
    step(tr.measured);
  }
  return tr;
}

struct TraceBuilder {
  std::vector<Op> ops;
  std::unordered_map<uint64_t, uint32_t> live;
  size_t peak = 0;

  void add(uint64_t oid, uint32_t qty) {
    live.emplace(oid, qty);
    ops.push_back({oid, OpKind::Insert});
    peak = std::max(peak, live.size());
  }

  void on_event(const MboEvent &e) {
    switch (e.etype) {
    case EventType::ADDED:
      add(e.oid, e.qty);
      break;
    case EventType::EXECUTED:
    case EventType::CANCELLED: {
      auto it = live.find(e.oid);
      if (it == live.end()) {
        return;
      }
      ops.push_back({e.oid, OpKind::Find});
      if (e.qty >= it->second) {
        live.erase(it);
        ops.push_back({e.oid, OpKind::Erase});
      } else {
        it->second -= e.qty;
      }
      break;
    }
    case EventType::DELETED:
      if (live.erase(e.oid) != 0) {
        ops.push_back({e.oid, OpKind::Erase});
      }
      break;
    case EventType::REPLACED:
      if (live.erase(e.orig_oid) == 0) {
        return;
      }
      ops.push_back({e.orig_oid, OpKind::Erase});
      add(e.oid, e.qty);
      break;
    }
  }
};

constexpr uint64_t TRACE_MAGIC = 0x46424b5452414345;

int make_itch_trace(const char *capture, const char *out) {
  int fd = open(capture, O_RDONLY);
  struct stat st{};
  if (fd < 0 || fstat(fd, &st) != 0) {
    std::println(stderr, "cannot open {}", capture);
    return 1;
  }
  auto size = static_cast<size_t>(st.st_size);
  void *m = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (m == MAP_FAILED) {
    std::println(stderr, "cannot map {}", capture);
    return 1;
  }
  madvise(m, size, MADV_SEQUENTIAL);

  TraceBuilder b;
  const auto *p = static_cast<const std::byte *>(m);
  const std::byte *end = p + size;
  while (end - p >= 2) {
    size_t len =
        (std::to_integer<size_t>(p[0]) << 8) | std::to_integer<size_t>(p[1]);
    p += 2;
    if (static_cast<size_t>(end - p) < len) {
      break;
    }
    ITCHParser::parse({p, len}, b);
    p += len;
  }
  munmap(m, size);

  std::ofstream f(out, std::ios::binary);
  uint64_t header[3] = {TRACE_MAGIC, b.peak, b.ops.size()};
  f.write(reinterpret_cast<const char *>(header), sizeof(header));
  f.write(reinterpret_cast<const char *>(b.ops.data()),
          static_cast<std::streamsize>(b.ops.size() * sizeof(Op)));

  std::array<size_t, 3> counts{};
  for (const Op &op : b.ops) {
    counts[static_cast<size_t>(op.kind)]++;
  }
  std::println(stderr,
               "{} ops (insert {}, find {}, erase {}), peak live orders {}, "
               "live at end {}",
               b.ops.size(), counts[0], counts[1], counts[2], b.peak,
               b.live.size());
  return f.good() ? 0 : 1;
}

bool load_itch_trace(const char *path, Trace &tr) {
  std::ifstream f(path, std::ios::binary);
  uint64_t header[3];
  f.read(reinterpret_cast<char *>(header), sizeof(header));
  if (!f || header[0] != TRACE_MAGIC) {
    return false;
  }
  tr.live = header[1];
  tr.measured.resize(header[2]);
  f.read(reinterpret_cast<char *>(tr.measured.data()),
         static_cast<std::streamsize>(header[2] * sizeof(Op)));
  return static_cast<bool>(f);
}

struct Options {
  std::string table = "swiss";
  std::string workload = "churn";
  std::string mode = "tp";
  const char *trace = nullptr;
  const char *perf_csv = nullptr;
  size_t live = 1 << 16;
  size_t steps = 2'000'000;
  uint64_t seed = 42;
  unsigned budget_s = 120;
};

struct Stats {
  double mean, p50, p99, p999, max;
};

Stats summarize(OpLatency &l, double ns_per_tick) {
  profiling::Histogram &h = l.hist;
  double n = static_cast<double>(h.count());
  return {static_cast<double>(l.ticks) / n * ns_per_tick,
          h.percentile<50.0>() * ns_per_tick,
          h.percentile<99.0>() * ns_per_tick,
          h.percentile<99.9>() * ns_per_tick,
          static_cast<double>(h.max()) * ns_per_tick};
}

void write_counters(const Options &o, size_t live, const Pmu::Reading &before,
                    const Pmu::Reading &after) {
  std::FILE *f = std::fopen(o.perf_csv, "a");
  if (f == nullptr) {
    std::println(stderr, "cannot open {}", o.perf_csv);
    return;
  }
  bool exact =
      before.counting && after.counting && before.lost_time == after.lost_time;
  for (size_t i = 0; i < PMU_EVENTS.size(); i++) {
    const char *name =
        profiling::EVENTS[std::to_underlying(PMU_EVENTS[i])].name;
    std::println(f, "{},{},{},{},{},{}", o.table, o.workload, live, name,
                 after.counts[i] - before.counts[i],
                 exact ? "exact" : "scaled");
  }
  std::fclose(f);
}

void row(const Options &o, size_t live, std::string_view op, size_t count,
         const Stats &s, double table_mb, std::string_view status) {
  std::println("{},{},{},{},{},{},{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{:.1f},{}",
               o.table, o.workload, live, o.mode, op, count, s.mean, s.p50,
               s.p99, s.p999, s.max, table_mb, status);
}

template <class T> int bench(const Options &o, const Trace &tr) {
  double ns_per_tick = profiling::ns_per_tick();
  uint64_t overhead = o.mode == "lat" ? timer_overhead() : 0;

  size_t heap0 = heap_bytes();
  T table(tr.live);
  uint64_t ok = 0;
  auto far = Clock::now() + std::chrono::hours(24);
  run_plain(table, tr.warmup, far, ok);
  double table_mb = static_cast<double>(heap_bytes() - heap0) / (1 << 20);
  if (ok != tr.warmup.size()) {
    std::println(stderr, "warmup: {} of {} ops failed", tr.warmup.size() - ok,
                 tr.warmup.size());
    return 1;
  }

  ok = 0;
  auto deadline = Clock::now() + std::chrono::seconds(o.budget_s);
  bool finished;
  Latencies lat;
  std::optional<Pmu> pmu;
  if (o.perf_csv != nullptr) {
    pmu.emplace(PMU_EVENTS);
  }
  Pmu::Reading before = pmu ? pmu->read() : Pmu::Reading{};
  auto t0 = Clock::now();
  if (o.mode == "lat") {
    finished = run_timed(table, tr.measured, deadline, overhead, ok, lat);
  } else {
    finished = run_plain(table, tr.measured, deadline, ok);
  }
  auto t1 = Clock::now();
  Pmu::Reading after = pmu ? pmu->read() : Pmu::Reading{};
  if (pmu) {
    write_counters(o, tr.live, before, after);
  }

  std::string_view status = finished ? "ok" : "timeout";
  if (finished && ok != tr.measured.size()) {
    std::println(stderr, "measured: {} of {} ops failed",
                 tr.measured.size() - ok, tr.measured.size());
    status = "wrong";
  }

  if (o.mode == "lat") {
    OpLatency all;
    for (size_t k = 0; k < lat.size(); k++) {
      all.hist.merge(lat[k].hist);
      all.ticks += lat[k].ticks;
      if (lat[k].hist.count() != 0) {
        row(o, tr.live, OP_NAMES[k], lat[k].hist.count(),
            summarize(lat[k], ns_per_tick), table_mb, status);
      }
    }
    row(o, tr.live, "all", all.hist.count(), summarize(all, ns_per_tick),
        table_mb, status);
  } else {
    size_t done = finished ? tr.measured.size() : ok;
    double ns =
        static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                .count()) /
        static_cast<double>(done);
    row(o, tr.live, "all", done, {ns, 0, 0, 0, 0}, table_mb, status);
  }
  return 0;
}

void usage(const char *prog) {
  std::println(stderr,
               "Usage: {} [--table T] [--workload churn|itch] [--mode tp|lat]"
               " [--live N] [--steps N] [--trace FILE] [--seed N]"
               " [--budget SECONDS] [--perf-csv FILE] [--header]\n"
               "       {} --make-trace CAPTURE OUT\n"
               "tables: umap map lp lp-tomb swiss swiss-sse2 swiss-swar "
               "swiss-scalar",
               prog, prog);
}

} // namespace

int main(int argc, char *argv[]) {
  if (argc == 4 && std::string_view(argv[1]) == "--make-trace") {
    return make_itch_trace(argv[2], argv[3]);
  }

  Options o;
  static option long_opts[] = {{"table", required_argument, nullptr, 't'},
                               {"workload", required_argument, nullptr, 'w'},
                               {"mode", required_argument, nullptr, 'm'},
                               {"live", required_argument, nullptr, 'n'},
                               {"steps", required_argument, nullptr, 's'},
                               {"trace", required_argument, nullptr, 'f'},
                               {"seed", required_argument, nullptr, 'r'},
                               {"budget", required_argument, nullptr, 'b'},
                               {"perf-csv", required_argument, nullptr, 'P'},
                               {"header", no_argument, nullptr, 'H'},
                               {nullptr, 0, nullptr, 0}};
  int c;
  while ((c = getopt_long(argc, argv, "", long_opts, nullptr)) != -1) {
    switch (c) {
    case 't':
      o.table = optarg;
      break;
    case 'w':
      o.workload = optarg;
      break;
    case 'm':
      o.mode = optarg;
      break;
    case 'n':
      o.live = std::strtoull(optarg, nullptr, 10);
      break;
    case 's':
      o.steps = std::strtoull(optarg, nullptr, 10);
      break;
    case 'f':
      o.trace = optarg;
      break;
    case 'r':
      o.seed = std::strtoull(optarg, nullptr, 10);
      break;
    case 'b':
      o.budget_s = static_cast<unsigned>(std::strtoul(optarg, nullptr, 10));
      break;
    case 'P':
      o.perf_csv = optarg;
      break;
    case 'H':
      std::println("table,workload,live,mode,op,count,mean_ns,p50_ns,p99_ns,"
                   "p999_ns,max_ns,table_mb,status");
      return 0;
    default:
      usage(argv[0]);
      return 1;
    }
  }

  Trace tr;
  if (o.workload == "itch") {
    if (!o.trace || !load_itch_trace(o.trace, tr)) {
      std::println(stderr, "cannot load trace '{}'", o.trace ? o.trace : "");
      return 1;
    }
  } else if (o.workload == "churn") {
    tr = make_churn(o.live, o.steps, o.seed);
  } else {
    usage(argv[0]);
    return 1;
  }

  const std::string &t = o.table;
  if (t == "umap") {
    return bench<StdUnorderedMap>(o, tr);
  }
  if (t == "map") {
    return bench<StdMap>(o, tr);
  }
  if (t == "lp") {
    return bench<Linear<EraseMode::BackwardShift>>(o, tr);
  }
  if (t == "lp-tomb") {
    return bench<Linear<EraseMode::Tombstone>>(o, tr);
  }
  if (t == "swiss") {
    return bench<Swiss<swiss::DefaultGroup>>(o, tr);
  }
  if (t == "swiss-swar") {
    return bench<Swiss<swiss::SwarGroup>>(o, tr);
  }
  if (t == "swiss-scalar") {
    return bench<Swiss<swiss::ScalarGroup>>(o, tr);
  }
#if defined(__SSE2__)
  if (t == "swiss-sse2") {
    return bench<Swiss<swiss::Sse2Group>>(o, tr);
  }
#endif
  usage(argv[0]);
  return 1;
}
