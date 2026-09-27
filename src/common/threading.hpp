#pragma once

#include "logger.hpp"
#include <atomic>
#include <cstdint>
#include <cstring>
#include <latch>
#include <pthread.h>
#include <sched.h>

struct StartGate {
  std::latch ready;
  std::latch go;
  std::atomic<uint32_t> failed;

  explicit StartGate(uint32_t n_threads) : ready(n_threads), go(1), failed(0) {}
};

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
