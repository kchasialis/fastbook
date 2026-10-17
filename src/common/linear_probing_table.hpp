#pragma once

#include "logger.hpp"
#include <bit>
#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>

template <typename V>
concept HashNode = std::is_pointer_v<V>;

template <typename K>
concept TrivialKey = std::is_trivially_copyable_v<K>;

template <typename K> struct FibHash {
  size_t operator()(K key) const noexcept { return key * 0x9e3779b97f4a7c15; }
};

enum class EraseMode : uint8_t { BackwardShift, Tombstone };

template <TrivialKey K, HashNode V, typename Hash = FibHash<K>,
          EraseMode Mode = EraseMode::BackwardShift>
class LinearProbingTable {
private:
  std::unique_ptr<std::pair<K, V>[]> data_;
  size_t size_;
  size_t capacity_;
  int shift_;
  Hash hash_;

  size_t hash(K key) const noexcept { return hash_(key); }
  size_t get_pos(K key) const noexcept { return hash(key) >> shift_; }

  bool is_empty(size_t slot) const noexcept {
    return data_[slot].second == nullptr;
  }
  void set_empty(size_t slot) noexcept { data_[slot].second = nullptr; }

  bool is_deleted(size_t slot) const noexcept {
    return data_[slot].second == reinterpret_cast<V>(1);
  }
  void set_deleted(size_t slot) noexcept {
    data_[slot].second = reinterpret_cast<V>(1);
  }

  size_t find_key(K key) const noexcept {
    size_t pos = get_pos(key);
    for (size_t i = 0; i < capacity_; i++) {
      size_t slot = (pos + i) & (capacity_ - 1);
      if (is_empty(slot)) {
        return capacity_;
      }
      if constexpr (Mode == EraseMode::Tombstone) {
        if (is_deleted(slot)) {
          continue;
        }
      }
      if (data_[slot].first == key) {
        return slot;
      }
    }

    return capacity_;
  }

public:
  LinearProbingTable(size_t capacity)
      : data_(new std::pair<K, V>[capacity << 1]()), size_(0),
        capacity_(capacity << 1), shift_(std::numeric_limits<size_t>::digits -
                                         std::countr_zero(capacity_)) {
    assert((capacity_ & (capacity_ - 1)) == 0);
  }
  ~LinearProbingTable() = default;

  LinearProbingTable(const LinearProbingTable &) = delete;
  LinearProbingTable(LinearProbingTable &&) = delete;
  LinearProbingTable &operator=(const LinearProbingTable &) = delete;
  LinearProbingTable &operator=(LinearProbingTable &&) = delete;

  bool insert(K key, V value) noexcept {
    size_t pos = get_pos(key);
    size_t first_deleted = capacity_;
    for (size_t i = 0; i < capacity_; i++) {
      size_t slot = (pos + i) & (capacity_ - 1);
      if constexpr (Mode == EraseMode::Tombstone) {
        if (is_deleted(slot)) {
          if (first_deleted == capacity_)
            first_deleted = slot;
          continue;
        }
      }
      if (is_empty(slot)) {
        size_t insert_slot =
            (first_deleted != capacity_) ? first_deleted : slot;
        data_[insert_slot] = {key, value};
        size_++;
        return true;
      }
      if (data_[slot].first == key) [[unlikely]] {
        return false;
      }
    }
    if (first_deleted != capacity_) {
      data_[first_deleted] = {key, value};
      size_++;
      return true;
    }
    return false;
  }

  V find(K key) noexcept {
    size_t pos = find_key(key);
    if (pos == capacity_) {
      return nullptr;
    }

    return data_[pos].second;
  }

  bool erase(K key) noexcept {
    size_t pos = find_key(key);
    if (pos == capacity_) {
      return false;
    }
    --size_;

    if constexpr (Mode == EraseMode::Tombstone) {
      set_deleted(pos);
      return true;
    }

    set_empty(pos);
    size_t mask = capacity_ - 1;
    size_t empty = pos;
    size_t current = (pos + 1) & mask;
    while (!is_empty(current)) {
      size_t wanted = get_pos(data_[current].first);
      // Interestingly, this works for wraparound as well.
      // for example, if wanted = 6 in a table of 8 slots 0..7
      // then the steps needed to go from index 6 to index 1 with wraparound
      // are: 6 -> 7 -> 0 -> 1 = 3 1 - 6 = -5 mod 8 (or & 7) = 3. we
      // basically need to add the capacity back if its negative. we want to
      // avoid branches, the mask handles it by masking unsigned overflow is
      // defined. it wraps around.
      size_t steps_wc = (current - wanted) & mask;
      size_t steps_we = (empty - wanted) & mask;

      if (steps_wc > steps_we) {
        // the slot wanted to go to wanted initially, move it back to compact
        // the array. We do that to avoid tombstones,
        // in order to maintain the invariant that empty means not-present.
        data_[empty] = data_[current];
        set_empty(current);
        empty = current;
      }

      current = (current + 1) & mask;
    }

    return true;
  }

  bool rehash(size_t new_capacity) noexcept {
    try {
      LinearProbingTable new_table(new_capacity);

      for (size_t i = 0; i < capacity_; i++) {
        if (is_empty(i) || is_deleted(i)) {
          continue;
        }
        [[maybe_unused]] bool ok =
            new_table.insert(this->data_[i].first, this->data_[i].second);
        assert(ok);
      }

      std::swap(this->data_, new_table.data_);
      std::swap(this->capacity_, new_table.capacity_);
      std::swap(this->shift_, new_table.shift_);

      return true;
    } catch (const std::bad_alloc &ba) {
      LOG_ERROR("LinearProbingTable::rehash exception: {}", ba.what());
      return false;
    }
  }
};
