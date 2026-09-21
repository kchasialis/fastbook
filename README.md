# fastbook

A low-latency market-data pipeline in C++23. It consumes exchange market-data
feeds and reconstructs a limit order book per instrument - the trading-firm
side of the market, reading the exchange's public feed.

Feed ingest (io_uring TCP, UDP multicast) → parsing → lock-free SPSC
queues sharded by instrument → per-shard order books, with a size-class buffer
pool and an asynchronous logger underneath.

Linux x86-64. Work in progress.
