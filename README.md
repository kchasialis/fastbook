# fastbook

A low-latency market-data pipeline in C++23 that turns exchange feeds into full-depth limit order books,
one per instrument, the way a trading firm's feed handler does. It is the firm's side of the market
only: the exchange already does the matching and publishes the result on its feed, so there is no
matching engine here.

**Feeds:** NASDAQ TotalView-ITCH 5.0 over MoldUDP64 today. The pipeline is generic over the transport,
the framing and the message format (each one is a C++ concept), so a new venue needs its own framer and
parser. Work in progress: more feed handlers are planned.

## Pipeline

```text
 UDP multicast (recvmmsg)  ─┐
 file replay               ─┼─▶ MoldUDP64 framer ─▶ ITCH 5.0 parser ─▶ shard by instrument
 TCP (io_uring)            ─┘   gap detection,                              │
 AF_XDP (in progress)           retransmit requests          one SPSC ring per builder
                                                                            ▼
                                                   book builder threads: one order book
                                                   per instrument, each on its own core
```

- **Feed thread:** reads datagrams, checks MoldUDP64 sequence numbers (requests a retransmission on a
  gap), parses ITCH messages and routes each event to the builder that owns its instrument.
- **Book builders:** each owns a set of instruments and applies add, execute, cancel, delete and replace
  events to their books. Nothing is shared between builders.

## Design

- **No exceptions on the hot path, and no allocation in steady state.** Memory comes from the two pools
  below, and hash tables are allocated and page-faulted up front. A book is allocated on its
  instrument's first message, and grows in chunks if it outgrows its order pool.
- **Buffer pool** ([buffer_pool.hpp](src/common/buffer_pool.hpp)): a size-class allocator built from
  scratch.
  - Power-of-two size classes from 16 B to 8 KiB, carved out of 2 MiB chunks backed by huge pages.
  - Each chunk is split into 64 KiB runs, one size class per run, and the chunk header records each
    run's class, so `free()` finds the class from the pointer alone, with no per-allocation header.
  - **Thread-local cache:** every thread keeps its own free blocks per class, so most allocations and
    frees are a push or a pop on a thread-local array, with no atomics. Blocks move between threads in
    batches through lock-free Treiber stacks.
- **Object pool** ([object_pool.hpp](src/common/object_pool.hpp)): a stack of pre-constructed objects,
  where getting and returning an object are O(1) with no atomics, plus an optional reset policy for
  returned objects. It holds the orders of each book (growing with memory from the buffer pool), the
  datagram buffers of the UDP source and the request slots of the io_uring TCP source.
- **Lock-free SPSC rings** between threads, with producer and consumer state on separate cache lines and
  a cached copy of the other side's index, so the shared line is only read when the ring looks full or
  empty.
- **Order book:** the best 8 price levels per side live in a small hot array, and up to 8,192 more in a
  sorted cold array, which is deep enough for every symbol in a full NASDAQ day. Orders at a level form an intrusive doubly linked list, and an open-addressing hash table maps
  order ids to orders.
- **Asynchronous logger:** a log call stores a pointer to compile-time metadata and the raw arguments
  into a per-thread ring, and a background thread formats and writes. About 70 cycles per call.
- **Profiling library** ([profiling.hpp](src/common/profiling.hpp)): `rdtscp`-based timing,
  log-linear histograms and hardware counters through `perf_event_open`.
- Threads are pinned to cores from a config file ([config/](config/)), optionally with `SCHED_FIFO`.

## Tick-to-book latency

The time from the feed thread taking a datagram from the socket to the book being updated with a
message from it, for every message of a full NASDAQ trading day (the 30 Dec 2019 ITCH capture, 263M
messages). Both ends are `rdtscp` reads.

| Replay rate | p50 | p99 | p99.9 |
|---|---|---|---|
| 260K msgs/s | 3.0 µs | 9.7 µs | 801 µs |
| 1.08M msgs/s | 3.3 µs | 14.2 µs | 4.7 ms |

- **Setup:** `mold_replay` sends the capture as MoldUDP64 multicast at a fixed rate, on the same machine,
  so the NIC is not involved. Both runs had no sequence gaps and no dropped orders.
- **Machine:** Hetzner AX41 (bare metal), AMD Ryzen 5 3600 at a fixed 3.6 GHz, SMT off. Linux 7.0,
  gcc 15.2, `-O3 -mavx2`.
- **Threads:** the feed thread on core 3 and two book builders on cores 4 and 5, all in one L3 group and
  `SCHED_FIFO`. The replay runs on core 1. Cores 1–5 are isolated (`isolcpus`, `nohz_full`,
  `rcu_nocbs`, all IRQs on core 0). The setup scripts are in [tools/benchbox](tools/benchbox).
- **The p50 is a few µs** because a datagram carries about 44 messages and is stamped once, so each
  message also waits for the ones ahead of it.
- **The tail is queueing:** when a builder falls behind and its ring fills up, the feed thread waits,
  and so does every message behind it.

## Write-ups

- [Hash tables for the order map](https://kchasialis.dev/blog/order-map-hash-tables/): linear probing vs
  SwissTable (SSE2, AVX2, SWAR, scalar) vs `std::unordered_map` and `std::map` on a full trading day.
- [Taking logging off the hot path](https://kchasialis.dev/blog/async-logger/): the asynchronous logger
  and its benchmarks.

## Build

Linux x86-64, CMake 3.25+, gcc 15. GoogleTest and Google Benchmark are fetched by CMake.
AF_XDP support is built when libbpf, libxdp, bpftool and clang are found.

```sh
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release -DFASTBOOK_N_SHARDS=2
cmake --build build/release
ctest --test-dir build/release
```

`FASTBOOK_N_SHARDS` is the number of book builder threads, and the thread config must list one
`builder:` core for each. Run on a capture file, or on a live MoldUDP64 multicast feed:

```sh
build/release/bin/fastbook -e nasdaq -c config/benchbox.conf -f 12302019.NASDAQ_ITCH50
build/release/bin/fastbook -e nasdaq -c config/benchbox.conf -g 239.1.1.1 -p 5000 -P 6000
```

`mold_replay` ([tools/mold_replay.cpp](tools/mold_replay.cpp)) replays a capture as MoldUDP64 at a
fixed rate and answers retransmission requests.

## Layout

| Directory | What |
|---|---|
| [src/transport](src/transport) | file, UDP multicast, TCP (io_uring) and AF_XDP sources |
| [src/feed](src/feed) | MoldUDP64 framing and recovery, ITCH 5.0 parser |
| [src/orderbook](src/orderbook) | order book and the two-tier price levels |
| [src/bookbuilder](src/bookbuilder), [src/pipeline](src/pipeline) | builder threads and the feed handler that wires them |
| [src/common](src/common) | SPSC ring, pools, hash tables, logger, profiling |
| [bench](bench), [tools/benchbox](tools/benchbox) | benchmarks and the scripts that run them on the benchmark machine |
| [tests](tests) | GoogleTest unit tests |
