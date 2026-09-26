#pragma once

#include "logger.hpp"
#include <cerrno>
#include <format>
#include <pthread.h>
#include <sched.h>
#include <source_location>
#include <string_view>
#include <system_error>

inline int check(int rc, std::string_view what,
                 std::source_location loc = std::source_location::current()) {
  if (rc < 0) {
    throw std::system_error(
        errno, std::system_category(),
        std::format("{} at {}:{}", what, loc.file_name(), loc.line()));
  }

  return rc;
}

inline int
check_neg(int rc, std::string_view what,
          std::source_location loc = std::source_location::current()) {
  if (rc < 0) {
    throw std::system_error(
        -rc, std::system_category(),
        std::format("{} at {}:{}", what, loc.file_name(), loc.line()));
  }

  return rc;
}

inline bool pin_and_prioritize(pthread_t thread, uint32_t core) noexcept {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(core, &set);
  if (int rc = pthread_setaffinity_np(thread, sizeof(set), &set); rc != 0) {
    LOG_ERROR("pin to core {} failed: {}", core, std::strerror(rc));
    return false;
  }

  struct sched_param param{};
  param.sched_priority = sched_get_priority_max(SCHED_FIFO);
  if (int rc = pthread_setschedparam(thread, SCHED_FIFO, &param); rc != 0) {
    LOG_ERROR("pin to core {} failed: {}", core, std::strerror(rc));
    return false;
  }

  return true;
}
