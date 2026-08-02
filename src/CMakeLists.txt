add_subdirectory(spsc)
add_subdirectory(feed)
add_subdirectory(orderbook)
add_subdirectory(bookbuilder)
add_subdirectory(feedreader)
add_subdirectory(transport)

add_executable(fastbook main.cpp)

target_link_libraries(fastbook fastbook_options feed orderbook bookbuilder feedreader transport)