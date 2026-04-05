add_library(feedreader INTERFACE)

target_include_directories(feedreader INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

target_link_libraries(feedreader
    INTERFACE fastbook_options
    INTERFACE spsc
    INTERFACE feed
)