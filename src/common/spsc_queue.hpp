#pragma once

#include <atomic>
#include <cstdint>
#include <new>
#include <optional>

template <typename T, size_t N>
  requires std::is_nothrow_move_constructible_v<T>
class SPSCQueue {
private:
  static_assert(N > 0, "Queue size must be greater than 0");
  static_assert((N & (N - 1)) == 0, "Queue size must be a power of 2");

  alignas(T) std::byte buffer_[N * sizeof(T)];
  alignas(
      std::hardware_destructive_interference_size) std::atomic<size_t> head_;
  alignas(
      std::hardware_destructive_interference_size) std::atomic<size_t> tail_;

  bool empty_at(size_t tail_val) const noexcept {
    return head_.load(std::memory_order_acquire) == tail_val;
  }

  bool empty() const noexcept {
    return empty_at(tail_.load(std::memory_order_relaxed));
  }

  bool full_at(size_t head_val) const noexcept {
    return ((head_val + 1) & (N - 1)) == tail_.load(std::memory_order_acquire);
  }

  bool full() const noexcept {
    return full_at(head_.load(std::memory_order_relaxed));
  }

  template <typename... Args> bool push(Args &&...args) {
    size_t head_val = head_.load(std::memory_order_relaxed);
    if (full_at(head_val)) {
      return false;
    }

    new (&buffer_[head_val * sizeof(T)]) T(std::forward<Args>(args)...);
    head_.store((head_val + 1) & (N - 1), std::memory_order_release);

    return true;
  }

  std::optional<T> pop() noexcept {
    size_t tail_val = tail_.load(std::memory_order_relaxed);
    if (empty_at(tail_val)) {
      return std::nullopt;
    }

    T *obj = reinterpret_cast<T *>(&buffer_[tail_val * sizeof(T)]);
    T value = std::move(*obj);
    obj->~T();
    tail_.store((tail_val + 1) & (N - 1), std::memory_order_release);
    return value;
  }

public:
  SPSCQueue() noexcept : head_(0), tail_(0) {}
  SPSCQueue(const SPSCQueue &) = delete;
  SPSCQueue &operator=(const SPSCQueue &) = delete;
  SPSCQueue(SPSCQueue &&) = delete;
  SPSCQueue &operator=(SPSCQueue &&) = delete;

  ~SPSCQueue() noexcept {
    size_t tail_val = tail_.load(std::memory_order_acquire);
    size_t head_val = head_.load(std::memory_order_acquire);
    for (size_t i = tail_val; i != head_val; i = (i + 1) & (N - 1)) {
      reinterpret_cast<T *>(&buffer_[i * sizeof(T)])->~T();
    }
  }

  class SPSCProducer {
  private:
    SPSCQueue<T, N> &queue_;

  public:
    SPSCProducer(SPSCQueue<T, N> &queue) : queue_(queue) {}
    bool full() const noexcept { return queue_.full(); }
    bool push(const T &val) noexcept { return queue_.push(val); }
    bool push(T &&val) noexcept { return queue_.push(std::move(val)); }

    template <typename... Args> bool emplace(Args &&...args) noexcept {
      return queue_.push(std::forward<Args>(args)...);
    }
  };

  class SPSCConsumer {
  private:
    SPSCQueue<T, N> &queue_;

  public:
    SPSCConsumer(SPSCQueue<T, N> &queue) : queue_(queue) {}
    bool empty() const noexcept { return queue_.empty(); }
    std::optional<T> pop() noexcept { return queue_.pop(); }
  };

  SPSCProducer producer() noexcept { return SPSCProducer{*this}; };

  SPSCConsumer consumer() noexcept { return SPSCConsumer{*this}; };
};