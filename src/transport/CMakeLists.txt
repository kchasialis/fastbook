add_library(transport INTERFACE)

target_include_directories(transport INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

target_link_libraries(transport
        INTERFACE fastbook_options
        INTERFACE common
)
