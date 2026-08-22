#pragma once

#include <atomic>
#include <cstdint>
#include <new>
#include <optional>

constexpr std::size_t CACHE_LINE_SIZE =
    std::hardware_destructive_interference_size;

template <typename T, size_t N>
requires std::is_nothrow_move_constructible_v<T> class SPSCQueue {
private:
  static_assert(N > 0, "Queue size must be greater than 0");
  static_assert((N & (N - 1)) == 0, "Queue size must be a power of 2");

  alignas(T) std::byte buffer_[N * sizeof(T)];
  alignas(CACHE_LINE_SIZE) std::atomic<size_t> head_;
  alignas(CACHE_LINE_SIZE) std::atomic<size_t> tail_;

  bool empty() const noexcept {
    return head_.load(std::memory_order_acquire) ==
           tail_.load(std::memory_order_relaxed);
  }

  bool full() const noexcept {
    return ((head_.load(std::memory_order_relaxed) + 1) & (N - 1)) ==
           tail_.load(std::memory_order_acquire);
  }

  template <typename... Args> bool push(Args &&...args) {
    if (full()) {
      return false;
    }

    size_t head_val = head_.load(std::memory_order_relaxed);
    new (&buffer_[head_val * sizeof(T)]) T(std::forward<Args>(args)...);
    head_.store((head_val + 1) & (N - 1), std::memory_order_release);

    return true;
  }

  std::optional<T> pop() noexcept {
    if (empty()) {
      return std::nullopt;
    }
    size_t tail_val = tail_.load(std::memory_order_relaxed);

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