add_library(orderbook STATIC order_book.cpp)

target_include_directories(orderbook PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

target_link_libraries(orderbook
        PRIVATE fastbook_options
        PUBLIC  spsc
)