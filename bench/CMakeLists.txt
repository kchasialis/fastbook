add_executable(fastbook_bench
    bench_spsc.cpp
    bench_order_book.cpp
    bench_hashmap.cpp
)

target_link_libraries(fastbook_bench
    PRIVATE fastbook_options
    PRIVATE common
    PRIVATE orderbook
    PRIVATE benchmark::benchmark_main
)

add_executable(fastbook_bench_logger bench_logger.cpp)

target_link_libraries(fastbook_bench_logger
    PRIVATE fastbook_options
    PRIVATE common
)
