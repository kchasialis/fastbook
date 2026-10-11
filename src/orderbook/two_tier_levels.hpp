#pragma once

#include "types.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <utility>

struct Order;

struct LevelData {
  Order *head;
  Order *tail;
  size_t total_qty;
};

template <Side S, size_t K, size_t ColdCap> class TwoTierLevels {
public:
  static constexpr uint32_t EMPTY = S == Side::BID ? 0 : UINT32_MAX;
  static constexpr uint32_t BEST_POSSIBLE = S == Side::BID ? UINT32_MAX : 0;

  using comp_t =
      std::conditional_t<S == Side::BID, std::less<>, std::greater<>>;

  static constexpr bool better(uint32_t a, uint32_t b) noexcept {
    if constexpr (S == Side::BID)
      return a > b;
    else
      return a < b;
  }

  [[nodiscard]] LevelData *find(uint32_t price) noexcept {
    auto loc = find_idx(price);
    switch (loc.tier) {
    case Tier::NONE:
      return nullptr;
    case Tier::HOT:
      return &(hot_.levels[loc.idx]);
    case Tier::COLD:
      return &(cold_.levels[loc.idx]);
    default:
      std::unreachable();
    }
  }

  [[nodiscard]] LevelData *find_or_insert(uint32_t price) noexcept {
    auto *level_data = find(price);
    if (level_data) {
      return level_data;
    }

    auto p = belongs_to_hot(price);
    if (!p.first) {
      auto res = insert_to_cold(price);
      if (!res.first) {
        return nullptr;
      }
      return &(cold_.levels[res.second]);
    }

    auto &level_price = hot_.prices[p.second];
    level_data = &hot_.levels[p.second];

    if (level_price != EMPTY) {
      auto res = insert_to_cold(level_price, *level_data);
      if (!res.first) {
        return nullptr;
      }
    }

    level_price = price;
    bool is_better = better(price, hot_.best_price);
    hot_.best_price = is_better ? price : hot_.best_price;
    hot_.best_idx = is_better ? p.second : hot_.best_idx;
    *level_data = {};
    return level_data;
  }

  void erase(uint32_t price) noexcept {
    auto loc = find_idx(price);
    switch (loc.tier) {
    case Tier::NONE:
      return;
    case Tier::HOT: {
      auto &prices = hot_.prices;
      auto &levels = hot_.levels;

      if (cold_.size > 0) {
        prices[loc.idx] = cold_.prices[cold_.size - 1];
        levels[loc.idx] = cold_.levels[cold_.size - 1];
        --cold_.size;
      } else {
        prices[loc.idx] = EMPTY;
      }

      if (loc.idx == hot_.best_idx) {
        uint32_t new_best = EMPTY;
        size_t new_best_idx = 0;
        for (size_t i = 0; i < prices.size(); i++) {
          const uint32_t p = prices[i];
          const bool is_better = better(p, new_best);
          new_best = is_better ? p : new_best;
          new_best_idx = is_better ? i : new_best_idx;
        }
        hot_.best_price = new_best;
        hot_.best_idx = new_best_idx;
      }

      return;
    }
    case Tier::COLD: {
      auto &prices = cold_.prices;
      auto &levels = cold_.levels;
      std::shift_left(prices.begin() + loc.idx, prices.begin() + cold_.size, 1);
      std::shift_left(levels.begin() + loc.idx, levels.begin() + cold_.size, 1);
      --cold_.size;
      return;
    }
    default:
      std::unreachable();
    }
  }

  [[nodiscard]] uint32_t best_price() const noexcept { return hot_.best_price; }

  [[nodiscard]] const LevelData *best() const noexcept {
    if (hot_.best_price == EMPTY) [[unlikely]] {
      return nullptr;
    }
    return &(hot_.levels[hot_.best_idx]);
  }

private:
  template <size_t N> struct Level {
    std::array<uint32_t, N> prices;
    std::array<LevelData, N> levels;
    size_t size;
    uint32_t best_price;
    size_t best_idx;

    Level() : size(0), best_price(EMPTY), best_idx(0) {
      std::fill(prices.begin(), prices.end(), EMPTY);
      std::fill(levels.begin(), levels.end(), LevelData{});
    }
  };

  Level<K> hot_;
  Level<ColdCap> cold_;

  enum class Tier : uint8_t { NONE, HOT, COLD };
  struct Loc {
    Tier tier;
    size_t idx;
  };

  Loc find_idx(uint32_t price) const noexcept {
    const auto &prices = hot_.prices;
    for (size_t i = 0; i < prices.size(); i++) {
      if (prices[i] == price) {
        return {Tier::HOT, i};
      }
    }

    const auto &prices_cold = cold_.prices;
    auto end = prices_cold.begin() + cold_.size;
    auto it = std::lower_bound(prices_cold.begin(), end, price, comp_t{});
    if (it == end || *it != price) {
      return {Tier::NONE, 0};
    }

    size_t idx = static_cast<size_t>(it - prices_cold.begin());
    return {Tier::COLD, idx};
  }

  std::pair<bool, size_t> insert_to_cold(uint32_t price,
                                         const LevelData &level_data) noexcept {
    if (cold_.size >= cold_.prices.size()) {
      return {false, 0};
    }

    auto &prices = cold_.prices;
    auto &levels = cold_.levels;
    auto end = prices.begin() + cold_.size;
    auto it = std::upper_bound(prices.begin(), end, price, comp_t{});

    auto diff = static_cast<size_t>(it - prices.begin());
    std::shift_right(prices.begin() + diff, end + 1, 1);
    std::shift_right(levels.begin() + diff, levels.begin() + cold_.size + 1, 1);
    *it = price;
    levels[diff] = level_data;
    ++cold_.size;

    return {true, static_cast<size_t>(diff)};
  }

  std::pair<bool, size_t> insert_to_cold(uint32_t price) noexcept {
    LevelData level{};
    return insert_to_cold(price, level);
  }

  [[nodiscard]] std::pair<bool, size_t>
  belongs_to_hot(uint32_t price) const noexcept {
    bool found = false;
    size_t idx = 0;
    const auto &prices = hot_.prices;
    uint32_t worst = BEST_POSSIBLE;
    for (size_t i = 0; i < prices.size(); i++) {
      uint32_t p = prices[i];
      bool is_worse = better(worst, p);
      worst = is_worse ? p : worst;
      idx = is_worse ? i : idx;
    }

    found = better(price, worst);

    return {found, idx};
  }
};
