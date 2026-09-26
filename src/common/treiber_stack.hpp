#pragma once

#include <atomic>
#include <cstdint>

template <typename T, unsigned TagBits = 3> struct PackedTag {
  using Repr = uintptr_t;
  using Head = std::atomic<Repr>;
  static_assert(Head::is_always_lock_free);

  static constexpr Repr TAG_MASK = (Repr{1} << TagBits) - 1;
  static constexpr bool tagged = TagBits > 0;

  static T *ptr(Repr r) noexcept {
    return reinterpret_cast<T *>(r & ~TAG_MASK);
  }
  static uint64_t tag(Repr r) noexcept { return r & TAG_MASK; }
  static Repr pack(T *p, uint64_t t) noexcept {
    return reinterpret_cast<Repr>(p) | (t & TAG_MASK);
  }
};

template <typename T> struct WideTag {
  struct alignas(16) Repr {
    T *ptr;
    uint64_t tag;
  };

  using Head = std::atomic<Repr>;
  static_assert(Head::is_always_lock_free);
  static constexpr bool tagged = true;

  static T *ptr(Repr r) noexcept { return r.ptr; }
  static uint64_t tag(Repr r) noexcept { return r.tag; }
  static Repr pack(T *p, uint64_t t) noexcept { return Repr{p, t}; }
};

template <class T, T *T::*Next, template <class> class Tag> class TreiberStack {
private:
  using Policy = Tag<T>;
  using Repr = typename Policy::Repr;

  typename Policy::Head head_;

public:
  TreiberStack() noexcept : head_(Policy::pack(nullptr, 0)) {}

  TreiberStack(const TreiberStack &) = delete;
  TreiberStack &operator=(const TreiberStack &) = delete;

  void push(T *n) noexcept {
    Repr expected = head_.load(std::memory_order_relaxed);
    Repr desired;
    do {
      n->*Next = Policy::ptr(expected);
      desired = Policy::pack(n, Policy::tag(expected) + 1);
    } while (!head_.compare_exchange_weak(expected, desired,
                                          std::memory_order_release,
                                          std::memory_order_relaxed));
  }

  T *pop() noexcept
    requires(Policy::tagged)
  {
    Repr old_head = head_.load(std::memory_order_acquire);
    Repr desired;
    T *old_head_ptr = nullptr;
    do {
      old_head_ptr = Policy::ptr(old_head);
      if (!old_head_ptr) {
        return nullptr;
      }
      desired = Policy::pack(old_head_ptr->*Next, Policy::tag(old_head) + 1);
    } while (!head_.compare_exchange_weak(old_head, desired,
                                          std::memory_order_relaxed,
                                          std::memory_order_acquire));

    return old_head_ptr;
  }

  void push_chain(T *head, T *tail) noexcept {
    Repr old_head = head_.load(std::memory_order_relaxed);
    Repr desired;
    do {
      tail->*Next = Policy::ptr(old_head);
      desired = Policy::pack(head, Policy::tag(old_head) + 1);
    } while (!head_.compare_exchange_weak(old_head, desired,
                                          std::memory_order_release,
                                          std::memory_order_relaxed));
  }

  T *take_all() noexcept {
    Repr old_head = head_.load(std::memory_order_relaxed);
    Repr desired = Policy::pack(nullptr, Policy::tag(old_head) + 1);
    // need acquire here because we need to observe all the changes
    // that were made to the list from all the previous push/push_chain calls
    // which use release
    Repr current_head = head_.exchange(desired, std::memory_order_acquire);

    return Policy::ptr(current_head);
  }
};
