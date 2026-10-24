#include <gtest/gtest.h>

#include "spsc_queue.hpp"

#include <memory>
#include <thread>
#include <vector>

TEST(SPSCQueue, PushPopIsFifo) {
  SPSCQueue<int, 8> q;
  auto producer = q.producer();
  auto consumer = q.consumer();

  EXPECT_TRUE(consumer.empty());
  EXPECT_EQ(consumer.pop(), std::nullopt);

  EXPECT_TRUE(producer.push(1));
  EXPECT_TRUE(producer.push(2));
  EXPECT_TRUE(producer.emplace(3));
  EXPECT_FALSE(consumer.empty());

  EXPECT_EQ(consumer.pop(), 1);
  EXPECT_EQ(consumer.pop(), 2);
  EXPECT_EQ(consumer.pop(), 3);
  EXPECT_TRUE(consumer.empty());
}

TEST(SPSCQueue, RejectsPushWhenFullAndWrapsAround) {
  SPSCQueue<int, 8> q;
  auto producer = q.producer();
  auto consumer = q.consumer();

  for (int round = 0; round < 3; round++) {
    for (int i = 0; i < 7; i++) {
      ASSERT_TRUE(producer.push(round * 10 + i));
    }
    EXPECT_TRUE(producer.full());
    EXPECT_FALSE(producer.push(99));

    for (int i = 0; i < 7; i++) {
      EXPECT_EQ(consumer.pop(), round * 10 + i);
    }
    EXPECT_TRUE(consumer.empty());
  }
}

TEST(SPSCQueue, HoldsMoveOnlyValues) {
  SPSCQueue<std::unique_ptr<int>, 8> q;
  auto producer = q.producer();
  auto consumer = q.consumer();

  ASSERT_TRUE(producer.push(std::make_unique<int>(42)));
  auto result = consumer.pop();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(**result, 42);
}

TEST(SPSCQueue, ProducerAndConsumerThreadsSeeEveryItemInOrder) {
  constexpr int N = 1 << 16;
  SPSCQueue<int, 1024> q;
  auto producer = q.producer();
  auto consumer = q.consumer();

  std::vector<int> received;
  received.reserve(N);

  std::thread producer_thread([&] {
    for (int i = 0; i < N; i++) {
      while (!producer.push(i)) {
      }
    }
  });

  std::thread consumer_thread([&] {
    for (int i = 0; i < N; i++) {
      std::optional<int> val;
      while (!(val = consumer.pop())) {
      }
      received.push_back(*val);
    }
  });

  producer_thread.join();
  consumer_thread.join();

  ASSERT_EQ(received.size(), static_cast<size_t>(N));
  for (int i = 0; i < N; i++) {
    ASSERT_EQ(received[static_cast<size_t>(i)], i);
  }
}
