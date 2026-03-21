#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <new>

constexpr std::size_t CACHE_LINE_SIZE = std::hardware_destructive_interference_size;

template<typename T, size_t N>
requires std::is_nothrow_move_constructible_v<T>
class SPSCQueue {
private:
  static_assert(N > 0, "Queue size must be greater than 0");
  static_assert((N & (N - 1)) == 0, "Queue size must be a power of 2");

  alignas(T) std::byte buffer[N * sizeof(T)];
  alignas(CACHE_LINE_SIZE) std::atomic<size_t> head;
  alignas(CACHE_LINE_SIZE) std::atomic<size_t> tail;

  template<typename... Args>
  bool push_impl(Args&&... args) {
    if (full()) {
      return false;
    }

    size_t head_val = head.load(std::memory_order_relaxed);
    new (&buffer[head_val * sizeof(T)]) T(std::forward<Args>(args)...);
    head.store((head_val + 1) & (N - 1), std::memory_order_release);

    return true;
  }

public:
  SPSCQueue() noexcept : head(0), tail(0) {}
  SPSCQueue(const SPSCQueue&)            = delete;
  SPSCQueue& operator=(const SPSCQueue&) = delete;
  SPSCQueue(SPSCQueue&&)            = delete;
  SPSCQueue& operator=(SPSCQueue&&) = delete;

  ~SPSCQueue() noexcept {
    size_t tail_val = tail.load(std::memory_order_acquire);
    size_t head_val = head.load(std::memory_order_acquire);
    for (size_t i = tail_val; i != head_val; i = (i + 1) & (N - 1)) {
      reinterpret_cast<T*>(&buffer[i * sizeof(T)])->~T();
    }
  }

  bool empty() const noexcept {
    return head.load(std::memory_order_acquire) == tail.load(std::memory_order_relaxed);
  }

  bool full() const noexcept {
    return ((head.load(std::memory_order_relaxed) + 1) & (N - 1)) == tail.load(std::memory_order_acquire);
  }

  bool push(const T& val) noexcept { return push_impl(val); }
  bool push(T&& val) noexcept { return push_impl(std::move(val)); }

  template<typename... Args>
  bool emplace(Args&&... args) noexcept {
    return push_impl(std::forward<Args>(args)...);
  }

  std::optional<T> pop() noexcept {
    if (empty()) {
      return std::nullopt;
    }
    size_t tail_val = tail.load(std::memory_order_relaxed);

    T* obj = reinterpret_cast<T*>(&buffer[tail_val * sizeof(T)]);
    T value = std::move(*obj);
    obj->~T();
    tail.store((tail_val + 1) & (N - 1), std::memory_order_release);
    return value;
  }
};