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

project(ixtreeme_client
    VERSION 0.1.0
    DESCRIPTION "IxtreemeWorld Vulkan client"
    LANGUAGES C CXX)

include(CTest)

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
    boost-system
    capnproto
    fastgltf
    flecs
	zlib
    stb
)

set(CLIENT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(CLIENT_LIBS_DIR "${CLIENT_ROOT}/libs")

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../shared/protocol shared_protocol_build)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../shared/map shared_map_build)

find_package(fastgltf CONFIG REQUIRED)
set(STB_INCLUDE_DIR "" CACHE PATH "stb include directory")
if(DEFINED VCPKG_ROOT AND DEFINED VCPKG_TARGET_TRIPLET)
    set(_client_vcpkg_prefix "${VCPKG_ROOT}/installed/${VCPKG_TARGET_TRIPLET}")
    if(EXISTS "${_client_vcpkg_prefix}/share/stb/FindStb.cmake")
        list(PREPEND CMAKE_MODULE_PATH "${_client_vcpkg_prefix}/share/stb")
    endif()
    if(EXISTS "${_client_vcpkg_prefix}/include/stb_image.h")
        set(STB_INCLUDE_DIR "${_client_vcpkg_prefix}/include" CACHE PATH "stb include directory" FORCE)
        set(Stb_INCLUDE_DIR "${STB_INCLUDE_DIR}" CACHE PATH "stb include directory" FORCE)
    endif()
endif()
find_package(Stb QUIET)
if(NOT STB_INCLUDE_DIR AND Stb_INCLUDE_DIR)
    set(STB_INCLUDE_DIR "${Stb_INCLUDE_DIR}" CACHE PATH "stb include directory" FORCE)
endif()
if(NOT STB_INCLUDE_DIR)
    message(FATAL_ERROR "stb_image.h not found. Install stb with vcpkg or set STB_INCLUDE_DIR.")
endif()
find_package(flecs CONFIG REQUIRED)
find_package(Boost CONFIG REQUIRED COMPONENTS system)

if(TARGET flecs::flecs_static)
    set(CLIENT_FLECS_TARGET flecs::flecs_static)
elseif(TARGET flecs::flecs)
    set(CLIENT_FLECS_TARGET flecs::flecs)
else()
    message(FATAL_ERROR "flecs CMake target not found")
endif()

set(OZZ_ROOT "D:/SDK/ozz-animation-master" CACHE PATH "ozz-animation source root")
if(NOT EXISTS "${OZZ_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "ozz-animation source not found at: ${OZZ_ROOT}")
endif()
set(ozz_build_tools OFF CACHE BOOL "" FORCE)
set(ozz_build_fbx OFF CACHE BOOL "" FORCE)
set(ozz_build_gltf OFF CACHE BOOL "" FORCE)
set(ozz_build_data OFF CACHE BOOL "" FORCE)
set(ozz_build_samples OFF CACHE BOOL "" FORCE)
set(ozz_build_howtos OFF CACHE BOOL "" FORCE)
set(ozz_build_tests OFF CACHE BOOL "" FORCE)
set(ozz_build_postfix OFF CACHE BOOL "" FORCE)
add_subdirectory(${OZZ_ROOT} ozz_build)
set(CLIENT_OZZ_TARGETS ozz_animation ozz_base)

set(NOESIS_SDK "D:/SDK/NoesisGUI-3.2.13" CACHE PATH "NoesisGUI SDK root")
set(NOESIS_VKRENDERDEVICE "${NOESIS_SDK}/Src/Packages/Render/VKRenderDevice")
set(NOESIS_PROVIDERS "${NOESIS_SDK}/Src/Packages/App/Providers")
set(NOESIS_APPLICATIONLAUNCHER "${NOESIS_SDK}/Src/Packages/App/ApplicationLauncher")

if(WIN32)
    find_package(Vulkan REQUIRED)
    find_program(DXC_EXECUTABLE
        NAMES dxc.exe dxc
        HINTS
            "$ENV{VULKAN_SDK}/Bin"
            "C:/VulkanSDK/1.4.350.0/Bin"
            "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64"
    )
    if(NOT DXC_EXECUTABLE)
        message(FATAL_ERROR "DXC was not found. Install the Vulkan SDK or Windows SDK dxc.exe for HLSL-to-SPIR-V compilation.")
    endif()

    set(GENERATED_SHADER_DIR "${CLIENT_ROOT}/assets/shaders")
    set(CUBE_VS_SPV "${GENERATED_SHADER_DIR}/cube_vs.spv")
    set(CUBE_PS_SPV "${GENERATED_SHADER_DIR}/cube_ps.spv")
    set(WARRIOR_VS_SPV "${GENERATED_SHADER_DIR}/warrior_vs.spv")
    set(WARRIOR_PS_SPV "${GENERATED_SHADER_DIR}/warrior_ps.spv")
    set(WARRIOR_CS_SPV "${GENERATED_SHADER_DIR}/warrior_cs.spv")
    set(TERRAIN_VS_SPV "${GENERATED_SHADER_DIR}/terrain_vs.spv")
    set(TERRAIN_PS_SPV "${GENERATED_SHADER_DIR}/terrain_ps.spv")
    set(WATER_VS_SPV "${GENERATED_SHADER_DIR}/water_vs.spv")
    set(WATER_PS_SPV "${GENERATED_SHADER_DIR}/water_ps.spv")
    set(COMPOSITE_VS_SPV "${GENERATED_SHADER_DIR}/composite_vs.spv")
    set(COMPOSITE_PS_SPV "${GENERATED_SHADER_DIR}/composite_ps.spv")
    set(SHADOW_DEPTH_VS_SPV "${GENERATED_SHADER_DIR}/shadow_depth_vs.spv")
    set(NAMEPLATE_VS_SPV "${GENERATED_SHADER_DIR}/nameplate_vs.spv")
    set(NAMEPLATE_PS_SPV "${GENERATED_SHADER_DIR}/nameplate_ps.spv")

    add_custom_command(
        OUTPUT "${CUBE_VS_SPV}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_SHADER_DIR}"
        COMMAND "${DXC_EXECUTABLE}" -spirv -T vs_6_0 -E VSMain
            -Fo "${CUBE_VS_SPV}" "${CLIENT_ROOT}/shaders/Cube.hlsl"
        DEPENDS "${CLIENT_ROOT}/shaders/Cube.hlsl"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${CUBE_PS_SPV}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_SHADER_DIR}"
        COMMAND "${DXC_EXECUTABLE}" -spirv -T ps_6_0 -E PSMain
            -Fo "${CUBE_PS_SPV}" "${CLIENT_ROOT}/shaders/Cube.hlsl"
        DEPENDS "${CLIENT_ROOT}/shaders/Cube.hlsl"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${WARRIOR_VS_SPV}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_SHADER_DIR}"
        COMMAND "${DXC_EXECUTABLE}" -spirv -T vs_6_0 -E VSMain
            -Fo "${WARRIOR_VS_SPV}" "${CLIENT_ROOT}/shaders/Warrior.hlsl"
        DEPENDS "${CLIENT_ROOT}/shaders/Warrior.hlsl"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${WARRIOR_PS_SPV}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_SHADER_DIR}"
        COMMAND "${DXC_EXECUTABLE}" -spirv -T ps_6_0 -E PSMain
            -Fo "${WARRIOR_PS_SPV}" "${CLIENT_ROOT}/shaders/Warrior.hlsl"
        DEPENDS "${CLIENT_ROOT}/shaders/Warrior.hlsl"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${WARRIOR_CS_SPV}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_SHADER_DIR}"
        COMMAND "${DXC_EXECUTABLE}" -spirv -T cs_6_0 -E CSMain
            -Fo "${WARRIOR_CS_SPV}" "${CLIENT_ROOT}/shaders/WarriorSkin.hlsl"
        DEPENDS "${CLIENT_ROOT}/shaders/WarriorSkin.hlsl"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${TERRAIN_VS_SPV}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_SHADER_DIR}"
        COMMAND "${DXC_EXECUTABLE}" -spirv -fvk-use-dx-layout -T vs_6_0 -E VSMain
            -Fo "${TERRAIN_VS_SPV}" "${CLIENT_ROOT}/shaders/Terrain.hlsl"
        DEPENDS "${CLIENT_ROOT}/shaders/Terrain.hlsl"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${TERRAIN_PS_SPV}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_SHADER_DIR}"
        COMMAND "${DXC_EXECUTABLE}" -spirv -fvk-use-dx-layout -T ps_6_0 -E PSMain
            -Fo "${TERRAIN_PS_SPV}" "${CLIENT_ROOT}/shaders/Terrain.hlsl"
        DEPENDS "${CLIENT_ROOT}/shaders/Terrain.hlsl"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${WATER_VS_SPV}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_SHADER_DIR}"
        COMMAND "${DXC_EXECUTABLE}" -spirv -fvk-use-dx-layout -T vs_6_0 -E VSMain
            -Fo "${WATER_VS_SPV}" "${CLIENT_ROOT}/shaders/Water.hlsl"
        DEPENDS "${CLIENT_ROOT}/shaders/Water.hlsl"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${WATER_PS_SPV}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_SHADER_DIR}"
        COMMAND "${DXC_EXECUTABLE}" -spirv -fvk-use-dx-layout -T ps_6_0 -E PSMain
            -Fo "${WATER_PS_SPV}" "${CLIENT_ROOT}/shaders/Water.hlsl"
        DEPENDS "${CLIENT_ROOT}/shaders/Water.hlsl"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${COMPOSITE_VS_SPV}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_SHADER_DIR}"
        COMMAND "${DXC_EXECUTABLE}" -spirv -T vs_6_0 -E VSMain
            -Fo "${COMPOSITE_VS_SPV}" "${CLIENT_ROOT}/shaders/Composite.hlsl"
        DEPENDS "${CLIENT_ROOT}/shaders/Composite.hlsl"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${COMPOSITE_PS_SPV}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_SHADER_DIR}"
        COMMAND "${DXC_EXECUTABLE}" -spirv -T ps_6_0 -E PSMain
            -Fo "${COMPOSITE_PS_SPV}" "${CLIENT_ROOT}/shaders/Composite.hlsl"
        DEPENDS "${CLIENT_ROOT}/shaders/Composite.hlsl"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${SHADOW_DEPTH_VS_SPV}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_SHADER_DIR}"
        COMMAND "${DXC_EXECUTABLE}" -spirv -fvk-use-dx-layout -T vs_6_0 -E VSMain
            -Fo "${SHADOW_DEPTH_VS_SPV}" "${CLIENT_ROOT}/shaders/ShadowDepth.hlsl"
        DEPENDS "${CLIENT_ROOT}/shaders/ShadowDepth.hlsl"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${NAMEPLATE_VS_SPV}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_SHADER_DIR}"
        COMMAND "${DXC_EXECUTABLE}" -spirv -fvk-use-dx-layout -T vs_6_0 -E VSMain
            -Fo "${NAMEPLATE_VS_SPV}" "${CLIENT_ROOT}/shaders/Nameplate.hlsl"
        DEPENDS "${CLIENT_ROOT}/shaders/Nameplate.hlsl"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${NAMEPLATE_PS_SPV}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_SHADER_DIR}"
        COMMAND "${DXC_EXECUTABLE}" -spirv -fvk-use-dx-layout -T ps_6_0 -E PSMain
            -Fo "${NAMEPLATE_PS_SPV}" "${CLIENT_ROOT}/shaders/Nameplate.hlsl"
        DEPENDS "${CLIENT_ROOT}/shaders/Nameplate.hlsl"
        VERBATIM
    )
    add_custom_target(ClientShaders DEPENDS
        "${CUBE_VS_SPV}"
        "${CUBE_PS_SPV}"
        "${WARRIOR_VS_SPV}"
        "${WARRIOR_PS_SPV}"
        "${WARRIOR_CS_SPV}"
        "${TERRAIN_VS_SPV}"
        "${TERRAIN_PS_SPV}"
        "${WATER_VS_SPV}"
        "${WATER_PS_SPV}"
        "${COMPOSITE_VS_SPV}"
        "${COMPOSITE_PS_SPV}"
        "${SHADOW_DEPTH_VS_SPV}"
        "${NAMEPLATE_VS_SPV}"
        "${NAMEPLATE_PS_SPV}"
    )
elseif(ANDROID)
    if(CMAKE_ANDROID_NDK)
        set(_ANDROID_NDK_ROOT "${CMAKE_ANDROID_NDK}")
    elseif(DEFINED ENV{ANDROID_NDK_HOME})
        set(_ANDROID_NDK_ROOT "$ENV{ANDROID_NDK_HOME}")
    else()
        message(FATAL_ERROR "Android NDK path is not available. Set ANDROID_NDK_HOME.")
    endif()

    set(NOESIS_ANDROID_LIB "${NOESIS_SDK}/Bin/android_arm64/libNoesis.so")
    if(NOT EXISTS "${NOESIS_ANDROID_LIB}")
        message(FATAL_ERROR "NoesisGUI Android library not found at: ${NOESIS_ANDROID_LIB}")
    endif()

    add_library(Noesis_imported SHARED IMPORTED)
    set_target_properties(Noesis_imported PROPERTIES
        IMPORTED_LOCATION "${NOESIS_ANDROID_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${NOESIS_SDK}/Include"
    )
endif()

add_subdirectory(libs/platform)
add_subdirectory(libs/asset)
add_subdirectory(libs/network)
add_subdirectory(libs/render)
add_subdirectory(apps/client)
add_subdirectory(apps/selftest)
add_subdirectory(apps/water_body_test_gen)
