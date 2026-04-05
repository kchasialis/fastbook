add_library(orderbook INTERFACE)

target_include_directories(orderbook INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

target_link_libraries(orderbook
        INTERFACE fastbook_options
        INTERFACE spsc
)