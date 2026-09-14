#pragma once

#include "spsc_queue.hpp"
#include "types.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>

// Shared vocabulary between the feed side and the book side: the queue that
// carries parsed events across the thread boundary, and how many of them there
// are.
//
// It lives in common/ rather than with either layer because both need it and
// neither owns it - putting it in feed/ made bookbuilder depend on feed for
// three type aliases, and would have made the layer above them circular.

// Number of book-building shards, and therefore of queues and of consumer
// threads. An instrument is routed to `instrument & (N_SHARDS - 1)`, so every
// event for a symbol lands on the same shard and each BookBuilder owns its
// books and maps outright, with no synchronisation.
inline constexpr uint32_t N_SHARDS = 4;
static_assert(std::has_single_bit(N_SHARDS), "shard mask requires a power of 2");

// Ring depth in events, unrelated to the shard count.
inline constexpr size_t QUEUE_SIZE = 4096;
static_assert(std::has_single_bit(QUEUE_SIZE), "SPSCQueue requires a power of 2");

// Single-producer/single-consumer is only valid because exactly one VenueFeed
// thread writes and exactly one BookBuilder thread reads each queue. A second
// venue routing into the same queues would make them MPSC.
using Queue = SPSCQueue<MboEvent, QUEUE_SIZE>;
using Producer = Queue::SPSCProducer;
using Consumer = Queue::SPSCConsumer;
