# Standalone build of the MMO client scene script (MmoClient.cpp) as a native game module. Like
# hello_module it uses ONLY the SDK headers. Usually you do not need this file: copy MmoClient.cpp into
# your project's script folder and the editor builds it. To build it on its own:
#
#   cmake -S . -B build
#   cmake --build build --config Release
#
# then copy build/Release/MmoClientGame.dll into <YourProject>/Binaries/.
#
# REQUIREMENT: the SAME MSVC toolset + static runtime (/MT) + C++20 as the engine binary.

cmake_minimum_required(VERSION 3.20)
project(MmoClientGame CXX)

add_library(MmoClientGame MODULE MmoClient.cpp)

set_target_properties(MmoClientGame PROPERTIES
    PREFIX ""
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

target_compile_definitions(MmoClientGame PRIVATE IXTREEME_GAME_MODULE=1)
target_include_directories(MmoClientGame PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../include")

if(MSVC)
    set_property(TARGET MmoClientGame PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
