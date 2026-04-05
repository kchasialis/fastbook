add_library(matching INTERFACE)

target_include_directories(matching INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

target_link_libraries(matching
        INTERFACE fastbook_options
        INTERFACE orderbook
)
