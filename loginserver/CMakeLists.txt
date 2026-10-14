cmake_minimum_required(VERSION 3.21)

option(ENABLE_ASAN "Enable AddressSanitizer" OFF)

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

project(loginserver
    VERSION 0.1.0
    DESCRIPTION "LoginServer for the IxtreemeWorld MMORPG project"
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
)

find_package(Boost CONFIG REQUIRED COMPONENTS asio system throw_exception)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../gameserver/libs/common gameserver_common_build)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../shared/protocol shared_protocol_build)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../gameserver/libs/network gameserver_network_build)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../gameserver/libs/db gameserver_db_build)
add_subdirectory(apps/loginserver)
