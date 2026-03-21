add_library(matching STATIC
        matching_engine.cpp
)

target_include_directories(matching PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

target_link_libraries(matching
        PRIVATE fastbook_options
        PUBLIC  orderbook
)