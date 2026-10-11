#pragma once

#include "logger.hpp"
#include "sys.hpp"
#include "timing.hpp"
#include <array>
#include <bit>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <linux/perf_event.h>
#include <pthread.h>
#include <stdexcept>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <x86intrin.h>

namespace profiling {

inline constexpr uint32_t ACCURACY_BITS = 4;
inline constexpr uint32_t RANGE_BITS = 32;
inline constexpr uint32_t SLICE_MASK = (uint32_t{1} << ACCURACY_BITS) - 1;
inline constexpr uint64_t EXACT_LIMIT = (uint64_t{1} << ACCURACY_BITS);
inline constexpr uint64_t VALUE_LIMIT = (uint64_t{1} << RANGE_BITS);
inline constexpr size_t MIN_BUCKETS = EXACT_LIMIT;
inline constexpr size_t MAX_BUCKETS = (RANGE_BITS - ACCURACY_BITS + 1)
                                      << ACCURACY_BITS;
inline constexpr uint8_t REPORT_AFTER_S = 5;

using time_point = timing::MonoClock::time_point;
using seconds = std::chrono::seconds;

struct ClockSample {
  uint64_t tsc;
  time_point mono;
};

inline ClockSample sample_clocks() noexcept {
  ClockSample best{};
  uint64_t best_width = std::numeric_limits<uint64_t>::max();
  for (int i = 0; i < 5; i++) {
    uint64_t before = __rdtsc();
    time_point mono = timing::MonoClock::now();
    uint64_t after = __rdtsc();
    if (after - before < best_width) {
      best_width = after - before;
      best = {before + best_width / 2, mono};
    }
  }

  return best;
}

inline double calibrate_ns_per_tick() noexcept {
  ClockSample start = sample_clocks();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  ClockSample end = sample_clocks();

  double ns =
      std::chrono::duration<double, std::nano>(end.mono - start.mono).count();
  return ns / static_cast<double>(end.tsc - start.tsc);
}

inline double ns_per_tick() noexcept {
  static const double value = calibrate_ns_per_tick();
  return value;
}

inline uint64_t report_interval_ticks() noexcept {
  double interval_ns =
      std::chrono::duration<double, std::nano>(seconds{REPORT_AFTER_S}).count();
  return static_cast<uint64_t>(interval_ns / ns_per_tick());
}

class Histogram {
  std::array<uint64_t, MAX_BUCKETS + 1> samples{};
  uint64_t total_samples{0};
  uint64_t min_value{std::numeric_limits<uint64_t>::max()};
  uint64_t max_value{0};

  size_t value_to_bucket(uint64_t value) {
    size_t bucket_idx;
    if (value < EXACT_LIMIT) {
      bucket_idx = value;
    } else if (value < VALUE_LIMIT) {
      auto range_bit = static_cast<uint32_t>(std::bit_width(value)) - 1;
      auto d = range_bit - ACCURACY_BITS;
      auto pos_in_range = value >> d;
      auto group = d << ACCURACY_BITS;
      bucket_idx = group + pos_in_range;
    } else {
      bucket_idx = MAX_BUCKETS; // overflow bucket.
    }

    return bucket_idx;
  }

  uint64_t bucket_to_value(size_t bucket) {
    uint64_t value;
    if (bucket < MIN_BUCKETS) {
      value = bucket;
    } else if (bucket < MAX_BUCKETS) {
      uint64_t slice = bucket & SLICE_MASK;
      uint32_t d = (static_cast<uint32_t>(bucket) >> ACCURACY_BITS) - 1u;
      uint64_t kept = (1 << ACCURACY_BITS) + slice;
      uint64_t bucket_max_width = uint64_t{1} << d;
      uint64_t value_min = kept << d; // fills the rest of the bits with 0s, the
                                      // information we masked out.
      value = value_min + bucket_max_width - 1;
    } else {
      value = VALUE_LIMIT;
    }

    return value;
  }

  template <double P> double percentile() {
    static constexpr auto PER_100K = static_cast<uint64_t>(P * 1000.0 + 0.5);

    uint64_t threshold =
        std::max<uint64_t>((total_samples * PER_100K + 99'999) / 100'000, 1);
    uint64_t samples_s = 0;
    for (size_t i = 0; i <= MAX_BUCKETS; i++) {
      samples_s += samples[i];
      if (samples_s >= threshold) {
        if (i == MAX_BUCKETS) {
          return std::numeric_limits<double>::infinity();
        }
        return static_cast<double>(std::min(bucket_to_value(i), max_value));
      }
    }

    return 0.0f;
  }

public:
  void record(uint64_t value) {
    min_value = std::min(min_value, value);
    max_value = std::max(max_value, value);

    ++samples[value_to_bucket(value)];
    ++total_samples;
  }

  void report(const char *name, double scale) {
    if (total_samples == 0) {
      return;
    }

    double min = std::round(static_cast<double>(min_value) * scale);
    double p50 = std::round(percentile<50.0>() * scale);
    double p99 = std::round(percentile<99.0>() * scale);
    double p99_9 = std::round(percentile<99.9>() * scale);
    double max = std::round(static_cast<double>(max_value) * scale);

    LOG_INFO("{}: n={}, min={}, p50={}, p99={}, p99_9={}, max={}", name,
             total_samples, min, p50, p99, p99_9, max);
  }
};

class CycleHistogram {
  Histogram histogram{};
  uint64_t next_report_tsc{0};

public:
  void update(uint64_t start, uint64_t end) {
    assert(end >= start);

    histogram.record(end - start);

    if (end >= next_report_tsc) [[unlikely]] {
      if (next_report_tsc != 0) {
        report();
      }
      next_report_tsc = end + report_interval_ticks();
    }
  }

  void report() { histogram.report("span_ns", ns_per_tick()); }

  void flush() { report(); }
};

constinit inline thread_local CycleHistogram hist{};

inline void flush_hist(void *args) {
  reinterpret_cast<CycleHistogram *>(args)->flush();
}

inline pthread_key_t get_pthread_key() {
  static pthread_key_t key = [] {
    pthread_key_t k;
    pthread_key_create(&k, flush_hist);
    return k;
  }();

  return key;
}

inline void thread_init() {
  pthread_key_t key = get_pthread_key();
  void *p = pthread_getspecific(key);
  if (!p) {
    pthread_setspecific(key, &hist);
  }
  ns_per_tick();
}

inline bool thread_is_init() {
  return pthread_getspecific(get_pthread_key()) != nullptr;
}

template <bool Fenced = false> class CycleProfiler {
private:
  // need something to hold the histograms.
  uint64_t start_;

  uint64_t tsc() {
    uint64_t result;
    if constexpr (Fenced) {
      unsigned int aux;
      result = __rdtscp(&aux);
      _mm_lfence();
    } else {
      result = __rdtsc();
    }

    return result;
  }

public:
  CycleProfiler() : start_(tsc()) {}
  ~CycleProfiler() {
    uint64_t end = tsc();
    hist.update(start_, end);
  }
};

struct EventInfo {
  const char *name;
  uint32_t type;
  uint64_t config;
};

static constexpr uint64_t hw_cache_config(uint8_t cache, uint8_t op,
                                          uint8_t result) {
  return static_cast<uint64_t>(cache) | (static_cast<uint64_t>(op) << 8) |
         (static_cast<uint64_t>(result) << 16);
}

enum class Event : uint8_t {
  CYCLES,
  INSTRUCTIONS,
  BRANCHES,
  BRANCH_MISSES,
  LLC_REFS,
  LLC_MISSES,
  L1D_LOADS,
  L1D_LOAD_MISSES,
  L1I_LOADS,
  L1I_LOAD_MISSES,
  ITLB_LOADS,
  ITLB_LOAD_MISSES,
  PAGE_FAULTS,
};

// TODO(kostas): Add event for HITM too.
inline constexpr auto EVENTS = std::to_array<EventInfo>(
    {{"cycles", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES},
     {"instructions", PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS},
     {"branch-instructions", PERF_TYPE_HARDWARE,
      PERF_COUNT_HW_BRANCH_INSTRUCTIONS},
     {"branch-misses", PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES},
     {"LLC-refs", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_REFERENCES},
     {"LLC-misses", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES},
     {"L1d-loads", PERF_TYPE_HW_CACHE,
      hw_cache_config(PERF_COUNT_HW_CACHE_L1D, PERF_COUNT_HW_CACHE_OP_READ,
                      PERF_COUNT_HW_CACHE_RESULT_ACCESS)},
     {"L1d-load-misses", PERF_TYPE_HW_CACHE,
      hw_cache_config(PERF_COUNT_HW_CACHE_L1D, PERF_COUNT_HW_CACHE_OP_READ,
                      PERF_COUNT_HW_CACHE_RESULT_MISS)},
     {"L1i-loads", PERF_TYPE_HW_CACHE,
      hw_cache_config(PERF_COUNT_HW_CACHE_L1I, PERF_COUNT_HW_CACHE_OP_READ,
                      PERF_COUNT_HW_CACHE_RESULT_ACCESS)},
     {"L1i-load-misses", PERF_TYPE_HW_CACHE,
      hw_cache_config(PERF_COUNT_HW_CACHE_L1I, PERF_COUNT_HW_CACHE_OP_READ,
                      PERF_COUNT_HW_CACHE_RESULT_MISS)},
     {"iTLB-loads", PERF_TYPE_HW_CACHE,
      hw_cache_config(PERF_COUNT_HW_CACHE_ITLB, PERF_COUNT_HW_CACHE_OP_READ,
                      PERF_COUNT_HW_CACHE_RESULT_ACCESS)},
     {"iTLB-load-misses", PERF_TYPE_HW_CACHE,
      hw_cache_config(PERF_COUNT_HW_CACHE_ITLB, PERF_COUNT_HW_CACHE_OP_READ,
                      PERF_COUNT_HW_CACHE_RESULT_MISS)},
     {"page-faults", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_PAGE_FAULTS}});

inline int perf_event_open(perf_event_attr *attr, pid_t pid, int cpu,
                           int group_fd, unsigned long flags) noexcept {
  return static_cast<int>(
      syscall(SYS_perf_event_open, attr, pid, cpu, group_fd, flags));
}

enum class PMUMode : uint8_t { READ, MMAP };

template <PMUMode M, size_t N> class PMUGroup {
public:
  using Counts = std::array<uint64_t, N>;

  struct Reading {
    Counts counts;
    uint64_t lost_time;
    bool counting;
  };

  class Span {
    PMUGroup &group_;
    Reading start_;

  public:
    explicit Span(PMUGroup &group) noexcept
        : group_(group), start_(group.read()) {}
    ~Span() { group_.record(start_, group_.read()); }
    Span(const Span &) = delete;
    Span &operator=(const Span &) = delete;
  };

  explicit PMUGroup(const std::array<Event, N> &events) {
    fds_.fill(-1);
    pages_.fill(nullptr);
    try {
      open_events(events);
    } catch (...) {
      release();
      throw;
    }
  }

  ~PMUGroup() {
    report();
    release();
  }

  PMUGroup(const PMUGroup &) = delete;
  PMUGroup &operator=(const PMUGroup &) = delete;

  [[nodiscard]] Span span() noexcept { return Span{*this}; }

  [[nodiscard]] Reading read() const noexcept {
    if constexpr (M == PMUMode::READ) {
      return read_syscall();
    } else {
      return read_rdpmc();
    }
  }

  void report() {
    for (size_t i = 0; i < N; i++) {
      histograms_[i].report(names_[i], 1.0);
    }
    if (dropped_ != 0) {
      LOG_WARN("pmu: {} spans dropped, the group was not counting", dropped_);
    }
  }

private:
  struct GroupRead {
    uint64_t nr;
    uint64_t time_enabled;
    uint64_t time_running;
    Counts values;
  };

  std::array<int, N> fds_;
  std::array<perf_event_mmap_page *, N> pages_;
  std::array<const char *, N> names_{};
  std::array<Histogram, N> histograms_{};
  size_t page_size_{0};
  uint64_t dropped_{0};

  static void barrier() noexcept { __asm__ volatile("" ::: "memory"); }

  void open_events(const std::array<Event, N> &events) {
    page_size_ = static_cast<size_t>(sysconf(_SC_PAGESIZE));

    for (size_t i = 0; i < N; i++) {
      const EventInfo &info = EVENTS[std::to_underlying(events[i])];
      names_[i] = info.name;

      perf_event_attr attr{};
      attr.size = sizeof(attr);
      attr.type = info.type;
      attr.config = info.config;
      attr.disabled = (i == 0);
      attr.exclude_kernel = 1;
      attr.exclude_hv = 1;
      if constexpr (M == PMUMode::READ) {
        attr.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED |
                           PERF_FORMAT_TOTAL_TIME_RUNNING;
      }

      int group_fd = i == 0 ? -1 : fds_[0];
      fds_[i] =
          check(perf_event_open(&attr, 0, -1, group_fd, PERF_FLAG_FD_CLOEXEC),
                std::format("perf_event_open({})", info.name));

      if constexpr (M == PMUMode::MMAP) {
        void *page =
            mmap(nullptr, page_size_, PROT_READ, MAP_SHARED, fds_[i], 0);
        if (page == MAP_FAILED) {
          throw std::system_error(errno, std::system_category(),
                                  std::format("mmap({})", info.name));
        }
        pages_[i] = static_cast<perf_event_mmap_page *>(page);
      }
    }

    check(ioctl(fds_[0], PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP),
          "PERF_EVENT_IOC_RESET");
    check(ioctl(fds_[0], PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP),
          "PERF_EVENT_IOC_ENABLE");

    if constexpr (M == PMUMode::MMAP) {
      for (size_t i = 0; i < N; i++) {
        if (!pages_[i]->cap_user_rdpmc) {
          throw std::runtime_error(
              std::format("rdpmc is not available for {}", names_[i]));
        }
      }
    }
  }

  void release() noexcept {
    for (perf_event_mmap_page *page : pages_) {
      if (page != nullptr) {
        munmap(page, page_size_);
      }
    }
    for (int fd : fds_) {
      if (fd >= 0) {
        close(fd);
      }
    }
  }

  Reading read_syscall() const noexcept {
    GroupRead raw{};
    ssize_t n = ::read(fds_[0], &raw, sizeof(raw));
    return {raw.values, raw.time_enabled - raw.time_running,
            n == static_cast<ssize_t>(sizeof(raw))};
  }

  // Seqlock protocol. Taken from include/uapi/linux/perf_event.h
  Reading read_rdpmc() const noexcept {
    Reading reading{};
    reading.counting = true;

    for (size_t i = 0; i < N; i++) {
      const perf_event_mmap_page *page = pages_[i];
      uint32_t seq;
      uint32_t idx;
      uint64_t lost_time;
      int64_t count;
      do {
        seq = __atomic_load_n(&page->lock, __ATOMIC_RELAXED);
        barrier();
        lost_time = __atomic_load_n(&page->time_enabled, __ATOMIC_RELAXED) -
                    __atomic_load_n(&page->time_running, __ATOMIC_RELAXED);
        idx = __atomic_load_n(&page->index, __ATOMIC_RELAXED);
        count = __atomic_load_n(&page->offset, __ATOMIC_RELAXED);
        if (idx != 0) {
          auto pmc = static_cast<int64_t>(_rdpmc(static_cast<int>(idx - 1)));
          int shift = 64 - __atomic_load_n(&page->pmc_width, __ATOMIC_RELAXED);
          pmc <<= shift;
          pmc >>= shift;
          count += pmc;
        }
        barrier();
      } while (__atomic_load_n(&page->lock, __ATOMIC_RELAXED) != seq);

      reading.counts[i] = static_cast<uint64_t>(count);
      reading.counting = reading.counting && idx != 0;
      if (i == 0) {
        reading.lost_time = lost_time;
      }
    }

    return reading;
  }

  void record(const Reading &start, const Reading &end) noexcept {
    if (!start.counting || !end.counting || start.lost_time != end.lost_time)
        [[unlikely]] {
      ++dropped_;
      return;
    }

    for (size_t i = 0; i < N; i++) {
      histograms_[i].record(end.counts[i] - start.counts[i]);
    }
  }
};

}; // namespace profiling
