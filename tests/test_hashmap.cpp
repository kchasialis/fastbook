#include <gtest/gtest.h>

#include "hash_map.hpp"

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

TEST_F(HashMapTest, TombstoneDoesNotBlockProbe) {
  Map small{4};
  Node x{10}, y{20};

  small.insert(10, &x);
  small.insert(10 + 8, &y);
  small.erase(10);
  EXPECT_EQ(small.find(10), nullptr);
  EXPECT_EQ(small.find(10 + 8), &y);
}