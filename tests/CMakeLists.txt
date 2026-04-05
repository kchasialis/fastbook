add_executable(fastbook_tests test_spsc.cpp test_hashmap.cpp test_order_book.cpp)

target_link_libraries(fastbook_tests
        PRIVATE fastbook_options
        PRIVATE spsc
        PRIVATE orderbook
        PRIVATE GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(fastbook_tests)