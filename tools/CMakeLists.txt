add_executable(mold_replay mold_replay.cpp)

target_link_libraries(mold_replay
    PRIVATE fastbook_options
    PRIVATE feed
)
