#include <gtest/gtest.h>
#include <thread>
#include <vector>

#include "spsc_queue.hpp"

class SPSCQueueTest : public ::testing::Test {
protected:
  SPSCQueue<int, 8> queue{};
  SPSCQueue<int, 8>::SPSCProducer producer = queue.producer();
  SPSCQueue<int, 8>::SPSCConsumer consumer = queue.consumer();
};

TEST_F(SPSCQueueTest, FreshQueueIsEmpty) {
  EXPECT_TRUE(consumer.empty());
  EXPECT_FALSE(producer.full());
}

TEST_F(SPSCQueueTest, PushMakesQueueNonEmpty) {
  EXPECT_TRUE(producer.push(42));
  EXPECT_FALSE(consumer.empty());
}

TEST_F(SPSCQueueTest, PopReturnsCorrectValue) {
  producer.push(42);
  auto result = consumer.pop();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 42);
}

TEST_F(SPSCQueueTest, PopEmptyReturnsNullopt) {
  auto result = consumer.pop();
  EXPECT_FALSE(result.has_value());
}

TEST_F(SPSCQueueTest, PushToFullReturnsFalse) {
  for (int i = 0; i < 7; i++) {
    EXPECT_TRUE(producer.push(i));
  }
  EXPECT_TRUE(producer.full());
  EXPECT_FALSE(producer.push(99));
}

TEST_F(SPSCQueueTest, FIFOOrdering) {
  producer.push(1);
  producer.push(2);
  producer.push(3);

  EXPECT_EQ(*consumer.pop(), 1);
  EXPECT_EQ(*consumer.pop(), 2);
  EXPECT_EQ(*consumer.pop(), 3);
}

TEST_F(SPSCQueueTest, EmptyAfterDrain) {
  producer.push(1);
  producer.push(2);
  consumer.pop();
  consumer.pop();
  EXPECT_TRUE(consumer.empty());
}

TEST_F(SPSCQueueTest, WrapAround) {
  for (int i = 0; i < 4; i++) EXPECT_TRUE(producer.push(i));
  for (int i = 0; i < 4; i++) consumer.pop();

  for (int i = 10; i < 14; i++) EXPECT_TRUE(producer.push(i));

  for (int i = 10; i < 14; i++) {
    auto result = consumer.pop();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, i);
  }
}

TEST_F(SPSCQueueTest, FillDrainFillAgain) {
  for (int round = 0; round < 3; round++) {
    for (int i = 0; i < 7; i++) EXPECT_TRUE(producer.push(i));
    EXPECT_TRUE(producer.full());
    for (int i = 0; i < 7; i++) {
      auto result = consumer.pop();
      ASSERT_TRUE(result.has_value());
      EXPECT_EQ(*result, i);
    }
    EXPECT_TRUE(consumer.empty());
  }
}

TEST_F(SPSCQueueTest, EmplaceWorks) {
  EXPECT_TRUE(producer.emplace(42));
  auto result = consumer.pop();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 42);
}

TEST(SPSCQueueMoveOnly, UniquePtr) {
  SPSCQueue<std::unique_ptr<int>, 8> q;
  auto producer = q.producer();
  auto consumer = q.consumer();

  producer.push(std::make_unique<int>(42));
  auto result = consumer.pop();

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(**result, 42);
}

TEST(SPSCQueueThreaded, ProducerConsumer) {
  constexpr int N = 1 << 12;
  SPSCQueue<int, 4096> q;
  auto producer = q.producer();
  auto consumer = q.consumer();

  std::vector<int> received;
  received.reserve(N);

  std::thread producer_thread([&]() {
    for (int i = 0; i < N; i++) {
      while (!producer.push(i));
    }
  });

  std::thread consumer_thread([&]() {
    for (int i = 0; i < N; i++) {
      std::optional<int> val;
      while (!(val = consumer.pop()));
      received.push_back(*val);
    }
  });

  producer_thread.join();
  consumer_thread.join();

  ASSERT_EQ(received.size(), N);
  for (size_t i = 0; i < N; i++) {
    EXPECT_EQ(received[i], static_cast<int>(i));
  }
}