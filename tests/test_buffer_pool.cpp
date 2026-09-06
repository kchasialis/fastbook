#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <set>
#include <thread>
#include <vector>

#include "buffer_pool.hpp"

namespace {

// Size of the VMA containing p, straight from /proc/self/maps. 0 if unmapped.
size_t vma_size_at(const void *p) {
  FILE *f = fopen("/proc/self/maps", "r");
  if (!f) {
    return 0;
  }
  char line[512];
  size_t found = 0;
  const uintptr_t addr = reinterpret_cast<uintptr_t>(p);
  while (fgets(line, sizeof(line), f)) {
    unsigned long start = 0, end = 0;
    if (sscanf(line, "%lx-%lx", &start, &end) == 2 && start <= addr &&
        addr < end) {
      found = end - start;
    }
  }
  fclose(f);
  return found;
}

bool aligned_to(const void *p, size_t a) {
  return (reinterpret_cast<uintptr_t>(p) & (a - 1)) == 0;
}

// One pool for the suite: g_classes and the thread-local magazines are global,
// so separate instances would share free lists anyway.
BufferPool &pool() {
  static BufferPool p;
  return p;
}

} // namespace

TEST(BufferPool, SmallAllocIsSuitablyAligned) {
  void *p = pool().alloc(64);
  ASSERT_NE(p, nullptr);
  EXPECT_TRUE(aligned_to(p, alignof(std::max_align_t)));
  pool().free(p);
}

TEST(BufferPool, EveryClassAllocatesAndIsAlignedToItsClassSize) {
  std::vector<void *> ptrs;
  for (uint32_t cls = 0; cls < N_CLASSES; cls++) {
    const size_t want = sizeof_class(cls);
    void *p = pool().alloc(want);
    ASSERT_NE(p, nullptr) << "class " << cls;
    // Blocks are carved at run_base + k * class_size from a 64 KiB-aligned
    // run, so each one is aligned to its own class size.
    EXPECT_TRUE(aligned_to(p, want)) << "class " << cls;
    ptrs.push_back(p);
  }
  for (void *p : ptrs) {
    pool().free(p);
  }
}

TEST(BufferPool, SizesRoundUpToTheEnclosingClass) {
  // 17 and 32 both land in the 32-byte class, so freeing one and allocating
  // the other must hand back the same block.
  void *a = pool().alloc(32);
  pool().free(a);
  void *b = pool().alloc(17);
  EXPECT_EQ(a, b);
  pool().free(b);
}

TEST(BufferPool, FreeThenAllocReusesTheSameBlock) {
  void *a = pool().alloc(64);
  pool().free(a);
  void *b = pool().alloc(64);
  EXPECT_EQ(a, b) << "magazine should hand back the most recently freed block";
  pool().free(b);
}

TEST(BufferPool, DistinctAllocationsDoNotOverlap) {
  constexpr size_t kCount = 4096; // more than one run's worth for class 0
  std::set<void *> seen;
  std::vector<void *> ptrs;
  for (size_t i = 0; i < kCount; i++) {
    void *p = pool().alloc(16);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(seen.insert(p).second) << "duplicate block at i=" << i;
    ptrs.push_back(p);
  }
  for (void *p : ptrs) {
    pool().free(p);
  }
}

TEST(BufferPool, AllocatedBlocksAreWritableForTheirFullClassSize) {
  for (uint32_t cls = 0; cls < N_CLASSES; cls++) {
    const size_t want = sizeof_class(cls);
    auto *p = static_cast<unsigned char *>(pool().alloc(want));
    ASSERT_NE(p, nullptr);
    memset(p, 0xA5, want);
    EXPECT_EQ(p[0], 0xA5);
    EXPECT_EQ(p[want - 1], 0xA5);
    pool().free(p);
  }
}

TEST(BufferPool, LargeAllocationGetsItsOwnAlignedMapping) {
  constexpr size_t kWant = 16 * 1024; // above MAX_CLASS_SIZE
  void *p = pool().alloc(kWant);
  ASSERT_NE(p, nullptr);

  // The payload sits immediately after the descriptor at the chunk base, so
  // masking off the low CHUNK_SIZE bits still finds the descriptor.
  const uintptr_t base = reinterpret_cast<uintptr_t>(p) & ~(CHUNK_SIZE - 1);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(p) - base, sizeof(Chunk));
  EXPECT_TRUE(aligned_to(reinterpret_cast<void *>(base), CHUNK_SIZE));

  const auto *desc = reinterpret_cast<const Chunk *>(base);
  EXPECT_TRUE(desc->is_large);
  EXPECT_EQ(desc->sz, vma_size_at(p))
      << "descriptor size must match what was actually mapped";

  memset(p, 0x5A, kWant);
  pool().free(p);
  EXPECT_EQ(vma_size_at(reinterpret_cast<void *>(base)), 0u)
      << "free must unmap the whole large mapping";
}

TEST(BufferPool, LargeAllocationBiggerThanOneChunk) {
  // Spans more than CHUNK_SIZE, so the tail trim and the recorded size have to
  // account for the real length rather than assuming one chunk.
  constexpr size_t kWant = 3 * 1024 * 1024;
  void *p = pool().alloc(kWant);
  ASSERT_NE(p, nullptr);

  const uintptr_t base = reinterpret_cast<uintptr_t>(p) & ~(CHUNK_SIZE - 1);
  const auto *desc = reinterpret_cast<const Chunk *>(base);
  EXPECT_TRUE(desc->is_large);
  EXPECT_GE(desc->sz, kWant + sizeof(Chunk));
  EXPECT_EQ(desc->sz, vma_size_at(p));

  memset(p, 0x3C, kWant); // must be writable end to end
  pool().free(p);
  EXPECT_EQ(vma_size_at(reinterpret_cast<void *>(base)), 0u);
}

TEST(BufferPool, ReserveWarmsTheMagazineForThatClass) {
  EXPECT_TRUE(pool().reserve(64));
  // Everything after a successful reserve comes from the magazine.
  std::vector<void *> ptrs;
  for (int i = 0; i < 64; i++) {
    void *p = pool().alloc(64);
    ASSERT_NE(p, nullptr);
    ptrs.push_back(p);
  }
  for (void *p : ptrs) {
    pool().free(p);
  }
}

TEST(BufferPool, BlocksFreedOnOneThreadAreUsableOnAnother) {
  void *from_worker = nullptr;
  std::thread t([&] { from_worker = pool().alloc(128); });
  t.join();
  ASSERT_NE(from_worker, nullptr);
  pool().free(from_worker); // freed on a different thread than allocated
  void *p = pool().alloc(128);
  EXPECT_NE(p, nullptr);
  pool().free(p);
}

TEST(BufferPool, ThreadExitReturnsMagazineBlocksToTheGlobalStack) {
  // A dying thread's magazine holds blocks that are on no other list, so
  // without the pthread_key destructor they are unreachable for good.
  //
  // The observer has to be a second fresh thread: a thread with a warm
  // magazine of its own would satisfy the allocation locally and never reach
  // the global stack the flush pushes to.
  constexpr size_t kCount = 32;
  std::set<void *> donated;

  std::thread producer([&] {
    std::vector<void *> ptrs;
    for (size_t i = 0; i < kCount; i++) {
      ptrs.push_back(pool().alloc(256));
    }
    for (void *p : ptrs) {
      pool().free(p); // parked in this thread's magazine
      donated.insert(p);
    }
  });
  producer.join(); // flush_magazine runs here

  size_t recovered = 0;
  std::thread observer([&] {
    std::vector<void *> ptrs;
    for (size_t i = 0; i < kCount; i++) {
      void *p = pool().alloc(256);
      ASSERT_NE(p, nullptr);
      recovered += donated.count(p);
      ptrs.push_back(p);
    }
    for (void *p : ptrs) {
      pool().free(p);
    }
  });
  observer.join();

  EXPECT_GT(recovered, 0u) << "no block came back from the exited thread";
}
