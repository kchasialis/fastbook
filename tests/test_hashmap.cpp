#include <gtest/gtest.h>

#include "hash_map.hpp"

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

using Map = HashMap<uint64_t, Node *>;

class HashMapTest : public ::testing::Test {
protected:
  static constexpr size_t CAPACITY = 16;
  Map map{CAPACITY};
  Node a{1}, b{2}, c{3};
};

TEST_F(HashMapTest, FindMissOnEmpty) { EXPECT_EQ(map.find(42), nullptr); }

TEST_F(HashMapTest, InsertAndFind) {
  EXPECT_TRUE(map.insert(1, &a));
  EXPECT_EQ(map.find(1), &a);
}

TEST_F(HashMapTest, InsertDuplicateReturnsFalse) {
  map.insert(1, &a);
  EXPECT_FALSE(map.insert(1, &b));
  EXPECT_EQ(map.find(1), &a);
}

TEST_F(HashMapTest, EraseRemovesKey) {
  map.insert(1, &a);
  EXPECT_TRUE(map.erase(1));
  EXPECT_EQ(map.find(1), nullptr);
}

TEST_F(HashMapTest, EraseMissingReturnsFalse) { EXPECT_FALSE(map.erase(99)); }

TEST_F(HashMapTest, InsertAfterErase) {
  map.insert(1, &a);
  map.erase(1);
  EXPECT_TRUE(map.insert(1, &b));
  EXPECT_EQ(map.find(1), &b);
}

TEST_F(HashMapTest, MultipleKeys) {
  map.insert(1, &a);
  map.insert(2, &b);
  map.insert(3, &c);
  EXPECT_EQ(map.find(1), &a);
  EXPECT_EQ(map.find(2), &b);
  EXPECT_EQ(map.find(3), &c);
}

TEST_F(HashMapTest, EraseOneDoesNotAffectOthers) {
  map.insert(1, &a);
  map.insert(2, &b);
  map.erase(1);
  EXPECT_EQ(map.find(1), nullptr);
  EXPECT_EQ(map.find(2), &b);
}

TEST_F(HashMapTest, EraseDoesNotBlockProbe) {
  Map small{4};
  Node x{10}, y{20};

  small.insert(10, &x);
  small.insert(23, &y);
  small.erase(10);
  EXPECT_EQ(small.find(10), nullptr);
  EXPECT_EQ(small.find(23), &y);
}

template <int Bits> struct BoxHash {
  size_t operator()(uint64_t key) const noexcept {
    return key << (std::numeric_limits<size_t>::digits - Bits);
  }
};

template <EraseMode M, int Bits>
using BoxMap = HashMap<uint64_t, Node *, BoxHash<Bits>, M>;

template <class Mode> class HashMapEraseTest : public ::testing::Test {
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
TYPED_TEST_SUITE(HashMapEraseTest, EraseModes);

TYPED_TEST(HashMapEraseTest, PushedKeyMovesBack) {
  this->insert_all({2, 10, 3, 5});

  EXPECT_TRUE(this->map.erase(10));
  EXPECT_EQ(this->map.find(10), nullptr);
  this->expect_found({2, 3, 5});
}

TYPED_TEST(HashMapEraseTest, KeyThatNeverPassedTheEmptyBoxStays) {
  this->insert_all({2, 10, 4, 12, 11});

  EXPECT_TRUE(this->map.erase(10));
  EXPECT_EQ(this->map.find(10), nullptr);
  this->expect_found({2, 4, 12, 11});
}

TYPED_TEST(HashMapEraseTest, ShiftAcrossTheEnd) {
  this->insert_all({6, 14, 22, 1});

  EXPECT_TRUE(this->map.erase(14));
  this->expect_found({6, 22, 1});

  EXPECT_TRUE(this->map.erase(6));
  this->expect_found({22, 1});
  EXPECT_EQ(this->map.find(6), nullptr);
  EXPECT_EQ(this->map.find(14), nullptr);
}

TYPED_TEST(HashMapEraseTest, EraseInLastBox) {
  this->insert_all({7, 15});

  EXPECT_TRUE(this->map.erase(7));
  EXPECT_EQ(this->map.find(7), nullptr);
  this->expect_found({15});
}

TYPED_TEST(HashMapEraseTest, EraseFromLongRun) {
  this->insert_all({0, 8, 16, 24, 32, 40});

  EXPECT_TRUE(this->map.erase(0));
  this->expect_found({8, 16, 24, 32, 40});

  EXPECT_TRUE(this->map.erase(24));
  this->expect_found({8, 16, 32, 40});
  EXPECT_EQ(this->map.find(0), nullptr);
  EXPECT_EQ(this->map.find(24), nullptr);
}

TYPED_TEST(HashMapEraseTest, EraseFromFullTable) {
  this->insert_all({0, 8, 16, 24, 32, 40, 48, 56});

  EXPECT_TRUE(this->map.erase(16));
  this->expect_found({0, 8, 24, 32, 40, 48, 56});
  EXPECT_EQ(this->map.find(16), nullptr);

  this->insert_all({16});
  this->expect_found({0, 8, 16, 24, 32, 40, 48, 56});
}

TYPED_TEST(HashMapEraseTest, ReinsertAfterErase) {
  this->insert_all({3, 11, 19});

  EXPECT_TRUE(this->map.erase(11));
  this->insert_all({11});
  this->expect_found({3, 11, 19});
  EXPECT_FALSE(this->map.insert(19, &this->nodes[19]));
}

TYPED_TEST(HashMapEraseTest, MatchesUnorderedMapUnderRandomOps) {
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
