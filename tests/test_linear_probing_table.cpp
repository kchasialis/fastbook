#include <gtest/gtest.h>

#include "linear_probing_table.hpp"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <random>
#include <type_traits>
#include <unordered_map>

struct Node {
  int val;
};

TEST(LinearProbingTable, InsertFindErase) {
  LinearProbingTable<uint64_t, Node *> map{16};
  Node a{1}, b{2};

  EXPECT_EQ(map.find(42), nullptr);
  EXPECT_TRUE(map.insert(1, &a));
  EXPECT_FALSE(map.insert(1, &b));
  EXPECT_EQ(map.find(1), &a);

  EXPECT_TRUE(map.erase(1));
  EXPECT_FALSE(map.erase(1));
  EXPECT_EQ(map.find(1), nullptr);

  EXPECT_TRUE(map.insert(1, &b));
  EXPECT_EQ(map.find(1), &b);
}

template <int Bits> struct BoxHash {
  size_t operator()(uint64_t key) const noexcept {
    return key << (std::numeric_limits<size_t>::digits - Bits);
  }
};

template <EraseMode M, int Bits>
using BoxMap = LinearProbingTable<uint64_t, Node *, BoxHash<Bits>, M>;

template <class Mode> class LinearProbingEraseTest : public ::testing::Test {
protected:
  static constexpr uint64_t N_KEYS = 256;
  std::array<Node, N_KEYS> nodes{};
  BoxMap<Mode::value, 3> map{4};

  void insert_all(std::initializer_list<uint64_t> keys) {
    for (uint64_t key : keys) {
      ASSERT_TRUE(map.insert(key, &nodes[key])) << "key " << key;
    }
  }

  void expect_found(std::initializer_list<uint64_t> keys) {
    for (uint64_t key : keys) {
      EXPECT_EQ(map.find(key), &nodes[key]) << "key " << key;
    }
  }
};

using EraseModes = ::testing::Types<
    std::integral_constant<EraseMode, EraseMode::BackwardShift>,
    std::integral_constant<EraseMode, EraseMode::Tombstone>>;
TYPED_TEST_SUITE(LinearProbingEraseTest, EraseModes);

TYPED_TEST(LinearProbingEraseTest, KeyThatNeverPassedTheEmptyBoxStays) {
  this->insert_all({2, 10, 4, 12, 11});

  EXPECT_TRUE(this->map.erase(10));
  EXPECT_EQ(this->map.find(10), nullptr);
  this->expect_found({2, 4, 12, 11});
}

TYPED_TEST(LinearProbingEraseTest, ShiftAcrossTheEnd) {
  this->insert_all({6, 14, 22, 1});

  EXPECT_TRUE(this->map.erase(14));
  this->expect_found({6, 22, 1});

  EXPECT_TRUE(this->map.erase(6));
  this->expect_found({22, 1});
  EXPECT_EQ(this->map.find(6), nullptr);
  EXPECT_EQ(this->map.find(14), nullptr);
}

TYPED_TEST(LinearProbingEraseTest, EraseFromFullTable) {
  this->insert_all({0, 8, 16, 24, 32, 40, 48, 56});

  EXPECT_TRUE(this->map.erase(16));
  this->expect_found({0, 8, 24, 32, 40, 48, 56});
  EXPECT_EQ(this->map.find(16), nullptr);

  this->insert_all({16});
  this->expect_found({0, 8, 16, 24, 32, 40, 48, 56});
}

TYPED_TEST(LinearProbingEraseTest, MatchesUnorderedMapUnderRandomOps) {
  constexpr size_t MAX_LIVE = 48;
  BoxMap<TypeParam::value, 6> big{32};
  std::unordered_map<uint64_t, Node *> ref;
  std::mt19937_64 rng(42);

  for (int i = 0; i < 200'000; i++) {
    uint64_t key = rng() % this->N_KEYS;
    Node *node = &this->nodes[key];
    switch (rng() % 3) {
    case 0:
      if (ref.size() < MAX_LIVE) {
        bool inserted = ref.emplace(key, node).second;
        ASSERT_EQ(big.insert(key, node), inserted) << "op " << i;
      }
      break;
    case 1:
      ASSERT_EQ(big.erase(key), ref.erase(key) == 1) << "op " << i;
      break;
    default: {
      auto it = ref.find(key);
      ASSERT_EQ(big.find(key), it == ref.end() ? nullptr : it->second)
          << "op " << i;
      break;
    }
    }
  }

  for (const auto &[key, node] : ref) {
    EXPECT_EQ(big.find(key), node) << "key " << key;
  }
}
