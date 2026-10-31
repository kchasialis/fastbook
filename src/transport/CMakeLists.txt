add_library(transport INTERFACE)

target_include_directories(transport INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

target_link_libraries(transport
        INTERFACE fastbook_options
        INTERFACE common
)

find_package(PkgConfig)
pkg_check_modules(LIBBPF IMPORTED_TARGET libbpf)
pkg_check_modules(LIBXDP IMPORTED_TARGET libxdp)
find_program(BPF_CLANG NAMES clang clang-21)
find_program(BPFTOOL bpftool)

if(LIBBPF_FOUND AND LIBXDP_FOUND AND BPF_CLANG AND BPFTOOL)
    set(XDP_OBJ ${CMAKE_CURRENT_BINARY_DIR}/feed_filter.bpf.o)
    set(XDP_SKEL ${CMAKE_CURRENT_BINARY_DIR}/feed_filter.skel.h)

    add_custom_command(
            OUTPUT ${XDP_OBJ}
            COMMAND ${BPF_CLANG} -O2 -g -target bpf
                    -I/usr/include/${CMAKE_LIBRARY_ARCHITECTURE}
                    -c ${CMAKE_CURRENT_SOURCE_DIR}/feed_filter.bpf.c -o ${XDP_OBJ}
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/feed_filter.bpf.c
            VERBATIM
    )
    add_custom_command(
            OUTPUT ${XDP_SKEL}
            COMMAND sh -c "${BPFTOOL} gen skeleton ${XDP_OBJ} name feed_filter > ${XDP_SKEL}"
            DEPENDS ${XDP_OBJ}
            VERBATIM
    )
    add_custom_target(feed_filter_skel DEPENDS ${XDP_SKEL})

    add_dependencies(transport feed_filter_skel)
    target_include_directories(transport SYSTEM INTERFACE
            $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    )
    target_link_libraries(transport INTERFACE PkgConfig::LIBBPF PkgConfig::LIBXDP)
    target_compile_definitions(transport INTERFACE FASTBOOK_AF_XDP)
else()
    message(STATUS "AF_XDP disabled: needs libbpf, libxdp, clang and bpftool")
endif()
