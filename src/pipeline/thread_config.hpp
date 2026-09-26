#pragma once

#include "event_queue.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <istream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <vector>

// Which core each hot thread is pinned to. File format, one entry per line:
//
//   # comment
//   feed: 2
//   builder: 3
//   builder: 4
//   builder: 5
//   builder: 6
//
struct ThreadConfig {
  uint32_t feed_core{};
  std::array<uint32_t, N_SHARDS> builder_cores{};

  static ThreadConfig load(const std::filesystem::path &path) {
    std::ifstream file(path);
    if (!file.is_open()) {
      throw std::runtime_error(
          std::format("failed to open thread config: {}", path.string()));
    }

    ThreadConfig cfg = parse(file, path.string());
    cfg.validate(path.string());
    return cfg;
  }

  static ThreadConfig parse(std::istream &in, std::string_view source) {
    auto fail = [source](size_t lineno, std::string_view why) {
      return std::runtime_error(std::format("{}:{}: {}", source, lineno, why));
    };

    std::optional<uint32_t> feed;
    std::vector<uint32_t> builders;
    builders.reserve(N_SHARDS);

    std::string line;
    size_t lineno = 0;
    while (std::getline(in, line)) {
      ++lineno;

      std::string_view entry = trim(line);
      if (entry.empty() || entry.front() == '#') {
        continue;
      }

      size_t colon = entry.find(':');
      if (colon == std::string_view::npos) {
        throw fail(lineno, "expected '<role>: <core>'");
      }
      std::string_view role = trim(entry.substr(0, colon));
      std::string_view value = trim(entry.substr(colon + 1));

      uint32_t core{};
      const char *end = value.data() + value.size();
      auto [ptr, ec] = std::from_chars(value.data(), end, core);
      if (value.empty() || ec != std::errc{} || ptr != end) {
        throw fail(lineno, std::format("invalid core number '{}'", value));
      }

      if (role == "feed") {
        if (feed.has_value()) {
          throw fail(lineno, "duplicate 'feed' entry");
        }
        feed = core;
      } else if (role == "builder") {
        if (builders.size() == N_SHARDS) {
          throw fail(lineno, std::format("more than N_SHARDS ({}) 'builder' "
                                         "entries",
                                         N_SHARDS));
        }
        builders.push_back(core);
      } else {
        throw fail(lineno, std::format("unknown role '{}', expected 'feed' or "
                                       "'builder'",
                                       role));
      }
    }

    if (in.bad()) {
      throw std::runtime_error(std::format("{}: read error", source));
    }
    if (!feed.has_value()) {
      throw std::runtime_error(std::format("{}: missing 'feed' entry", source));
    }
    if (builders.size() != N_SHARDS) {
      throw std::runtime_error(
          std::format("{}: expected {} 'builder' entries, found {}", source,
                      N_SHARDS, builders.size()));
    }

    ThreadConfig cfg;
    cfg.feed_core = *feed;
    std::ranges::copy(builders, cfg.builder_cores.begin());
    return cfg;
  }

  void validate(std::string_view source) const {
    long configured = sysconf(_SC_NPROCESSORS_CONF);
    if (configured <= 0) {
      throw std::runtime_error(
          std::format("{}: cannot determine the number of CPUs", source));
    }
    auto n_cpus = static_cast<uint32_t>(configured);

    std::array<uint32_t, N_SHARDS + 1> cores{};
    cores[0] = feed_core;
    std::ranges::copy(builder_cores, cores.begin() + 1);

    for (const uint32_t core : cores) {
      if (core >= n_cpus) {
        throw std::runtime_error(
            std::format("{}: core {} does not exist (this machine has {} "
                        "CPUs)",
                        source, core, n_cpus));
      }
    }

    std::ranges::sort(cores);
    const auto dup = std::ranges::adjacent_find(cores);
    if (dup != cores.end()) {
      throw std::runtime_error(std::format(
          "{}: core {} is assigned to more than one hot thread", source, *dup));
    }
  }

private:
  static std::string_view trim(std::string_view s) noexcept {
    // Also strips '\r', so CRLF files parse.
    std::string_view ws = " \t\r";
    size_t first = s.find_first_not_of(ws);
    if (first == std::string_view::npos) {
      return {};
    }
    size_t last = s.find_last_not_of(ws);
    return s.substr(first, last - first + 1);
  }
};
