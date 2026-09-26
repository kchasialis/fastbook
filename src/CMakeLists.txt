add_subdirectory(common)
add_subdirectory(feed)
add_subdirectory(orderbook)
add_subdirectory(bookbuilder)
add_subdirectory(transport)
add_subdirectory(pipeline)

add_executable(fastbook main.cpp)

target_link_libraries(fastbook fastbook_options common feed orderbook bookbuilder transport pipeline)