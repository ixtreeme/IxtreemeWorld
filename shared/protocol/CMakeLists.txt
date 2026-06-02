cmake_minimum_required(VERSION 3.21)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    if(NOT DEFINED VCPKG_ROOT)
        if(DEFINED ENV{VCPKG_ROOT})
            file(TO_CMAKE_PATH "$ENV{VCPKG_ROOT}" VCPKG_ROOT)
        elseif(WIN32)
            set(VCPKG_ROOT "C:/vcpkg")
        else()
            set(VCPKG_ROOT "${CMAKE_SOURCE_DIR}/vcpkg")
        endif()
    endif()

    set(VCPKG_TRIPLET_TYPE "static" CACHE STRING "static or dynamic")
    if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
        set(CMAKE_TOOLCHAIN_FILE
            "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
            CACHE FILEPATH "Vcpkg toolchain file"
        )
    endif()
    if(NOT DEFINED VCPKG_TARGET_TRIPLET)
        set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "vcpkg triplet")
    endif()

    project(ixtreeme_protocol LANGUAGES CXX)

    include("${CMAKE_CURRENT_LIST_DIR}/../../gameserver/cmake/BootstrapVcpkg.cmake")
    vcpkg_require(zlib capnproto)
endif()

if(DEFINED VCPKG_ROOT AND DEFINED VCPKG_TARGET_TRIPLET)
    set(_protocol_vcpkg_prefix "${VCPKG_ROOT}/installed/${VCPKG_TARGET_TRIPLET}")
    if(EXISTS "${_protocol_vcpkg_prefix}")
        list(PREPEND CMAKE_PREFIX_PATH "${_protocol_vcpkg_prefix}")
        set(ZLIB_ROOT "${_protocol_vcpkg_prefix}" CACHE PATH "ZLIB root from vcpkg")
        set(ZLIB_INCLUDE_DIR "${_protocol_vcpkg_prefix}/include" CACHE PATH "ZLIB include dir from vcpkg" FORCE)

        foreach(_zlib_release_name zs.lib zlib.lib libz.a)
            if(EXISTS "${_protocol_vcpkg_prefix}/lib/${_zlib_release_name}")
                set(ZLIB_LIBRARY_RELEASE "${_protocol_vcpkg_prefix}/lib/${_zlib_release_name}"
                    CACHE FILEPATH "ZLIB release library from vcpkg" FORCE)
                break()
            endif()
        endforeach()

        foreach(_zlib_debug_name zsd.lib zlibd.lib zlib.lib libz.a)
            if(EXISTS "${_protocol_vcpkg_prefix}/debug/lib/${_zlib_debug_name}")
                set(ZLIB_LIBRARY_DEBUG "${_protocol_vcpkg_prefix}/debug/lib/${_zlib_debug_name}"
                    CACHE FILEPATH "ZLIB debug library from vcpkg" FORCE)
                break()
            endif()
        endforeach()
    endif()
endif()

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
        ZLIB::ZLIB
)

target_compile_features(protocol PUBLIC cxx_std_20)
