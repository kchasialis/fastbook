add_library(feed STATIC feed_handler.cpp)

target_include_directories(feed PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

target_link_libraries(feed
        PRIVATE fastbook_options
        PUBLIC spsc
)