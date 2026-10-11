#pragma once

#include <chrono>
#include <ctime>

namespace timing {

struct WallClock {
  using duration = std::chrono::nanoseconds;
  using rep = duration::rep;
  using period = duration::period;
  using time_point = std::chrono::time_point<WallClock>;
  static constexpr bool is_steady = false;
  [[nodiscard]] static time_point now() noexcept {
    struct timespec tp;
    clock_gettime(CLOCK_REALTIME, &tp);

    return time_point{std::chrono::seconds{tp.tv_sec} +
                      std::chrono::nanoseconds{tp.tv_nsec}};
  }
};

struct MonoClock {
  using duration = std::chrono::nanoseconds;
  using rep = duration::rep;
  using period = duration::period;
  using time_point = std::chrono::time_point<MonoClock>;
  static constexpr bool is_steady = true;
  [[nodiscard]] static time_point now() noexcept {
    struct timespec tp;
    clock_gettime(CLOCK_MONOTONIC_RAW, &tp);

    return time_point{std::chrono::seconds{tp.tv_sec} +
                      std::chrono::nanoseconds{tp.tv_nsec}};
  }
};

}; // namespace timing