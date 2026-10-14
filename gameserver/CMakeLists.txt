cmake_minimum_required(VERSION 3.21)

option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_WORLDBENCH "Build the world load benchmark (dev only, never production)" OFF)

set(VCPKG_TRIPLET_TYPE "static" CACHE STRING "Choose vcpkg triplet type (static/dynamic)")
set_property(CACHE VCPKG_TRIPLET_TYPE PROPERTY STRINGS static dynamic)

if(NOT DEFINED VCPKG_ROOT)
    if(DEFINED ENV{VCPKG_ROOT})
        file(TO_CMAKE_PATH "$ENV{VCPKG_ROOT}" VCPKG_ROOT)
    elseif(CMAKE_HOST_WIN32)
        set(VCPKG_ROOT "C:/vcpkg")
    else()
        set(VCPKG_ROOT "${CMAKE_SOURCE_DIR}/vcpkg")
    endif()
endif()

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE
        "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE FILEPATH "Vcpkg toolchain file")
endif()

if(ENABLE_ASAN)
    set(VCPKG_TARGET_TRIPLET "x64-windows" CACHE STRING "vcpkg triplet" FORCE)
    set(VCPKG_TRIPLET_TYPE "dynamic" CACHE STRING "" FORCE)
elseif(NOT DEFINED VCPKG_TARGET_TRIPLET AND CMAKE_HOST_WIN32)
    if(VCPKG_TRIPLET_TYPE STREQUAL "dynamic")
        set(VCPKG_TARGET_TRIPLET "x64-windows" CACHE STRING "vcpkg triplet")
    else()
        set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "vcpkg triplet")
    endif()
endif()

project(gameserver
    VERSION 0.1.0
    DESCRIPTION "GameServer for the new MMORPG project"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_ASAN "Enable AddressSanitizer" OFF)

set(VCPKG_TRIPLET_TYPE "static" CACHE STRING "Choose vcpkg triplet type (static/dynamic)")
set_property(CACHE VCPKG_TRIPLET_TYPE PROPERTY STRINGS static dynamic)


if(ENABLE_ASAN)
    set(CMAKE_MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
        CACHE STRING "MSVC runtime library" FORCE
    )
    if(VCPKG_TARGET_TRIPLET STREQUAL "x64-windows-static")
        set(VCPKG_TARGET_TRIPLET "x64-windows" CACHE STRING "vcpkg triplet" FORCE)
        set(VCPKG_TRIPLET_TYPE "dynamic" CACHE STRING "" FORCE)
    endif()
elseif(VCPKG_TRIPLET_TYPE STREQUAL "static")
    set(CMAKE_MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        CACHE STRING "MSVC runtime library" FORCE
    )
else()
    set(CMAKE_MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
        CACHE STRING "MSVC runtime library" FORCE
    )
endif()

if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8 /MP /EHsc)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Dev-only sanitizer instrumentation. OFF by default; production builds are
# unaffected. MSVC supports AddressSanitizer (/fsanitize=address, VS2019
# 16.9+). ThreadSanitizer and UBSan are NOT available on MSVC -- use
# clang/gcc builds on other platforms for those.
if(ENABLE_ASAN)
    if(MSVC)
        add_compile_options(/fsanitize=address)
        add_link_options(/fsanitize=address)
    else()
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address)
    endif()
endif()

include(cmake/BootstrapVcpkg.cmake)

vcpkg_require(
	boost-asio
	boost-system
	boost-throw-exception
	capnproto
	spdlog
	fmt
	zlib
	mariadb-connector-cpp
	libsodium
	nlohmann-json
	flecs
)

find_package(Boost CONFIG REQUIRED COMPONENTS asio system throw_exception)

add_subdirectory(libs/common)
# A protocol lib a kozos shared/ mappaban el, mindket oldal hasznalja.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../shared/protocol shared_protocol_build)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../shared/map shared_map_build)
add_subdirectory(libs/network)
add_subdirectory(libs/db)
add_subdirectory(apps/gameserver)
