#pragma once

#include "linear_probing_table.hpp"
#include <bit>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace swiss {

struct Control {
  static constexpr uint8_t EMPTY = 0x80;
  static constexpr uint8_t DELETED = 0xFE;

  uint8_t data{EMPTY};

  void set_empty() noexcept { data = EMPTY; }
  void set_deleted() noexcept { data = DELETED; }
  void set_full(uint8_t fprint) noexcept {
    data = static_cast<uint8_t>(fprint & 0x7F);
  }
  bool is_full() const noexcept { return (data & 0x80) == 0; }
};

static_assert(sizeof(Control) == 1 && std::is_standard_layout_v<Control>);

struct ScalarGroup {
  static constexpr size_t WIDTH = 16;
  static constexpr size_t LANE_BITS = 1;
  using Mask = uint16_t;

  static Mask match(const uint8_t *ctrl, uint8_t b) noexcept {
    uint32_t m = 0;
    for (size_t i = 0; i < WIDTH; i++) {
      m |= static_cast<uint32_t>(ctrl[i] == b) << i;
    }
    return static_cast<Mask>(m);
  }

  static Mask match_empty(const uint8_t *ctrl) noexcept {
    return match(ctrl, Control::EMPTY);
  }

  static Mask match_empty_or_deleted(const uint8_t *ctrl) noexcept {
    uint32_t m = 0;
    for (size_t i = 0; i < WIDTH; i++) {
      m |= static_cast<uint32_t>(ctrl[i] >> 7) << i;
    }
    return static_cast<Mask>(m);
  }
};

struct SwarGroup {
  static constexpr size_t WIDTH = 8;
  static constexpr size_t LANE_BITS = 8;
  using Mask = uint64_t;

  static constexpr uint64_t LSBS = 0x0101010101010101;
  static constexpr uint64_t MSBS = 0x8080808080808080;

  static uint64_t load(const uint8_t *ctrl) noexcept {
    uint64_t g;
    std::memcpy(&g, ctrl, sizeof(uint64_t));
    return g;
  }

  static Mask match(const uint8_t *ctrl, uint8_t b) noexcept {
    // broadcast the hash byte into all positions.
    uint64_t test = static_cast<uint64_t>(b) * LSBS;
    uint64_t x = load(ctrl) ^ test; // will contain 00 on the "lane" it matches
    // trick to return one's (0x80) in the place where there were 0x00 before
    return (x - LSBS) & ~x & MSBS;
  }

  static Mask match_empty(const uint8_t *ctrl) noexcept {
    uint64_t g = load(ctrl);
    return g & ~(g << 6) & MSBS;
  }

  static Mask match_empty_or_deleted(const uint8_t *ctrl) noexcept {
    return load(ctrl) & MSBS;
  }
};

#if defined(__SSE2__)
struct Sse2Group {
  static constexpr size_t WIDTH = 16;
  static constexpr size_t LANE_BITS = 1;
  using Mask = uint16_t;

  static __m128i load(const uint8_t *ctrl) noexcept {
    __m128i g;
    std::memcpy(&g, ctrl, sizeof(g));
    return g;
  }

  static Mask match(const uint8_t *ctrl, uint8_t b) noexcept {
    __m128i cmp =
        _mm_cmpeq_epi8(load(ctrl), _mm_set1_epi8(static_cast<char>(b)));
    return static_cast<Mask>(_mm_movemask_epi8(cmp));
  }

  static Mask match_empty(const uint8_t *ctrl) noexcept {
    return match(ctrl, Control::EMPTY);
  }

  static Mask match_empty_or_deleted(const uint8_t *ctrl) noexcept {
    return static_cast<Mask>(_mm_movemask_epi8(load(ctrl)));
  }
};
#endif

#if defined(__AVX2__)
struct Avx2Group {
  static constexpr size_t WIDTH = 32;
  static constexpr size_t LANE_BITS = 1;
  using Mask = uint32_t;

  static __m256i load(const uint8_t *ctrl) noexcept {
    __m256i g;
    std::memcpy(&g, ctrl, sizeof(g));
    return g;
  }

  static Mask match(const uint8_t *ctrl, uint8_t b) noexcept {
    __m256i cmp =
        _mm256_cmpeq_epi8(load(ctrl), _mm256_set1_epi8(static_cast<char>(b)));
    return static_cast<Mask>(_mm256_movemask_epi8(cmp));
  }

  static Mask match_empty(const uint8_t *ctrl) noexcept {
    return match(ctrl, Control::EMPTY);
  }

  static Mask match_empty_or_deleted(const uint8_t *ctrl) noexcept {
    return static_cast<Mask>(_mm256_movemask_epi8(load(ctrl)));
  }
};
#endif

template <class G>
concept Group = requires(const uint8_t *ctrl, uint8_t b) {
  requires std::has_single_bit(G::WIDTH);
  requires static_cast<size_t>(std::numeric_limits<typename G::Mask>::digits) ==
               G::WIDTH *G::LANE_BITS;
  { G::match(ctrl, b) } -> std::same_as<typename G::Mask>;
  { G::match_empty(ctrl) } -> std::same_as<typename G::Mask>;
  { G::match_empty_or_deleted(ctrl) } -> std::same_as<typename G::Mask>;
};

#if defined(__AVX2__)
using DefaultGroup = Avx2Group;
#elif defined(__SSE2__)
using DefaultGroup = Sse2Group;
#else
using DefaultGroup = SwarGroup;
#endif

} // namespace swiss

template <TrivialKey K, typename V, typename Hash = FibHash<K>,
          swiss::Group G = swiss::DefaultGroup>
  requires std::is_trivially_copyable_v<V>
class SwissTable {
private:
  using Control = swiss::Control;
  using Mask = typename G::Mask;
  using ValueType = std::pair<K, V>;

  struct HashResult {
    size_t h1;
    uint8_t h2;
  };

  using FindResult = std::pair<bool, size_t>;

  PrefaultedArray<Control> ctrl_;
  PrefaultedArray<ValueType> data_;
  size_t size_;
  size_t capacity_;
  size_t growth_left_;
  int shift_;
  Hash hash_;

  static size_t growth_for(size_t capacity) noexcept {
    return capacity - capacity / 8;
  }

  static size_t first_lane(Mask m) noexcept {
    return static_cast<size_t>(std::countr_zero(m)) / G::LANE_BITS;
  }

  static size_t last_lanes_clear(Mask m) noexcept {
    return static_cast<size_t>(std::countl_zero(m)) / G::LANE_BITS;
  }

  size_t mask() const noexcept { return capacity_ - 1; }

  const uint8_t *ctrl_at(size_t pos) const noexcept {
    return reinterpret_cast<const uint8_t *>(ctrl_.get()) + pos;
  }

  HashResult hash(const K &key) const noexcept {
    size_t h = hash_(key);
    return {h >> shift_, static_cast<uint8_t>((h >> (shift_ - 7)) & 0x7F)};
  }

  size_t copy_index(size_t pos) const noexcept {
    return pos < G::WIDTH ? capacity_ + pos : pos;
  }

  std::optional<size_t> find_index(const K &key, HashResult h) const noexcept {
    size_t pos = h.h1;
    size_t stride = 0;
    for (;;) {
      for (Mask m = G::match(ctrl_at(pos), h.h2); m != 0; m &= m - 1) {
        size_t idx = (pos + first_lane(m)) & mask();
        if (data_[idx].first == key) {
          return idx;
        }
      }
      if (G::match_empty(ctrl_at(pos)) != 0) {
        return std::nullopt;
      }
      stride += G::WIDTH;
      pos = (pos + stride) & mask();
    }
  }

  size_t prepare_insert(HashResult h) const noexcept {
    size_t pos = h.h1;
    size_t stride = 0;
    for (;;) {
      Mask m = G::match_empty_or_deleted(ctrl_at(pos));
      if (m != 0) {
        return (pos + first_lane(m)) & mask();
      }
      stride += G::WIDTH;
      pos = (pos + stride) & mask();
    }
  }

  [[nodiscard]] FindResult find_or_prepare_insert(const K &key,
                                                  HashResult h) const noexcept {
    std::optional<size_t> idx = find_index(key, h);
    if (idx) {
      return {true, *idx};
    }
    return {false, prepare_insert(h)};
  }

  void place(size_t pos, HashResult h, const ValueType &value) noexcept {
    if (ctrl_[pos].data == Control::EMPTY) {
      growth_left_--;
    }
    ctrl_[pos].set_full(h.h2);
    ctrl_[copy_index(pos)].set_full(h.h2);
    data_[pos] = value;
    size_++;
  }

  void swap(SwissTable &other) noexcept {
    std::swap(ctrl_, other.ctrl_);
    std::swap(data_, other.data_);
    std::swap(size_, other.size_);
    std::swap(capacity_, other.capacity_);
    std::swap(growth_left_, other.growth_left_);
    std::swap(shift_, other.shift_);
  }

public:
  SwissTable(size_t capacity)
      : ctrl_(make_prefaulted_array<Control>(capacity + G::WIDTH)),
        data_(make_prefaulted_array<ValueType>(capacity)), size_(0),
        capacity_(capacity), growth_left_(growth_for(capacity)),
        shift_(std::numeric_limits<size_t>::digits -
               std::countr_zero(capacity)) {
    if (!std::has_single_bit(capacity) || capacity < G::WIDTH) {
      throw std::runtime_error("Hash table should have a power-of-2 capacity, "
                               "at least the group width");
    }
  }

  ~SwissTable() = default;

  SwissTable(const SwissTable &) = delete;
  SwissTable(SwissTable &&) = delete;
  SwissTable &operator=(const SwissTable &) = delete;
  SwissTable &operator=(SwissTable &&) = delete;

  [[nodiscard]] bool insert(const K &key, const V &val) noexcept {
    HashResult hash_res = hash(key);

    FindResult find_res = find_or_prepare_insert(key, hash_res);
    if (find_res.first) {
      // found
      return false;
    }

    size_t insert_pos = find_res.second;
    if (growth_left_ == 0 && ctrl_[insert_pos].data == Control::EMPTY)
        [[unlikely]] {
      bool mostly_deleted = size_ * 32 <= capacity_ * 25;
      if (!rehash(mostly_deleted ? capacity_ : capacity_ * 2)) {
        return false;
      }
      hash_res = hash(key);
      insert_pos = prepare_insert(hash_res);
    }

    place(insert_pos, hash_res, {key, val});
    return true;
  }

  [[nodiscard]] std::optional<V> find(const K &key) const noexcept {
    std::optional<size_t> idx = find_index(key, hash(key));
    if (!idx) {
      return std::nullopt;
    }

    return data_[*idx].second;
  }

  [[nodiscard]] bool erase(const K &key) noexcept {
    std::optional<size_t> idx = find_index(key, hash(key));
    if (!idx) {
      return false;
    }
    size_--;

    size_t pos = *idx;
    Mask empty_after = G::match_empty(ctrl_at(pos));
    Mask empty_before = G::match_empty(ctrl_at((pos - G::WIDTH) & mask()));
    bool never_in_full_window =
        empty_before != 0 && empty_after != 0 &&
        first_lane(empty_after) + last_lanes_clear(empty_before) < G::WIDTH;

    if (never_in_full_window) {
      ctrl_[pos].set_empty();
      ctrl_[copy_index(pos)].set_empty();
      growth_left_++;
    } else {
      ctrl_[pos].set_deleted();
      ctrl_[copy_index(pos)].set_deleted();
    }

    return true;
  }

  bool rehash(size_t new_capacity) noexcept {
    try {
      SwissTable fresh(new_capacity);
      for (size_t i = 0; i < capacity_; i++) {
        if (ctrl_[i].is_full()) {
          HashResult h = fresh.hash(data_[i].first);
          fresh.place(fresh.prepare_insert(h), h, data_[i]);
        }
      }
      swap(fresh);
      return true;
    } catch (const std::exception &e) {
      LOG_ERROR("SwissTable::rehash failed: {}", e.what());
      return false;
    }
  }

  [[nodiscard]] size_t size() const noexcept { return size_; }
  [[nodiscard]] size_t capacity() const noexcept { return capacity_; }
};
