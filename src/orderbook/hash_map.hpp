#pragma once

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

template <TrivialKey K, HashNode V, typename Hash = FibHash<K>> class HashMap {
private:
  std::unique_ptr<std::pair<K, V>[]> data_;
  size_t size_;
  size_t capacity_;
  Hash hash_;

  size_t hash(K key) const noexcept { return hash_(key); }
  size_t get_pos(K key) const noexcept { return hash(key) & (capacity_ - 1); }

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
      if (is_deleted(slot)) {
        continue;
      }
      if (data_[slot].first == key) {
        return slot;
      }
    }

    return capacity_;
  }

public:
  HashMap(size_t capacity)
      : data_(new std::pair<K, V>[capacity << 1]()), size_(0),
        capacity_(capacity << 1) {
    assert((capacity_ & (capacity_ - 1)) == 0);
  }
  ~HashMap() = default;

  HashMap(const HashMap &) = delete;
  HashMap(HashMap &&) = delete;
  HashMap &operator=(const HashMap &) = delete;
  HashMap &operator=(HashMap &&) = delete;

  bool insert(K key, V value) noexcept {
    size_t pos = get_pos(key);
    size_t first_deleted = capacity_;
    for (size_t i = 0; i < capacity_; i++) {
      size_t slot = (pos + i) & (capacity_ - 1);
      if (is_deleted(slot)) {
        if (first_deleted == capacity_)
          first_deleted = slot;
        continue;
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

    set_deleted(pos);
    --size_;

    return true;
  }
};
