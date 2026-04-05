add_library(bookbuilder INTERFACE)

target_include_directories(bookbuilder INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

target_link_libraries(bookbuilder
    INTERFACE fastbook_options
    INTERFACE spsc
    INTERFACE feed
    INTERFACE matching
)