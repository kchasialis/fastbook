add_library(datasources INTERFACE)

target_include_directories(datasources INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
