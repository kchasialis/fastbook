add_library(fastbook_options INTERFACE)

target_compile_features(fastbook_options INTERFACE cxx_std_23)

target_compile_options(fastbook_options INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-align
        -Woverloaded-virtual
        -Wnull-dereference
)

option(ENABLE_ASAN  "Enable AddressSanitizer"        OFF)
option(ENABLE_TSAN  "Enable ThreadSanitizer"         OFF)
option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)

if(ENABLE_ASAN)
    message(STATUS "fastbook: ASan enabled")
    target_compile_options(fastbook_options INTERFACE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(fastbook_options    INTERFACE -fsanitize=address)
endif()

if(ENABLE_TSAN)
    message(STATUS "fastbook: TSan enabled")
    target_compile_options(fastbook_options INTERFACE -fsanitize=thread)
    target_link_options(fastbook_options    INTERFACE -fsanitize=thread)
endif()

if(ENABLE_UBSAN)
    message(STATUS "fastbook: UBSan enabled")
    target_compile_options(fastbook_options INTERFACE -fsanitize=undefined)
    target_link_options(fastbook_options    INTERFACE -fsanitize=undefined)
endif()

if(APPLE)
    message(STATUS "fastbook: platform = macOS/ARM (M1)")
    target_compile_definitions(fastbook_options INTERFACE
            FASTBOOK_PLATFORM_MACOS
            FASTBOOK_ARCH_ARM
    )
elseif(UNIX)
    message(STATUS "fastbook: platform = Linux")
    target_compile_definitions(fastbook_options INTERFACE
            FASTBOOK_PLATFORM_LINUX
    )
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-mavx2" COMPILER_SUPPORTS_AVX2)
    if(COMPILER_SUPPORTS_AVX2)
        message(STATUS "fastbook: AVX2 available")
        target_compile_options(fastbook_options INTERFACE -mavx2)
        target_compile_definitions(fastbook_options INTERFACE FASTBOOK_ARCH_AVX2)
    endif()
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported OUTPUT ipo_output)
    if(ipo_supported)
        message(STATUS "fastbook: LTO enabled")
        set_target_properties(fastbook_options PROPERTIES
                INTERPROCEDURAL_OPTIMIZATION TRUE
        )
    endif()
endif()