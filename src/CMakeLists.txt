add_subdirectory(spsc)
add_subdirectory(feed)
add_subdirectory(orderbook)
add_subdirectory(matching)
add_subdirectory(bookbuilder)
add_subdirectory(feedreader)
add_subdirectory(datasources)

add_executable(fastbook main.cpp)

target_link_libraries(fastbook fastbook_options feed orderbook matching bookbuilder feedreader datasources)