#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <thread>
#include <vector>

#include "buffer_pool.hpp"

using namespace bufpool;

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

TEST(BufferPool, SmallAllocationsAreAlignedWritableAndDistinct) {
  for (uint32_t cls = 0; cls < N_CLASSES; cls++) {
    const size_t want = sizeof_class(cls);
    void *p = pool().alloc(want);
    ASSERT_NE(p, nullptr) << "class " << cls;
    EXPECT_TRUE(aligned_to(p, want)) << "class " << cls;
    memset(p, 0xA5, want);
    pool().free(p);
  }

  std::set<void *> seen;
  std::vector<void *> ptrs;
  for (size_t i = 0; i < 4096; i++) {
    void *p = pool().alloc(16);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(seen.insert(p).second) << "duplicate block at i=" << i;
    ptrs.push_back(p);
  }
  for (void *p : ptrs) {
    pool().free(p);
  }
}

TEST(BufferPool, FreedBlockIsReusedBySameSizeClass) {
  void *a = pool().alloc(32);
  pool().free(a);
  void *b = pool().alloc(17);
  EXPECT_EQ(a, b);
  pool().free(b);
}

TEST(BufferPool, LargeAllocationIsMappedAndUnmapped) {
  constexpr size_t kWant = 3 * 1024 * 1024;
  void *p = pool().alloc(kWant);
  ASSERT_NE(p, nullptr);
  EXPECT_GE(vma_size_at(p), kWant);

  memset(p, 0x3C, kWant);
  pool().free(p);
  EXPECT_EQ(vma_size_at(p), 0u);
}

TEST(BufferPool, BlocksFreedOnOneThreadAreUsableOnAnother) {
  void *from_worker = nullptr;
  std::thread t([&] { from_worker = pool().alloc(128); });
  t.join();
  ASSERT_NE(from_worker, nullptr);
  pool().free(from_worker);
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
      pool().free(p);
      donated.insert(p);
    }
  });
  producer.join();

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
