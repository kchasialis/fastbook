#include <gtest/gtest.h>
#include <thread>
#include <vector>

#include "spsc_queue.hpp"

class SPSCQueueTest : public ::testing::Test {
protected:
  SPSCQueue<int, 8> queue{};
};

TEST_F(SPSCQueueTest, FreshQueueIsEmpty) {
  EXPECT_TRUE(queue.empty());
  EXPECT_FALSE(queue.full());
}

TEST_F(SPSCQueueTest, PushMakesQueueNonEmpty) {
  EXPECT_TRUE(queue.push(42));
  EXPECT_FALSE(queue.empty());
}

TEST_F(SPSCQueueTest, PopReturnsCorrectValue) {
  queue.push(42);
  auto result = queue.pop();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 42);
}

TEST_F(SPSCQueueTest, PopEmptyReturnsNullopt) {
  auto result = queue.pop();
  EXPECT_FALSE(result.has_value());
}

TEST_F(SPSCQueueTest, PushToFullReturnsFalse) {
  for (int i = 0; i < 7; i++) {
    EXPECT_TRUE(queue.push(i));
  }
  EXPECT_TRUE(queue.full());
  EXPECT_FALSE(queue.push(99));
}

TEST_F(SPSCQueueTest, FIFOOrdering) {
  queue.push(1);
  queue.push(2);
  queue.push(3);

  EXPECT_EQ(*queue.pop(), 1);
  EXPECT_EQ(*queue.pop(), 2);
  EXPECT_EQ(*queue.pop(), 3);
}

TEST_F(SPSCQueueTest, EmptyAfterDrain) {
  queue.push(1);
  queue.push(2);
  queue.pop();
  queue.pop();
  EXPECT_TRUE(queue.empty());
}

TEST_F(SPSCQueueTest, WrapAround) {
  for (int i = 0; i < 4; i++) EXPECT_TRUE(queue.push(i));
  for (int i = 0; i < 4; i++) queue.pop();

  for (int i = 10; i < 14; i++) EXPECT_TRUE(queue.push(i));

  for (int i = 10; i < 14; i++) {
    auto result = queue.pop();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, i);
  }
}

TEST_F(SPSCQueueTest, FillDrainFillAgain) {
  for (int round = 0; round < 3; round++) {
    for (int i = 0; i < 7; i++) EXPECT_TRUE(queue.push(i));
    EXPECT_TRUE(queue.full());
    for (int i = 0; i < 7; i++) {
      auto result = queue.pop();
      ASSERT_TRUE(result.has_value());
      EXPECT_EQ(*result, i);
    }
    EXPECT_TRUE(queue.empty());
  }
}

TEST_F(SPSCQueueTest, EmplaceWorks) {
  EXPECT_TRUE(queue.emplace(42));
  auto result = queue.pop();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 42);
}

TEST(SPSCQueueMoveOnly, UniquePtr) {
  SPSCQueue<std::unique_ptr<int>, 8> q;

  q.push(std::make_unique<int>(42));
  auto result = q.pop();

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(**result, 42);
}

TEST(SPSCQueueThreaded, ProducerConsumer) {
  constexpr int N = 1 << 12;
  SPSCQueue<int, 4096> q;

  std::vector<int> received;
  received.reserve(N);

  std::thread producer([&]() {
    for (int i = 0; i < N; i++) {
      while (!q.push(i));
    }
  });

  std::thread consumer([&]() {
    for (int i = 0; i < N; i++) {
      std::optional<int> val;
      while (!(val = q.pop()));
      received.push_back(*val);
    }
  });

  producer.join();
  consumer.join();

  ASSERT_EQ(received.size(), N);
  for (int i = 0; i < N; i++) {
    EXPECT_EQ(received[i], i);
  }
}