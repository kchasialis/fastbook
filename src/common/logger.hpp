#pragma once

#include "spsc_queue.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <iostream>
#include <iterator>
#include <print>
#include <stop_token>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unistd.h>
#include <vector>
#include <x86intrin.h>

namespace logger {

enum class Level : uint8_t { TRACE, DEBUG, INFO, WARN, ERROR, FATAL };

constexpr std::string_view level_to_str(Level lvl) {
  switch (lvl) {
  case Level::TRACE:
    return "TRACE";
  case Level::DEBUG:
    return "DEBUG";
  case Level::INFO:
    return "INFO";
  case Level::WARN:
    return "WARN";
  case Level::ERROR:
    return "ERROR";
  case Level::FATAL:
    return "FATAL";
  }
  return "UNKNOWN";
}

#ifndef FASTBOOK_LOG_LEVEL
#define FASTBOOK_LOG_LEVEL 2
#endif

#define LOG_TRACE(fmt, ...)                                                    \
  FASTBOOK_LOG(logger::Level::TRACE, fmt __VA_OPT__(, ) __VA_ARGS__)
#define LOG_DEBUG(fmt, ...)                                                    \
  FASTBOOK_LOG(logger::Level::DEBUG, fmt __VA_OPT__(, ) __VA_ARGS__)
#define LOG_INFO(fmt, ...)                                                     \
  FASTBOOK_LOG(logger::Level::INFO, fmt __VA_OPT__(, ) __VA_ARGS__)
#define LOG_WARN(fmt, ...)                                                     \
  FASTBOOK_LOG(logger::Level::WARN, fmt __VA_OPT__(, ) __VA_ARGS__)
#define LOG_ERROR(fmt, ...)                                                    \
  FASTBOOK_LOG(logger::Level::ERROR, fmt __VA_OPT__(, ) __VA_ARGS__)
#define LOG_FATAL(fmt, ...)                                                    \
  FASTBOOK_LOG(logger::Level::FATAL, fmt __VA_OPT__(, ) __VA_ARGS__)

struct LogMetadata;

struct RecordHeader {
  const LogMetadata *meta;
  uint64_t tsc;
};

inline constexpr size_t RECORD_BYTES = 256;
inline constexpr size_t PAYLOAD_BYTES = RECORD_BYTES - sizeof(RecordHeader);

struct Record {
  RecordHeader hdr;
  std::byte payload[PAYLOAD_BYTES];
};
static_assert(sizeof(Record) == RECORD_BYTES);

using StrLen = std::conditional_t<PAYLOAD_BYTES <= 255, uint8_t, uint16_t>;
using FormatFn = void (*)(const std::byte *payload, std::string &out);

template <typename T> struct Stored {
  static_assert(std::is_trivially_copyable_v<T>,
                "log arguments must be trivially copyable or string-like");
  using Type = T;
};

template <> struct Stored<const char *> {
  using Type = std::string_view;
};

template <> struct Stored<char *> {
  using Type = std::string_view;
};

template <> struct Stored<std::string> {
  using Type = std::string_view;
};

template <typename T> using Stored_t = typename Stored<std::decay_t<T>>::Type;

template <class T>
inline constexpr size_t min_wire_size =
    std::is_same_v<Stored_t<T>, std::string_view> ? sizeof(StrLen)
                                                  : sizeof(Stored_t<T>);

static constexpr size_t HUGE_PAGE_BYTES = 2 * 1024 * 1024;
static constexpr uint32_t N_SLOTS = 64;
static constexpr size_t LOG_RING_SIZE = 4096;
using LogRing = SPSCQueue<Record, LOG_RING_SIZE>;
struct alignas(std::hardware_destructive_interference_size) RingSlot {
  std::atomic<LogRing *> ring{nullptr};
  std::atomic<uint64_t> dropped{0};
};

inline std::array<RingSlot, N_SLOTS> slots{};
inline std::atomic<uint32_t> slots_so_far{0};

inline thread_local RingSlot *log_slot = nullptr;

inline void thread_init() {
  if (log_slot) {
    return;
  }

  uint32_t current = slots_so_far.fetch_add(1, std::memory_order_relaxed);
  if (current >= N_SLOTS) [[unlikely]] {
    // Cannot use LOG here it will recurse infinitely.
    std::cerr << "All slots are claimed, this thread logs via std::print"
              << std::endl;
    return;
  }

  // The ring is streamed through sequentially, and with 4 KiB pages every
  // sixteenth record (16 x 256 B) starts a new page: measured at ~7x the
  // median cost, because neither the hardware prefetcher nor the TLB follows
  // us across the boundary. One 2 MiB huge page removes 511 of every 512
  // boundaries, so the allocation is aligned and sized to a huge page and
  // MADV_HUGEPAGE asks for one (the kernel is in madvise mode by default).
  // Populate the page tables now, so the producer never takes a minor fault
  // mid session: a fault costs microseconds and lands on the hot thread.
  // POPULATE_WRITE prefaults the pages.
  static constexpr size_t RING_BYTES =
      ((sizeof(LogRing) + HUGE_PAGE_BYTES - 1) / HUGE_PAGE_BYTES) *
      HUGE_PAGE_BYTES;

  void *mem = std::aligned_alloc(HUGE_PAGE_BYTES, RING_BYTES);
  if (!mem) [[unlikely]] {
    std::cerr << "logger: cannot allocate a ring" << std::endl;
    return;
  }

  (void)::madvise(mem, RING_BYTES, MADV_HUGEPAGE);
  (void)::madvise(mem, RING_BYTES, MADV_POPULATE_WRITE);

  auto *ring = new (mem) LogRing();

  slots[current].ring.store(ring, std::memory_order_release);
  log_slot = &slots[current];
}

struct LogMetadata {
  const char *fmt, *file;
  uint32_t line;
  Level level;
  FormatFn fn;
};

#define FASTBOOK_LOG(lvl, fmt_, ...)                                           \
  do {                                                                         \
    if constexpr (static_cast<int>(lvl) >= FASTBOOK_LOG_LEVEL) {               \
      static constexpr logger::LogMetadata fastbook_log_meta_{                 \
          .fmt = fmt_,                                                         \
          .file = __FILE__,                                                    \
          .line = __LINE__,                                                    \
          .level = lvl,                                                        \
          .fn = logger::decoder_for<logger::constant_string{fmt_},             \
                                    decltype(std::make_tuple(__VA_ARGS__))>};  \
      static_assert(fastbook_log_meta_.fn != nullptr);                         \
      logger::emit(&fastbook_log_meta_, fmt_ __VA_OPT__(, ) __VA_ARGS__);      \
    }                                                                          \
  } while (0)

template <typename T> void read_one(const std::byte *&payload, T &a) {
  if constexpr (std::is_same_v<T, std::string_view>) {
    StrLen len;
    std::memcpy(&len, payload, sizeof(len));
    payload += sizeof(len);
    a = std::string_view(reinterpret_cast<const char *>(payload), len);
    payload += len;
  } else {
    std::memcpy(&a, payload, sizeof(a));
    payload += sizeof(a);
  }
}

template <typename T> void write_one(std::byte *payload, size_t &off, T a) {
  if constexpr (std::is_same_v<T, std::string_view>) {
    StrLen len = static_cast<StrLen>(
        std::min(a.size(), PAYLOAD_BYTES - off - sizeof(StrLen)));
    std::memcpy(payload + off, &len, sizeof(len));
    off += sizeof(len);
    std::memcpy(payload + off, a.data(), len);
    off += len;
  } else {
    std::memcpy(payload + off, &a, sizeof(T));
    off += sizeof(T);
  }
}

template <class... Args>
void emit(const LogMetadata *meta, std::format_string<Args...> fmt,
          Args &&...args) {
  static_assert((min_wire_size<Args> + ... + 0) <= PAYLOAD_BYTES,
                "log call has too many arguments for one record");

  if (!log_slot) [[unlikely]] {
    std::print(stderr, "[{}] {}:{} ", level_to_str(meta->level), meta->file,
               meta->line);
    std::println(stderr, fmt, std::forward<Args>(args)...);
    return;
  }

  LogRing *ring = log_slot->ring.load(std::memory_order_relaxed);
  auto prod = ring->producer();
  auto *slot = prod.try_reserve();
  if (!slot) [[unlikely]] {
    std::atomic<uint64_t> &d = log_slot->dropped;
    // Only this thread writes its own counter, so a load + store is better
    // than RMW because RMW drains the store buffer (lock instruction on x86).
    d.store(d.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    return;
  }

  auto *record = new (slot) Record;
  record->hdr.tsc = __rdtsc();
  record->hdr.meta = meta;

  [[maybe_unused]] size_t off = 0;
  (write_one(record->payload, off, static_cast<Stored_t<Args>>(args)), ...);

  prod.commit();
}

// Where the cold thread writes. Defaults to stderr.
inline std::atomic<int> out_fd{STDERR_FILENO};

inline bool open_output(const char *path) noexcept {
  int fd = ::open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd < 0) {
    return false;
  }

  int old = out_fd.exchange(fd, std::memory_order_release);
  if (old != STDERR_FILENO) {
    ::close(old);
  }

  return true;
}

inline void close_output() noexcept {
  int old = out_fd.exchange(STDERR_FILENO, std::memory_order_release);
  if (old != STDERR_FILENO) {
    ::close(old);
  }
}

inline void write_all(std::string_view buf) noexcept {
  const int fd = out_fd.load(std::memory_order_acquire);
  size_t off = 0;
  while (off < buf.size()) {
    ssize_t n = ::write(fd, buf.data() + off, buf.size() - off);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return;
    }
    off += static_cast<size_t>(n);
  }
}

inline bool drain_and_log(std::vector<Record> &recs, std::string &out_buff,
                          std::string &out,
                          std::array<uint64_t, N_SLOTS> &seen_dropped) {
  recs.clear();
  out_buff.clear();

  uint32_t n_slots = std::min(slots_so_far.load(std::memory_order_relaxed),
                              static_cast<uint32_t>(N_SLOTS));
  for (uint32_t i = 0; i < n_slots; i++) {
    LogRing *ring = slots[i].ring.load(std::memory_order_acquire);
    if (!ring) {
      continue;
    }

    auto consumer = ring->consumer();
    for (size_t k = 0; k < LOG_RING_SIZE; k++) {
      std::optional<Record> rec = consumer.pop();
      if (!rec.has_value()) {
        break;
      }
      recs.push_back(*rec);
    }

    uint64_t dropped = slots[i].dropped.load(std::memory_order_relaxed);
    if (dropped != seen_dropped[i]) {
      std::format_to(std::back_inserter(out_buff),
                     "[WARN] logger: slot {} dropped {} records\n", i,
                     dropped - seen_dropped[i]);
      seen_dropped[i] = dropped;
    }
  }

  std::sort(recs.begin(), recs.end(), [](const Record &a, const Record &b) {
    return a.hdr.tsc < b.hdr.tsc;
  });

  uint64_t failed = 0;
  for (const Record &rec : recs) {
    const LogMetadata *meta = rec.hdr.meta;
    out.clear();
    try {
      meta->fn(rec.payload, out);
    } catch (...) {
      failed++;
      continue;
    }
    std::format_to(std::back_inserter(out_buff), "[{}] {}:{} {}\n",
                   level_to_str(meta->level), meta->file, meta->line, out);
  }

  if (failed > 0) {
    std::format_to(std::back_inserter(out_buff),
                   "[ERROR] logger: {} records failed to format\n", failed);
  }

  write_all(out_buff);

  return !recs.empty();
}

inline void logger_thread(std::stop_token st) {
  std::vector<Record> recs;
  recs.reserve(LOG_RING_SIZE);
  std::string out_buff;
  out_buff.reserve(64 * 1024);
  std::string out;
  out.reserve(256);
  std::array<uint64_t, N_SLOTS> seen_dropped{};

  while (!st.stop_requested()) {
    if (!drain_and_log(recs, out_buff, out, seen_dropped)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  drain_and_log(recs, out_buff, out, seen_dropped);
}

template <size_t N> struct constant_string {
  char chars[N];
  consteval constant_string(const char (&s)[N]) {
    // copy_n instead of memcpy because copy_n is constexpr.
    std::copy_n(s, N, chars);
  }

  // N - 1 because it includes the '\0'
  consteval size_t size() const { return N - 1; }
};

template <size_t N>
consteval size_t count_fields(const constant_string<N> &fmt) {
  size_t n = 0;
  for (size_t i = 0; i + 1 < fmt.size(); i++) {
    if (fmt.chars[i] == '{' && fmt.chars[i + 1] == '}') {
      ++n;
      ++i;
    }
  }
  return n;
}

template <size_t K, size_t N>
consteval std::array<std::string_view, K + 1>
split(const constant_string<N> &fmt) {
  std::array<std::string_view, K + 1> segments{};

  size_t current_seg = 0;
  size_t current_start = 0;
  for (size_t i = 0; i < fmt.size(); i++) {
    if (fmt.chars[i] == '{') {
      if (i + 1 == fmt.size() || fmt.chars[i + 1] != '}') {
        throw "unsupported format: unclosed '{'";
      }
      size_t len = i - current_start;
      segments[current_seg++] =
          std::string_view(fmt.chars + current_start, len);
      current_start = i + 2;
      i++;
    } else if (fmt.chars[i] == '}') {
      throw "unsupported format: unclosed '}'";
    }
  }
  segments[K] =
      std::string_view(fmt.chars + current_start, fmt.size() - current_start);

  return segments;
}

template <class T> constexpr void format_arg(const T &arg, std::string &out) {
  if constexpr (std::is_same_v<T, std::string_view>) {
    out.append(arg);
  } else if constexpr (std::is_same_v<T, bool>) {
    out.append(arg ? "true" : "false");
  } else if constexpr (std::is_same_v<T, char>) {
    out.push_back(arg);
  } else if constexpr (std::is_arithmetic_v<T>) {
    char buf[48];
    [[maybe_unused]] auto [ptr, ec] =
        std::to_chars(buf, buf + sizeof(buf), arg);
    assert(ec == std::errc{});
    out.append(buf, static_cast<size_t>(ptr - buf));
  } else {
    static_assert(false, "format_arg: unsupported type");
  }
}

template <constant_string Fmt, class... Stored>
void decode(const std::byte *payload, std::string &out) {
  static constexpr size_t K = count_fields(Fmt);
  static_assert(K == sizeof...(Stored),
                "format string / argument count mismatch");
  static constexpr auto segs = split<K>(Fmt);

  std::tuple<Stored...> args;

  std::apply([&](auto &...a) { (read_one(payload, a), ...); }, args);

  [&]<size_t... I>(std::index_sequence<I...>) {
    ((out += segs[I], format_arg(std::get<I>(args), out)), ...);
  }(std::index_sequence_for<Stored...>{});
  out += segs[K];
}

template <constant_string Fmt, class Tuple>
inline constexpr FormatFn decoder_for = nullptr;

template <constant_string Fmt, class... Args>
inline constexpr FormatFn decoder_for<Fmt, std::tuple<Args...>> =
    &decode<Fmt, Stored_t<Args>...>;

} // namespace logger