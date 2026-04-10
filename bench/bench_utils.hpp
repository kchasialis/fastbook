#pragma once

#include <algorithm>
#include <chrono>
#include <vector>

#ifdef __APPLE__
#include <pthread.h>
inline void pin_to_core([[maybe_unused]] int core) {
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
}
#else
#include <pthread.h>
#include <sched.h>
inline void pin_to_core(int core) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core, &cpuset);
  pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}
#endif

inline int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::high_resolution_clock::now().time_since_epoch())
      .count();
}

inline std::pair<double, double> percentiles(std::vector<int64_t> &v) {
  std::sort(v.begin(), v.end());
  return {static_cast<double>(v[v.size() * 50 / 100]),
          static_cast<double>(v[v.size() * 99 / 100])};
}
