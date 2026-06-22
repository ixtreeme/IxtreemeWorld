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

project(IxtreemeEngine
    VERSION 0.1.0
    DESCRIPTION "IxtreemeEngeni - Vulkan"
    LANGUAGES C CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(IXTREEME_WITH_EDITOR "Build ImGui-based editor functionality" ON)
option(IXTREEME_VERBOSE_DIAGNOSTICS "Enable verbose per-frame engine diagnostics" OFF)
option(IXENGINE_ENABLE_AVX2_FMA "Build AVX2/FMA optimized engine code paths and prefer them at runtime with SSE2 fallback." ON)
option(IXENGINE_COMPILE_WHOLE_ENGINE_AVX2_FMA "Compile all engine targets with AVX2/FMA instructions. Requires an AVX2/FMA CPU and disables true binary-level SSE2 fallback." OFF)
mark_as_advanced(IXENGINE_COMPILE_WHOLE_ENGINE_AVX2_FMA)

if(IXTREEME_WITH_EDITOR)
    message(STATUS "IxtreemeEngeni editor build: ENABLED")
else()
    message(STATUS "IxtreemeEngeni editor build: DISABLED")
endif()

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
    foreach(_lang C CXX)
        string(REPLACE " /RTC1" "" CMAKE_${_lang}_FLAGS_DEBUG "${CMAKE_${_lang}_FLAGS_DEBUG}")
        string(REPLACE "/RTC1" "" CMAKE_${_lang}_FLAGS_DEBUG "${CMAKE_${_lang}_FLAGS_DEBUG}")
    endforeach()
    add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8 /MP /EHsc)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

set(IXENGINE_X86_TARGET OFF)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64|X86|x86|i[3-6]86")
    set(IXENGINE_X86_TARGET ON)
endif()

function(ixengine_apply_cpu_defaults target_name)
    if(NOT TARGET ${target_name})
        return()
    endif()

    if(IXENGINE_ENABLE_AVX2_FMA AND IXENGINE_X86_TARGET)
        target_compile_definitions(${target_name} PRIVATE IXENGINE_ENABLE_AVX2_FMA=1)
    endif()

    if(IXENGINE_COMPILE_WHOLE_ENGINE_AVX2_FMA AND IXENGINE_X86_TARGET)
        target_compile_definitions(${target_name} PRIVATE IXENGINE_WHOLE_ENGINE_AVX2_FMA=1)
        if(MSVC)
            target_compile_options(${target_name} PRIVATE /arch:AVX2)
        else()
            target_compile_options(${target_name} PRIVATE -mavx2 -mfma)
        endif()
    endif()
endfunction()

include(cmake/BootstrapVcpkg.cmake)
include(FetchContent)

vcpkg_require(
    capnproto
    efsw
    assimp
    fastgltf
    flecs
	rmlui
	zlib
    stb
)

if(IXTREEME_WITH_EDITOR)
    vcpkg_require(imguizmo)
endif()

if(WIN32 AND IXTREEME_WITH_EDITOR)
    find_package(imgui CONFIG QUIET)
    if(NOT imgui_FOUND)
        execute_process(
            COMMAND "${_vcpkg_exe}" install "imgui[docking-experimental,vulkan-binding,win32-binding]:${VCPKG_TARGET_TRIPLET}"
            RESULT_VARIABLE result
        )
        if(result)
            message(FATAL_ERROR "vcpkg: Nem sikerult telepiteni: imgui[docking-experimental,vulkan-binding,win32-binding]")
        endif()
        if(EXISTS "${_vcpkg_installed_prefix}")
            list(PREPEND CMAKE_PREFIX_PATH "${_vcpkg_installed_prefix}")
            list(PREPEND CMAKE_LIBRARY_PATH
                "${_vcpkg_installed_prefix}/lib"
                "${_vcpkg_installed_prefix}/debug/lib"
            )
            list(PREPEND CMAKE_INCLUDE_PATH "${_vcpkg_installed_prefix}/include")
        endif()
        find_package(imgui CONFIG REQUIRED)
    endif()
endif()

set(CLIENT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(CLIENT_LIBS_DIR "${CLIENT_ROOT}/libs")

add_subdirectory(libs/math)
add_subdirectory(${CLIENT_LIBS_DIR}/ixtreemetree)
add_subdirectory(libs/common)
add_subdirectory(libs/debug)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../shared/map shared_map_build)

find_package(fastgltf CONFIG REQUIRED)
find_package(assimp CONFIG REQUIRED)
find_package(efsw CONFIG REQUIRED)
FetchContent_Declare(
    meshoptimizer
    GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
    GIT_TAG v0.23
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(meshoptimizer)
if(TARGET meshoptimizer AND NOT TARGET meshoptimizer::meshoptimizer)
    add_library(meshoptimizer::meshoptimizer ALIAS meshoptimizer)
endif()
if(TARGET meshoptimizer)
    if(MSVC)
        set_property(TARGET meshoptimizer PROPERTY MSVC_RUNTIME_CHECKS "")
        set_target_properties(meshoptimizer PROPERTIES VS_GLOBAL_BasicRuntimeChecks "Default")
        target_compile_options(meshoptimizer PRIVATE $<$<CONFIG:Debug>:/O2;/Ob2>)
    else()
        target_compile_options(meshoptimizer PRIVATE $<$<CONFIG:Debug>:-O2>)
    endif()
else()
    message(FATAL_ERROR "FetchContent meshoptimizer target was not created")
endif()
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
find_package(RmlUi 6.2 CONFIG REQUIRED)
if(IXTREEME_WITH_EDITOR)
    find_package(imguizmo CONFIG REQUIRED)
endif()
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
set(CLIENT_OZZ_TARGETS ozz_animation ozz_animation_offline ozz_base)

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
    set(SKINNED_MESH_VS_SPV "${GENERATED_SHADER_DIR}/skinned_mesh_vs.spv")
    set(SKINNED_MESH_PS_SPV "${GENERATED_SHADER_DIR}/skinned_mesh_ps.spv")
    set(SKINNED_MESH_CS_SPV "${GENERATED_SHADER_DIR}/skinned_mesh_cs.spv")
    set(STATIC_MESH_VS_SPV "${GENERATED_SHADER_DIR}/static_mesh_vs.spv")
    set(STATIC_MESH_PS_SPV "${GENERATED_SHADER_DIR}/static_mesh_ps.spv")
    set(STATIC_MESH_UNLIT_PS_SPV "${GENERATED_SHADER_DIR}/static_mesh_unlit_ps.spv")
    set(STATIC_MESH_OUTLINE_VS_SPV "${GENERATED_SHADER_DIR}/static_mesh_outline_vs.spv")
    set(STATIC_MESH_OUTLINE_PS_SPV "${GENERATED_SHADER_DIR}/static_mesh_outline_ps.spv")
    set(TERRAIN_VS_SPV "${GENERATED_SHADER_DIR}/terrain_vs.spv")
    set(TERRAIN_PS_SPV "${GENERATED_SHADER_DIR}/terrain_ps.spv")
    set(WATER_VS_SPV "${GENERATED_SHADER_DIR}/water_vs.spv")
    set(WATER_PS_SPV "${GENERATED_SHADER_DIR}/water_ps.spv")
    set(COMPOSITE_VS_SPV "${GENERATED_SHADER_DIR}/composite_vs.spv")
    set(COMPOSITE_PS_SPV "${GENERATED_SHADER_DIR}/composite_ps.spv")
    set(SHADOW_DEPTH_VS_SPV "${GENERATED_SHADER_DIR}/shadow_depth_vs.spv")
    set(SELECTION_OUTLINE_VS_SPV "${GENERATED_SHADER_DIR}/selection_outline_vs.spv")
    set(SELECTION_OUTLINE_PS_SPV "${GENERATED_SHADER_DIR}/selection_outline_ps.spv")
    set(WORLD_LABEL_VS_SPV "${GENERATED_SHADER_DIR}/world_label_vs.spv")
    set(WORLD_LABEL_PS_SPV "${GENERATED_SHADER_DIR}/world_label_ps.spv")
    set(RMLUI_VS_SPV "${GENERATED_SHADER_DIR}/rmlui_vs.spv")
    set(RMLUI_PS_SPV "${GENERATED_SHADER_DIR}/rmlui_ps.spv")

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
        OUTPUT "${SKINNED_MESH_VS_SPV}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_SHADER_DIR}"
        COMMAND "${DXC_EXECUTABLE}" -spirv -T vs_6_0 -E VSMain
            -Fo "${SKINNED_MESH_VS_SPV}" "${CLIENT_ROOT}/shaders/SkinnedMesh.hlsl"
        DEPENDS "${CLIENT_ROOT}/shaders/SkinnedMesh.hlsl"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${SKINNED_MESH_PS_SPV}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_SHADER_DIR}"
        COMMAND "${DXC_EXECUTABLE}" -spirv -T ps_6_0 -E PSMain
            -Fo "${SKINNED_MESH_PS_SPV}" "${CLIENT_ROOT}/shaders/SkinnedMesh.hlsl"
        DEPENDS "${CLIENT_ROOT}/shaders/SkinnedMesh.hlsl"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${SKINNED_MESH_CS_SPV}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_SHADER_DIR}"
        COMMAND "${DXC_EXECUTABLE}" -spirv -T cs_6_0 -E CSMain
            -Fo "${SKINNED_MESH_CS_SPV}" "${CLIENT_ROOT}/shaders/SkinnedMeshSkin.hlsl"
        DEPENDS "${CLIENT_ROOT}/shaders/SkinnedMeshSkin.hlsl"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${STATIC_MESH_VS_SPV}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_SHADER_DIR}"
        COMMAND "${DXC_EXECUTABLE}" -spirv -T vs_6_0 -E VSMain
            -Fo "${STATIC_MESH_VS_SPV}" "${CLIENT_ROOT}/shaders/StaticMesh.hlsl"
        DEPENDS "${CLIENT_ROOT}/shaders/StaticMesh.hlsl"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${STATIC_MESH_PS_SPV}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_SHADER_DIR}"
        COMMAND "${DXC_EXECUTABLE}" -spirv -T ps_6_0 -E PSMain
            -Fo "${STATIC_MESH_PS_SPV}" "${CLIENT_ROOT}/shaders/StaticMesh.hlsl"
        DEPENDS "${CLIENT_ROOT}/shaders/StaticMesh.hlsl"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${STATIC_MESH_UNLIT_PS_SPV}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_SHADER_DIR}"
        COMMAND "${DXC_EXECUTABLE}" -spirv -T ps_6_0 -E PSMain -D SHADING_MODE_UNLIT=1
            -Fo "${STATIC_MESH_UNLIT_PS_SPV}" "${CLIENT_ROOT}/shaders/StaticMesh.hlsl"
        DEPENDS "${CLIENT_ROOT}/shaders/StaticMesh.hlsl"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${STATIC_MESH_OUTLINE_VS_SPV}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_SHADER_DIR}"
        COMMAND "${DXC_EXECUTABLE}" -spirv -T vs_6_0 -E VSMain -D STATIC_MESH_OUTLINE=1
            -Fo "${STATIC_MESH_OUTLINE_VS_SPV}" "${CLIENT_ROOT}/shaders/StaticMesh.hlsl"
        DEPENDS "${CLIENT_ROOT}/shaders/StaticMesh.hlsl"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${STATIC_MESH_OUTLINE_PS_SPV}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_SHADER_DIR}"
        COMMAND "${DXC_EXECUTABLE}" -spirv -T ps_6_0 -E PSMain -D STATIC_MESH_OUTLINE=1
            -Fo "${STATIC_MESH_OUTLINE_PS_SPV}" "${CLIENT_ROOT}/shaders/StaticMesh.hlsl"
        DEPENDS "${CLIENT_ROOT}/shaders/StaticMesh.hlsl"
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
        OUTPUT "${SELECTION_OUTLINE_VS_SPV}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_SHADER_DIR}"
        COMMAND "${DXC_EXECUTABLE}" -spirv -fvk-use-dx-layout -T vs_6_0 -E VSMain
            -Fo "${SELECTION_OUTLINE_VS_SPV}" "${CLIENT_ROOT}/shaders/SelectionOutline.hlsl"
        DEPENDS "${CLIENT_ROOT}/shaders/SelectionOutline.hlsl"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${SELECTION_OUTLINE_PS_SPV}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_SHADER_DIR}"
        COMMAND "${DXC_EXECUTABLE}" -spirv -fvk-use-dx-layout -T ps_6_0 -E PSMain
            -Fo "${SELECTION_OUTLINE_PS_SPV}" "${CLIENT_ROOT}/shaders/SelectionOutline.hlsl"
        DEPENDS "${CLIENT_ROOT}/shaders/SelectionOutline.hlsl"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${WORLD_LABEL_VS_SPV}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_SHADER_DIR}"
        COMMAND "${DXC_EXECUTABLE}" -spirv -fvk-use-dx-layout -T vs_6_0 -E VSMain
            -Fo "${WORLD_LABEL_VS_SPV}" "${CLIENT_ROOT}/shaders/WorldLabel.hlsl"
        DEPENDS "${CLIENT_ROOT}/shaders/WorldLabel.hlsl"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${WORLD_LABEL_PS_SPV}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_SHADER_DIR}"
        COMMAND "${DXC_EXECUTABLE}" -spirv -fvk-use-dx-layout -T ps_6_0 -E PSMain
            -Fo "${WORLD_LABEL_PS_SPV}" "${CLIENT_ROOT}/shaders/WorldLabel.hlsl"
        DEPENDS "${CLIENT_ROOT}/shaders/WorldLabel.hlsl"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RMLUI_VS_SPV}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_SHADER_DIR}"
        COMMAND "${DXC_EXECUTABLE}" -spirv -fvk-use-dx-layout -T vs_6_0 -E VSMain
            -Fo "${RMLUI_VS_SPV}" "${CLIENT_ROOT}/shaders/RmlUi.hlsl"
        DEPENDS "${CLIENT_ROOT}/shaders/RmlUi.hlsl"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RMLUI_PS_SPV}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_SHADER_DIR}"
        COMMAND "${DXC_EXECUTABLE}" -spirv -fvk-use-dx-layout -T ps_6_0 -E PSMain
            -Fo "${RMLUI_PS_SPV}" "${CLIENT_ROOT}/shaders/RmlUi.hlsl"
        DEPENDS "${CLIENT_ROOT}/shaders/RmlUi.hlsl"
        VERBATIM
    )
    add_custom_target(IXEngineShaders DEPENDS
        "${CUBE_VS_SPV}"
        "${CUBE_PS_SPV}"
        "${SKINNED_MESH_VS_SPV}"
        "${SKINNED_MESH_PS_SPV}"
        "${SKINNED_MESH_CS_SPV}"
        "${STATIC_MESH_VS_SPV}"
        "${STATIC_MESH_PS_SPV}"
        "${STATIC_MESH_UNLIT_PS_SPV}"
        "${STATIC_MESH_OUTLINE_VS_SPV}"
        "${STATIC_MESH_OUTLINE_PS_SPV}"
        "${TERRAIN_VS_SPV}"
        "${TERRAIN_PS_SPV}"
        "${WATER_VS_SPV}"
        "${WATER_PS_SPV}"
        "${COMPOSITE_VS_SPV}"
        "${COMPOSITE_PS_SPV}"
        "${SHADOW_DEPTH_VS_SPV}"
        "${SELECTION_OUTLINE_VS_SPV}"
        "${SELECTION_OUTLINE_PS_SPV}"
        "${WORLD_LABEL_VS_SPV}"
        "${WORLD_LABEL_PS_SPV}"
        "${RMLUI_VS_SPV}"
        "${RMLUI_PS_SPV}"
    )
elseif(ANDROID)
    if(CMAKE_ANDROID_NDK)
        set(_ANDROID_NDK_ROOT "${CMAKE_ANDROID_NDK}")
    elseif(DEFINED ENV{ANDROID_NDK_HOME})
        set(_ANDROID_NDK_ROOT "$ENV{ANDROID_NDK_HOME}")
    else()
        message(FATAL_ERROR "Android NDK path is not available. Set ANDROID_NDK_HOME.")
    endif()

endif()

add_subdirectory(libs/platform)
add_subdirectory(libs/asset)
add_subdirectory(libs/import_export)
add_subdirectory(libs/render)
add_subdirectory(libs/prefab)
add_subdirectory(libs/mesh_system)
add_subdirectory(libs/terrain_editor_system)
add_subdirectory(libs/selection_system)
add_subdirectory(apps/client)

foreach(_ixengine_target IN ITEMS
    IXEngineMath
    IXEngineTree
    IXEngineCommon
    IXEngineDebug
    IXEngineMapData
    meshoptimizer
    IXEnginePlatform
    IXEngineAsset
    IXEngineImportExport
    IXEngineRender
    IXEnginePrefab
    IXEngineMeshSystem
    IXEngineTerrainEditorSystem
    IXEngineSelectionSystem
    IxtreemeEngine
)
    ixengine_apply_cpu_defaults(${_ixengine_target})
endforeach()
