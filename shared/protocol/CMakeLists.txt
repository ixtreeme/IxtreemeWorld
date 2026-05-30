find_package(ZLIB REQUIRED)
find_package(CapnProto CONFIG REQUIRED)

# Android cross-compile: the target capnp binary is ARM64-ELF and cannot run on the
# Windows host. Use the host-build tool only for schema code generation; runtime
# Cap'n Proto libraries still come from the target arm64-android package.
if(CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(_capnp_host_dir "C:/vcpkg/installed/x64-windows-static/tools/capnproto")
    if(NOT EXISTS "${_capnp_host_dir}/capnp.exe")
        message(FATAL_ERROR
            "Cap'n Proto host-tool nem talalhato: ${_capnp_host_dir}/capnp.exe\n"
            "Telepitsd: vcpkg install capnproto:x64-windows-static"
        )
    endif()
    set(CAPNP_EXECUTABLE "${_capnp_host_dir}/capnp.exe"
        CACHE FILEPATH "Cap'n Proto compiler (host build for cross-compile)" FORCE)
    set(CAPNPC_CXX_EXECUTABLE "${_capnp_host_dir}/capnpc-c++.exe"
        CACHE FILEPATH "Cap'n Proto C++ codegen plugin (host build)" FORCE)
    message(STATUS "Android cross-compile: capnp host-tool overridden -> ${CAPNP_EXECUTABLE}")
endif()

set(SCHEMA_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/schema/packet.capnp
)

capnp_generate_cpp(CAPNP_SRCS CAPNP_HDRS ${SCHEMA_FILES})

add_library(protocol STATIC
    ${CAPNP_SRCS}
    src/ProtocolError.cpp
)

target_include_directories(protocol
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(protocol
    PUBLIC
        CapnProto::capnp
)

target_compile_features(protocol PUBLIC cxx_std_20)
