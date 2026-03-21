add_library(spsc INTERFACE)

target_include_directories(spsc INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

target_link_libraries(spsc INTERFACE fastbook_options)