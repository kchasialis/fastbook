#include <gtest/gtest.h>

#include "swiss_table.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <random>
#include <unordered_map>

namespace {

struct Node {
  int val;
};

template <int Bits> struct TopBitsHash {
  size_t operator()(uint64_t key) const noexcept {
    return key << (std::numeric_limits<size_t>::digits - Bits);
  }
};

template <class G>
using Table = SwissTable<uint64_t, Node *, FibHash<uint64_t>, G>;

template <class G>
using CollidingTable = SwissTable<uint64_t, Node *, TopBitsHash<6>, G>;

} // namespace

template <class G> class SwissTableTest : public ::testing::Test {
protected:
  static constexpr uint64_t N_KEYS = 512;
  std::array<Node, N_KEYS> nodes{};
};

#if defined(__AVX2__)
using Groups = ::testing::Types<swiss::ScalarGroup, swiss::SwarGroup,
                                swiss::Sse2Group, swiss::Avx2Group>;
#elif defined(__SSE2__)
using Groups =
    ::testing::Types<swiss::ScalarGroup, swiss::SwarGroup, swiss::Sse2Group>;
#else
using Groups = ::testing::Types<swiss::ScalarGroup, swiss::SwarGroup>;
#endif
TYPED_TEST_SUITE(SwissTableTest, Groups);

TYPED_TEST(SwissTableTest, CollidingKeysSpillAcrossGroupsAndWrap) {
  CollidingTable<TypeParam> t{64};
  for (uint64_t j = 0; j < 40; j++) {
    uint64_t k = 60 + 64 * j;
    ASSERT_TRUE(t.insert(k, &this->nodes[k % this->N_KEYS])) << "key " << k;
  }
  ASSERT_EQ(t.capacity(), 64u);

  for (uint64_t j = 0; j < 40; j += 2) {
    EXPECT_TRUE(t.erase(60 + 64 * j));
  }
  for (uint64_t j = 0; j < 40; j++) {
    uint64_t k = 60 + 64 * j;
    if (j % 2 == 0) {
      EXPECT_EQ(t.find(k), std::nullopt) << "key " << k;
    } else {
      EXPECT_EQ(t.find(k), &this->nodes[k % this->N_KEYS]) << "key " << k;
    }
  }

  for (uint64_t j = 0; j < 40; j += 2) {
    uint64_t k = 60 + 64 * j;
    EXPECT_TRUE(t.insert(k, &this->nodes[k % this->N_KEYS])) << "key " << k;
  }
  for (uint64_t j = 0; j < 40; j++) {
    uint64_t k = 60 + 64 * j;
    EXPECT_EQ(t.find(k), &this->nodes[k % this->N_KEYS]) << "key " << k;
  }
}

template <class T, class Nodes>
void check_against_unordered_map(T &t, Nodes &nodes, size_t max_live,
                                 uint64_t seed) {
  std::unordered_map<uint64_t, Node *> ref;
  std::mt19937_64 rng(seed);

  for (int i = 0; i < 300'000; i++) {
    uint64_t key = rng() % nodes.size();
    Node *node = &nodes[key];
    switch (rng() % 3) {
    case 0:
      if (ref.size() < max_live) {
        bool inserted = ref.emplace(key, node).second;
        ASSERT_EQ(t.insert(key, node), inserted) << "op " << i;
      }
      break;
    case 1:
      ASSERT_EQ(t.erase(key), ref.erase(key) == 1) << "op " << i;
      break;
    default: {
      auto it = ref.find(key);
      std::optional<Node *> expected =
          it == ref.end() ? std::nullopt : std::optional<Node *>(it->second);
      ASSERT_EQ(t.find(key), expected) << "op " << i;
      break;
    }
    }
    ASSERT_EQ(t.size(), ref.size()) << "op " << i;
  }

  for (const auto &[key, node] : ref) {
    EXPECT_EQ(t.find(key), node) << "key " << key;
  }
}

TYPED_TEST(SwissTableTest, MatchesUnorderedMap) {
  Table<TypeParam> spread{64};
  ASSERT_NO_FATAL_FAILURE(
      check_against_unordered_map(spread, this->nodes, 200, 42));

  CollidingTable<TypeParam> colliding{64};
  ASSERT_NO_FATAL_FAILURE(
      check_against_unordered_map(colliding, this->nodes, 48, 7));
}
