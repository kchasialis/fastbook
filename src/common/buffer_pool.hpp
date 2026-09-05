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
static constexpr uint32_t RUN_SHIFT = std::countr_zero(RUN_SIZE);
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
inline TreiberStack<Block, &Block::next, Untagged> g_classes[N_CLASSES];

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
inline size_t round_to_cnk_sz(size_t val) {
  return (val + CHUNK_SIZE - 1) & ~(CHUNK_SIZE - 1);
}

struct alignas(64) Chunk {
  std::array<uint8_t, N_RUNS> run_class{0};
  static_assert(N_RUNS <= 32);
  Bitmap bmap{};
  uint32_t free_runs{N_RUNS - 1};
  Chunk *next{};

  bool is_large{false};
  // sz is only used when dealing with large allocations.
  size_t sz{0};

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

static_assert(alignof(Chunk) >= alignof(std::max_align_t));

/* For this to work, ptr must be inside a CHUNK_SIZE-aligned Chunk. */
inline std::pair<Chunk *, uint8_t> base_cls_from_ptr(void *ptr) noexcept {
  uintptr_t ptr_n = reinterpret_cast<uintptr_t>(ptr);
  uintptr_t base = ptr_n & ~(CHUNK_SIZE - 1);
  size_t off = ptr_n - base;
  size_t run = off >> RUN_SHIFT;
  Chunk *cnk = reinterpret_cast<Chunk *>(base);

  return {cnk, cnk->run_class[run]};
}

class TLMagazine {
private:
  static constexpr uint32_t BATCH_SIZE = 16;
  static constexpr uint32_t N_BATCHES = 64;
  static constexpr uint32_t CACHE_SIZE = BATCH_SIZE * N_BATCHES;

  struct ClassState {
    std::array<Block *, CACHE_SIZE> stk_{nullptr};
    size_t top_{0};
  };

  std::array<ClassState, N_CLASSES> classes_{};

  bool take_batch(uint8_t cls) noexcept {
    auto &state = classes_[cls];
    Block *head = g_classes[cls].take_all();
    bool found = head != nullptr;
    Block *tail = head;
    for (size_t i = 0; i < BATCH_SIZE && head; i++) {
      state.stk_[state.top_++] = head;
      head = head->next;
      tail = head;
    }

    /* Need to walk to find the tail, known and deliberate design limitation. */
    Block *current = head;
    while (current) {
      tail = current;
      current = current->next;
    }

    if (head) {
      g_classes[cls].push_chain(head, tail);
    }

    return found;
  }

  void donate_batch(uint8_t cls) noexcept {
    auto &state = classes_[cls];

    // we are certain that we have at least BATCH_SIZE elements because this
    // is called when the stack is full (a multiple of BATCH_SIZE)
    assert(state.top_ >= BATCH_SIZE);

    Block *head = state.stk_[--state.top_];
    Block *current = head;
    for (uint32_t i = 0; i < BATCH_SIZE - 1; i++) {
      current->next = state.stk_[--state.top_];
      current = current->next;
    }
    current->next = nullptr;

    g_classes[cls].push_chain(head, current);
  }

public:
  Block *pop(uint8_t cls) noexcept {
    auto &state = classes_[cls];
    if (state.top_ == 0) {
      if (!take_batch(cls)) {
        return nullptr;
      }
    }

    return state.stk_[--state.top_];
  }

  void push(Block *blk, uint8_t cls) noexcept {
    auto &state = classes_[cls];
    if (state.top_ == state.stk_.size()) {
      donate_batch(cls);
    }

    state.stk_[state.top_++] = blk;
  }

  void refill_from_chain(size_t cls, Block *head, Block *tail) noexcept {
    auto &state = classes_[cls];
    while (head && state.top_ < state.stk_.size()) {
      state.stk_[state.top_++] = head;
      head = head->next;
    }

    if (head) {
      g_classes[cls].push_chain(head, tail);
    }
  }
};

inline thread_local TLMagazine magazine = {};

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

  // sz must be CHUNK_SIZE aligned.
  Chunk *allocate_new_chunk(size_t sz) noexcept {
    size_t alloc_size = sz;
    void *chunk_aligned =
        mmap(NULL, alloc_size, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_2MB, -1, 0);
    if (chunk_aligned == MAP_FAILED) [[unlikely]] {
      // system huge tables are exhausted, try fallback path.
      alloc_size += CHUNK_SIZE;
      void *chunk_raw = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (chunk_raw == MAP_FAILED) [[unlikely]] {
        return nullptr;
      }

      size_t space = alloc_size;
      chunk_aligned = chunk_raw;
      chunk_aligned = std::align(CHUNK_SIZE, sz, chunk_aligned, space);
      assert(chunk_aligned);
      if (madvise(chunk_aligned, sz, MADV_HUGEPAGE) < 0) {
        [[maybe_unused]] int rc = munmap(chunk_raw, alloc_size);
        assert(rc == 0);
        return nullptr;
      }

      size_t head_slack = alloc_size - space;
      size_t tail_slack = space - sz;
      if (head_slack) {
        [[maybe_unused]] int rc = munmap(chunk_raw, head_slack);
        assert(rc == 0);
      }
      if (tail_slack) {
        [[maybe_unused]] int rc =
            munmap(reinterpret_cast<void *>(
                       reinterpret_cast<uintptr_t>(chunk_aligned) + sz),
                   tail_slack);
        assert(rc == 0);
      }
    }

    if ((reinterpret_cast<uintptr_t>(chunk_aligned) & (CHUNK_SIZE - 1)) != 0)
        [[unlikely]] {
      [[maybe_unused]] int rc = munmap(chunk_aligned, alloc_size);
      assert(rc == 0);
      return nullptr;
    }

    // mmap returns 0d memory, need to construct a Chunk in-place.
    return new (chunk_aligned) Chunk();
  }

  bool refill(uint8_t cls) noexcept {
    Chunk *free_chunk = chunks_with_free_runs.pop();
    if (!free_chunk) {
      if (!(free_chunk = allocate_new_chunk(CHUNK_SIZE))) [[unlikely]] {
        return false;
      }
    }

    void *base = free_chunk->claim_run(cls);
    if (!base) [[unlikely]] {
      // The chunk was on the free-run list but had none.
      return false;
    }

    auto p = carve_run_in_blocks(base, cls);
    magazine.refill_from_chain(cls, p.first, p.second);

    if (free_chunk->has_free_runs()) {
      chunks_with_free_runs.push(free_chunk);
    }

    return true;
  }

  void *alloc(size_t sz) noexcept {
    if (sz > MAX_CLASS_SIZE) [[unlikely]] {
      size_t alloc_size = round_to_cnk_sz(sz + sizeof(Chunk));
      Chunk *cnk = allocate_new_chunk(alloc_size);
      if (!cnk) [[unlikely]] {
        return nullptr;
      }
      cnk->is_large = true;
      cnk->sz = alloc_size;
      return reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(cnk) +
                                      sizeof(*cnk));
    }

    size_t cls = class_of(sz);

    void *p = magazine.pop(cls);
    if (!p) {
      if (!refill(cls)) [[unlikely]] {
        return nullptr;
      }
      p = magazine.pop(cls);
    }

    return p;
  }

  void free(void *ptr) noexcept {
    auto p = base_cls_from_ptr(ptr);
    if (p.first->is_large) [[unlikely]] {
      munmap(p.first, p.first->sz);
      return;
    }

    magazine.push(reinterpret_cast<Block *>(ptr), p.second);
  }
};