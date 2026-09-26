#pragma once

#include "spsc_queue.hpp"
#include "types.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>

inline constexpr uint32_t N_SHARDS = 4;
static_assert(std::has_single_bit(N_SHARDS),
              "shard mask requires a power of 2");

inline constexpr size_t QUEUE_SIZE = 4096;
static_assert(std::has_single_bit(QUEUE_SIZE),
              "SPSCQueue requires a power of 2");

using Queue = SPSCQueue<MboEvent, QUEUE_SIZE>;
using Producer = Queue::SPSCProducer;
using Consumer = Queue::SPSCConsumer;
