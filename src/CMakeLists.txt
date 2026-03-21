add_subdirectory(spsc)
add_subdirectory(feed)
add_subdirectory(orderbook)
add_subdirectory(matching)

add_executable(fastbook main.cpp)

target_link_libraries(fastbook fastbook_options feed orderbook matching)