add_library(pipeline INTERFACE)

target_include_directories(pipeline INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

# The only target that depends on both sides: FeedHandler owns a VenueFeed and
# N BookBuilders, so it sits above feed and bookbuilder rather than inside
# either. Keeping it here is what stops the dependency becoming circular.
target_link_libraries(pipeline
    INTERFACE fastbook_options
    INTERFACE common
    INTERFACE feed
    INTERFACE bookbuilder
    INTERFACE transport
)
