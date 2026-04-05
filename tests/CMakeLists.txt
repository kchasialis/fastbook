add_executable(fastbook_tests test_spsc.cpp test_hashmap.cpp test_order_book.cpp test_feed_handler.cpp test_matching_engine.cpp)

target_link_libraries(fastbook_tests
        PRIVATE fastbook_options
        PRIVATE spsc
        PRIVATE orderbook
        PRIVATE feed
        PRIVATE matching
        PRIVATE GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(fastbook_tests)