#pragma once

#include "spsc_queue.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <iostream>
#include <iterator>
#include <print>
#include <string>
#include <string_view>
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

struct Descriptor;

struct RecordHeader {
  const Descriptor *desc;
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
using FormatFn = void (*)(std::string &out, const char *fmt,
                          const std::byte *payload);

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

static constexpr uint32_t N_SLOTS = 64;
static constexpr size_t LOG_RING_SIZE = 4096;
using LogRing = SPSCQueue<Record, LOG_RING_SIZE>;
struct alignas(std::hardware_destructive_interference_size) RingSlot {
  std::atomic<LogRing *> ring{nullptr};
  std::atomic<uint64_t> dropped{0};
};

inline std::array<RingSlot, N_SLOTS> slots{};
inline std::atomic<uint32_t> slots_so_far{0};
inline std::atomic<bool> stop{false};

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

  auto *ring = new LogRing();
  slots[current].ring.store(ring, std::memory_order_release);
  log_slot = &slots[current];
}

struct Descriptor {
  const char *fmt, *file;
  uint32_t line;
  Level level;
  FormatFn fn;
};

#define FASTBOOK_LOG(lvl, fmt_, ...)                                           \
  do {                                                                         \
    if constexpr (static_cast<int>(lvl) >= FASTBOOK_LOG_LEVEL) {               \
      static constexpr logger::Descriptor fastbook_log_desc_{                  \
          .fmt = fmt_,                                                         \
          .file = __FILE__,                                                    \
          .line = __LINE__,                                                    \
          .level = lvl,                                                        \
          .fn = logger::decoder_for<decltype(std::make_tuple(__VA_ARGS__))>};  \
      static_assert(fastbook_log_desc_.fn != nullptr);                         \
      logger::emit(&fastbook_log_desc_, fmt_ __VA_OPT__(, ) __VA_ARGS__);      \
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
void decode(std::string &out, const char *fmt, const std::byte *payload) {
  std::tuple<Args...> args;

  std::apply([&](auto &...a) { (read_one(payload, a), ...); }, args);

  std::apply(
      [&](auto &...a) {
        std::vformat_to(std::back_inserter(out), fmt,
                        std::make_format_args(a...));
      },
      args);
}

template <class... Args>
void emit(const Descriptor *desc, std::format_string<Args...> fmt,
          Args &&...args) {
  static_assert((min_wire_size<Args> + ... + 0) <= PAYLOAD_BYTES,
                "log call has too many arguments for one record");

  if (!log_slot) [[unlikely]] {
    std::print(stderr, "[{}] {}:{} ", level_to_str(desc->level), desc->file,
               desc->line);
    std::println(stderr, fmt, std::forward<Args>(args)...);
    return;
  }

  Record record;

  record.hdr.tsc = __rdtsc();
  record.hdr.desc = desc;

  [[maybe_unused]] size_t off = 0;
  (write_one(record.payload, off, static_cast<Stored_t<Args>>(args)), ...);

  LogRing *ring = log_slot->ring.load(std::memory_order_relaxed);
  if (!ring->producer().push(std::move(record))) {
    // Only this thread writes its own counter, so a load + store is better
    // than RMW because RMW drains the store buffer (lock instruction on x86).
    std::atomic<uint64_t> &d = log_slot->dropped;
    d.store(d.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
  }
}

inline void write_all(std::string_view buf) noexcept {
  size_t off = 0;
  while (off < buf.size()) {
    ssize_t n = ::write(STDERR_FILENO, buf.data() + off, buf.size() - off);
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
    const Descriptor *desc = rec.hdr.desc;
    out.clear();
    try {
      desc->fn(out, desc->fmt, rec.payload);
    } catch (...) {
      failed++;
      continue;
    }
    std::format_to(std::back_inserter(out_buff), "[{}] {}:{} {}\n",
                   level_to_str(desc->level), desc->file, desc->line, out);
  }

  if (failed > 0) {
    std::format_to(std::back_inserter(out_buff),
                   "[ERROR] logger: {} records failed to format\n", failed);
  }

  write_all(out_buff);

  return !recs.empty();
}

inline void logger_thread() {
  std::vector<Record> recs;
  recs.reserve(LOG_RING_SIZE);
  std::string out_buff;
  out_buff.reserve(64 * 1024);
  std::string out;
  out.reserve(256);
  std::array<uint64_t, N_SLOTS> seen_dropped{};

  while (!stop.load(std::memory_order_acquire)) {
    if (!drain_and_log(recs, out_buff, out, seen_dropped)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  drain_and_log(recs, out_buff, out, seen_dropped);
}

template <class Tuple> inline constexpr FormatFn decoder_for = nullptr;

template <class... Args>
inline constexpr FormatFn decoder_for<std::tuple<Args...>> =
    &decode<Stored_t<Args>...>;

} // namespace logger