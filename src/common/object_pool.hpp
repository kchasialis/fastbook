#pragma once

#include <concepts>
#include <memory>

template <typename T>
concept SlabNode = requires(T t) {
  { t.next } -> std::convertible_to<T *>;
  { t.prev } -> std::convertible_to<T *>;
};

template <SlabNode T> class SlabAllocator {
private:
  std::unique_ptr<T[]> pool_;
  T *free_list_;

public:
  SlabAllocator(size_t n_objects)
      : pool_(new T[n_objects]), free_list_(pool_.get()) {
    for (size_t i = 0; i < n_objects - 1; i++) {
      pool_[i].next = &pool_[i + 1];
    }
    pool_[n_objects - 1].next = nullptr;
  }
  ~SlabAllocator() = default;

  SlabAllocator(const SlabAllocator &) = delete;
  SlabAllocator(SlabAllocator &&) = delete;
  SlabAllocator &operator=(const SlabAllocator &) = delete;
  SlabAllocator &operator=(SlabAllocator &&) = delete;

  T *allocate() noexcept {
    if (free_list_ == nullptr) [[unlikely]] {
      return nullptr;
    }

    auto *ret = free_list_;
    free_list_ = free_list_->next;
    return ret;
  }
  void deallocate(T *obj) noexcept {
    if (obj == nullptr) [[unlikely]] {
      return;
    }

    obj->next = free_list_;
    free_list_ = obj;
  }
};