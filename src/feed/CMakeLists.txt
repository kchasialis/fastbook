add_library(feed INTERFACE)

target_include_directories(feed INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

target_link_libraries(feed
        INTERFACE fastbook_options
        INTERFACE spsc
)