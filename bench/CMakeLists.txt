add_executable(fastbook_bench
        bench_spsc.cpp
)

target_link_libraries(fastbook_bench
        PRIVATE fastbook_options
        PRIVATE spsc
        PRIVATE benchmark::benchmark_main
)