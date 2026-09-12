#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

struct NoReset {
  template <typename T> void operator()(T &) const noexcept {}
};

// ObjectPool implemented as a stack of objects.
template <typename T, typename Reset = NoReset> class ObjectPool {
private:
  std::unique_ptr<T[]> storage_;
  std::unique_ptr<T *[]> free_;
  size_t capacity_;
  size_t n_free_;
  [[no_unique_address]] Reset reset_;

public:
  explicit ObjectPool(size_t n_objects, Reset reset = Reset{})
      : storage_(n_objects > 0 ? new T[n_objects] : nullptr),
        free_(n_objects > 0 ? new T *[n_objects] : nullptr),
        capacity_(n_objects), n_free_(n_objects), reset_(std::move(reset)) {
    // Hand out low indices first so early allocations stay contiguous.
    for (size_t i = 0; i < n_objects; i++) {
      free_[i] = &storage_[n_objects - 1 - i];
    }
  }

  ~ObjectPool() = default;

  ObjectPool(const ObjectPool &) = delete;
  ObjectPool(ObjectPool &&) = delete;
  ObjectPool &operator=(const ObjectPool &) = delete;
  ObjectPool &operator=(ObjectPool &&) = delete;

  // Returns nullptr when exhausted.
  T *get() noexcept {
    if (n_free_ == 0) [[unlikely]] {
      return nullptr;
    }

    return free_[--n_free_];
  }

  void restore(T *obj) noexcept {
    if (obj == nullptr) [[unlikely]] {
      return;
    }

    reset_(*obj);
    free_[n_free_++] = obj;
  }

  size_t capacity() const noexcept { return capacity_; }
  size_t available() const noexcept { return n_free_; }

  bool add_chunk(std::span<std::byte> mem, size_t n) noexcept {
    static_assert(std::is_trivially_destructible_v<T>);

    assert(mem.size() >= n * sizeof(T));
    assert((reinterpret_cast<uintptr_t>(mem.data()) % alignof(T)) == 0);

    try {
      auto new_free = std::make_unique<T *[]>(capacity_ + n);
      for (size_t i = 0; i < n_free_; i++) {
        new_free[i] = free_[i];
      }
      for (size_t i = 0; i < n; i++) {
        new_free[n_free_ + i] = new (mem.data() + (i * sizeof(T))) T();
      }
      capacity_ += n;

      free_ = std::move(new_free);
      n_free_ += n;

      return true;
    } catch (const std::bad_alloc &ba) {
      std::cerr << "[DEBUG]: ObjectPool::add_chunk exception: " << ba.what()
                << std::endl;
      return false;
    }
  }
};
