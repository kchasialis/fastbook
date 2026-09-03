#pragma once

#include "treiber_stack.hpp"
#include "utils.hpp"
#include <array>
#include <bit>
#include <cassert>
#include <cstdint>
#include <memory>
#include <new>
#include <sys/mman.h>
#include <utility>

static constexpr uint32_t MIN_CLASS_SIZE = 16;
static constexpr uint32_t MAX_CLASS_SIZE = 8192;
static constexpr uint32_t MIN_SHIFT = std::countr_zero(MIN_CLASS_SIZE);
static constexpr uint32_t MAX_SHIFT = std::countr_zero(MAX_CLASS_SIZE);
static_assert(std::has_single_bit(MIN_CLASS_SIZE) &&
              std::has_single_bit(MAX_CLASS_SIZE));
static constexpr size_t N_CLASSES = MAX_SHIFT - MIN_SHIFT + 1;
static constexpr size_t CHUNK_SIZE = 2 * 1024 * 1024ULL;
static constexpr size_t RUN_SIZE = 64 * 1024ULL;
static constexpr size_t N_RUNS = CHUNK_SIZE / RUN_SIZE;

class Bitmap {
private:
  uint32_t data_;

public:
  // Bit 0 is set because run 0 holds the ChunkDesc and is never claimable.
  Bitmap() : data_(1) {}
  void clear_bit(uint32_t i) noexcept { data_ &= ~(1U << i); }
  void set_bit(uint32_t i) noexcept { data_ |= (1U << i); }
  bool get_bit(uint32_t i) const noexcept { return data_ & (1U << i); }
  uint32_t first_empty() const noexcept { return std::countr_one(data_); }
};

struct Block {
  Block *next;
};

template <typename T> using Untagged = PackedTag<T, 0>;
inline TreiberStack<Block, &Block::next, Untagged> classes[N_CLASSES];

// Returns the class index for this sz.
inline uint32_t class_of(size_t sz) noexcept {
  size_t ceil = std::bit_ceil(sz);
  uint32_t exp = std::countr_zero(ceil);
  if (exp < MIN_SHIFT) [[unlikely]] {
    return 0;
  }
  assert(exp <= MAX_SHIFT);

  return exp - MIN_SHIFT;
}

// Returns the size of the class index.
inline uint32_t sizeof_class(uint32_t class_idx) noexcept {
  return 1 << (class_idx + MIN_SHIFT);
}

struct Chunk {
  std::array<uint8_t, N_RUNS> run_class{0};
  static_assert(N_RUNS <= 32);
  Bitmap bmap{};
  uint32_t free_runs{N_RUNS - 1};
  Chunk *next{};

  bool has_free_runs() const noexcept { return free_runs > 0; }

  void *claim_run(uint8_t cls) noexcept {
    assert(has_free_runs());

    uint32_t first_free = bmap.first_empty();
    if (first_free >= N_RUNS) [[unlikely]] {
      return nullptr;
    }

    run_class[first_free] = cls;
    char *base = (char *)this + first_free * RUN_SIZE;
    bmap.set_bit(first_free);
    --free_runs;

    return base;
  }
};

struct ThreadState {
  static constexpr uint32_t TL_CACHE_SIZE = 64;
};

inline thread_local ThreadState tstate = {};

class BufferPool {

private:
  template <typename T>
  using PackedTagAlias = PackedTag<T, std::countr_zero(CHUNK_SIZE)>;
  TreiberStack<Chunk, &Chunk::next, PackedTagAlias> chunks_with_free_runs;

  static std::pair<Block *, Block *> carve_run_in_blocks(void *base,
                                                         uint32_t cls) {
    Block *head = (Block *)base;
    Block *current = head;
    uint32_t class_sz = sizeof_class(cls);
    uint32_t n_blocks = RUN_SIZE / class_sz;
    assert(RUN_SIZE % class_sz == 0);

    for (uint32_t i = 0; i < n_blocks - 1; i++) {
      current->next = reinterpret_cast<Block *>(
          reinterpret_cast<char *>(current) + class_sz);
      current = current->next;
    }
    current->next = nullptr;

    return {head, current};
  }

  Chunk *allocate_new_chunk() noexcept {
    size_t alloc_size = CHUNK_SIZE;
    void *chunk_aligned =
        mmap(NULL, CHUNK_SIZE, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_2MB, -1, 0);
    if (chunk_aligned == MAP_FAILED) [[unlikely]] {
      // system huge tables are exhausted, try fallback path.
      alloc_size *= 2;
      void *chunk_raw = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (chunk_raw == MAP_FAILED) [[unlikely]] {
        return nullptr;
      }

      size_t space = alloc_size;
      chunk_aligned = chunk_raw;
      chunk_aligned = std::align(CHUNK_SIZE, CHUNK_SIZE, chunk_aligned, space);
      assert(chunk_aligned);
      if (madvise(chunk_aligned, CHUNK_SIZE, MADV_HUGEPAGE) < 0) {
        munmap(chunk_raw, alloc_size);
        return nullptr;
      }

      size_t head_slack = alloc_size - space;
      size_t tail_slack = space - CHUNK_SIZE;
      if (head_slack) {
        munmap(chunk_raw, head_slack);
      }
      if (tail_slack) {
        munmap(reinterpret_cast<void *>(
                   reinterpret_cast<uintptr_t>(chunk_aligned) + CHUNK_SIZE),
               tail_slack);
      }
    }

    if ((reinterpret_cast<uintptr_t>(chunk_aligned) & (CHUNK_SIZE - 1)) != 0)
        [[unlikely]] {
      munmap(chunk_aligned, CHUNK_SIZE);
      return nullptr;
    }

    // mmap returns 0d memory, need to construct a Chunk in-place.
    return new (chunk_aligned) Chunk();
  }

  bool refill(uint8_t cls) noexcept {
    Chunk *free_chunk = chunks_with_free_runs.pop();
    if (!free_chunk) {
      if (!(free_chunk = allocate_new_chunk())) [[unlikely]] {
        return false;
      }
    }

    void *base = free_chunk->claim_run(cls);
    if (!base) [[unlikely]] {
      // The chunk was on the free-run list but had none.
      return false;
    }

    auto p = carve_run_in_blocks(base, cls);
    classes[cls].push_chain(p.first, p.second);

    if (free_chunk->has_free_runs()) {
      chunks_with_free_runs.push(free_chunk);
    }

    return true;
  }
};