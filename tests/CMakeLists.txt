add_executable(fastbook_tests test_spsc.cpp)

target_link_libraries(fastbook_tests
        PRIVATE fastbook_options
        PRIVATE spsc
        PRIVATE GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(fastbook_tests)