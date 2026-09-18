#pragma once

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

bool pin_and_prioritize(pthread_t thread, uint32_t core) noexcept {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(core, &set);
  if (pthread_setaffinity_np(thread, sizeof(set), &set) != 0) {
    std::cerr << "[DEBUG]: pin_and_prioritize: pthread_setaffinity_np() failed"
              << std::endl;
    return false;
  }

  struct sched_param param;
  param.sched_priority = sched_get_priority_max(SCHED_FIFO);
  if (pthread_setschedparam(thread, SCHED_FIFO, &param) != 0) {
    std::cerr << "[DEBUG]: pin_and_prioritize: pthread_setschedparam() failed"
              << std::endl;
    return false;
  }

  return true;
}