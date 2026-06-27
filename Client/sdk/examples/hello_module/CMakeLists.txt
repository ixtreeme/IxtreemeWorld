# Standalone build for a native C++ game module. Builds HelloGame.dll against ONLY the IxtreemeWorld
# SDK headers — no engine source, no engine library. Configure/build this folder on its own:
#
#   cmake -S . -B build
#   cmake --build build --config Release
#
# Then copy build/Release/HelloGame.dll into <YourProject>/Binaries/ and open the project in the engine.
#
# REQUIREMENT: build with the SAME MSVC toolset + static runtime (/MT) + C++20 as the engine binary.
# The plugin boundary is C++ ABI; a mismatched toolchain/runtime corrupts silently.

cmake_minimum_required(VERSION 3.20)
project(HelloGame CXX)

# A game module is a runtime-loaded shared library (MODULE, not SHARED — never linked, only dlopen'd).
add_library(HelloGame MODULE HelloScript.cpp)

set_target_properties(HelloGame PROPERTIES
    PREFIX ""                   # -> "HelloGame.dll", not "libHelloGame.dll"
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

# The SDK headers (this example sits at <engine>/sdk/examples/hello_module; the SDK is ../../include).
target_include_directories(HelloGame PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../include")

if(MSVC)
    # Match the engine's STATIC CRT so the C++/heap ABI lines up across the load boundary.
    set_property(TARGET HelloGame PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
