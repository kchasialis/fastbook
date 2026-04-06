add_executable(fastbook_bench
    bench_spsc.cpp
    bench_order_book.cpp
)

target_link_libraries(fastbook_bench
    PRIVATE fastbook_options
    PRIVATE spsc
    PRIVATE orderbook
    PRIVATE benchmark::benchmark_main
)
