# Standalone Vulkan clear/present sample.
#
# Build from this directory:
#   cmake -S . -B build -A x64 -DNOESIS_SDK=E:/SDK/NoesisGUI-3.2.13
#   cmake --build build --config Debug
#   .\build\Debug\VulkanClear.exe

cmake_minimum_required(VERSION 3.20)
project(VulkanClearStandalone LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This sample is Windows-only and uses the raw Win32 API.")
endif()

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

set(GENERATED_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders")
set(CUBE_VS_SPV "${GENERATED_SHADER_DIR}/cube_vs.spv")
set(CUBE_PS_SPV "${GENERATED_SHADER_DIR}/cube_ps.spv")
set(WARRIOR_VS_SPV "${GENERATED_SHADER_DIR}/warrior_vs.spv")
set(WARRIOR_PS_SPV "${GENERATED_SHADER_DIR}/warrior_ps.spv")
set(WARRIOR_CS_SPV "${GENERATED_SHADER_DIR}/warrior_cs.spv")
set(TERRAIN_VS_SPV "${GENERATED_SHADER_DIR}/terrain_vs.spv")
set(TERRAIN_PS_SPV "${GENERATED_SHADER_DIR}/terrain_ps.spv")
set(NAMEPLATE_VS_SPV "${GENERATED_SHADER_DIR}/nameplate_vs.spv")
set(NAMEPLATE_PS_SPV "${GENERATED_SHADER_DIR}/nameplate_ps.spv")
add_custom_command(
    OUTPUT "${CUBE_VS_SPV}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_SHADER_DIR}"
    COMMAND "${DXC_EXECUTABLE}" -spirv -T vs_6_0 -E VSMain
        -Fo "${CUBE_VS_SPV}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Cube.hlsl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Cube.hlsl"
    VERBATIM
)
add_custom_command(
    OUTPUT "${CUBE_PS_SPV}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_SHADER_DIR}"
    COMMAND "${DXC_EXECUTABLE}" -spirv -T ps_6_0 -E PSMain
        -Fo "${CUBE_PS_SPV}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Cube.hlsl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Cube.hlsl"
    VERBATIM
)
add_custom_command(
    OUTPUT "${WARRIOR_VS_SPV}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_SHADER_DIR}"
    COMMAND "${DXC_EXECUTABLE}" -spirv -T vs_6_0 -E VSMain
        -Fo "${WARRIOR_VS_SPV}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Warrior.hlsl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Warrior.hlsl"
    VERBATIM
)
add_custom_command(
    OUTPUT "${WARRIOR_PS_SPV}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_SHADER_DIR}"
    COMMAND "${DXC_EXECUTABLE}" -spirv -T ps_6_0 -E PSMain
        -Fo "${WARRIOR_PS_SPV}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Warrior.hlsl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Warrior.hlsl"
    VERBATIM
)
add_custom_command(
    OUTPUT "${WARRIOR_CS_SPV}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_SHADER_DIR}"
    COMMAND "${DXC_EXECUTABLE}" -spirv -T cs_6_0 -E CSMain
        -Fo "${WARRIOR_CS_SPV}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/WarriorSkin.hlsl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/WarriorSkin.hlsl"
    VERBATIM
)
add_custom_command(
    OUTPUT "${TERRAIN_VS_SPV}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_SHADER_DIR}"
    COMMAND "${DXC_EXECUTABLE}" -spirv -fvk-use-dx-layout -T vs_6_0 -E VSMain
        -Fo "${TERRAIN_VS_SPV}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Terrain.hlsl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Terrain.hlsl"
    VERBATIM
)
add_custom_command(
    OUTPUT "${TERRAIN_PS_SPV}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_SHADER_DIR}"
    COMMAND "${DXC_EXECUTABLE}" -spirv -fvk-use-dx-layout -T ps_6_0 -E PSMain
        -Fo "${TERRAIN_PS_SPV}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Terrain.hlsl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Terrain.hlsl"
    VERBATIM
)
add_custom_command(
    OUTPUT "${NAMEPLATE_VS_SPV}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_SHADER_DIR}"
    COMMAND "${DXC_EXECUTABLE}" -spirv -fvk-use-dx-layout -T vs_6_0 -E VSMain
        -Fo "${NAMEPLATE_VS_SPV}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Nameplate.hlsl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Nameplate.hlsl"
    VERBATIM
)
add_custom_command(
    OUTPUT "${NAMEPLATE_PS_SPV}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED_SHADER_DIR}"
    COMMAND "${DXC_EXECUTABLE}" -spirv -fvk-use-dx-layout -T ps_6_0 -E PSMain
        -Fo "${NAMEPLATE_PS_SPV}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Nameplate.hlsl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Nameplate.hlsl"
    VERBATIM
)
add_custom_target(CubeShaders DEPENDS
    "${CUBE_VS_SPV}"
    "${CUBE_PS_SPV}"
    "${WARRIOR_VS_SPV}"
    "${WARRIOR_PS_SPV}"
    "${WARRIOR_CS_SPV}"
    "${TERRAIN_VS_SPV}"
    "${TERRAIN_PS_SPV}"
    "${NAMEPLATE_VS_SPV}"
    "${NAMEPLATE_PS_SPV}"
)

if(DEFINED ENV{VCPKG_ROOT} AND EXISTS "$ENV{VCPKG_ROOT}/installed/x64-windows-static")
    list(APPEND CMAKE_PREFIX_PATH "$ENV{VCPKG_ROOT}/installed/x64-windows-static")
endif()
find_package(Boost CONFIG REQUIRED COMPONENTS system)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../shared/protocol shared_protocol_build)

set(NOESIS_SDK "D:/SDK/NoesisGUI-3.2.13" CACHE PATH "NoesisGUI SDK root")
set(NOESIS_VKRENDERDEVICE "${NOESIS_SDK}/Src/Packages/Render/VKRenderDevice")
set(NOESIS_PROVIDERS "${NOESIS_SDK}/Src/Packages/App/Providers")
set(NOESIS_APPLICATIONLAUNCHER "${NOESIS_SDK}/Src/Packages/App/ApplicationLauncher")
set(GRANNY_SDK_SOURCE "D:/SDK/Granny-3D-SDK-main/source" CACHE PATH "Granny SDK source root")
set(GRANNY_STATIC_LIB "${GRANNY_SDK_SOURCE}/build-mt/Debug/granny2_static.lib" CACHE FILEPATH "Granny static library")

add_executable(VulkanClear
    main.cpp
    InputEvent.h
    Debug.cpp
    Debug.h
    CubeRenderer.cpp
    CubeRenderer.h
    GrannyModel.cpp
    GrannyModel.h
    WarriorRenderer.cpp
    WarriorRenderer.h
    TerrainRenderer.cpp
    TerrainRenderer.h
    NameplateRenderer.cpp
    NameplateRenderer.h
    WorldCamera.h
    NativeWindow.cpp
    NativeWindow.h
    NoesisLayer.cpp
    NoesisLayer.h
    VulkanDevice.cpp
    VulkanDevice.h
    network/CharacterListItem.h
    network/IClientHandler.h
    network/LegacyBridge.h
    network/ClientSession.h
    network/ClientSession.cpp
    "${NOESIS_APPLICATIONLAUNCHER}/Src/DelegateCommand.cpp"
    "${NOESIS_APPLICATIONLAUNCHER}/Src/NotifyPropertyChangedBase.cpp"
    "${NOESIS_PROVIDERS}/Src/FastLZ.cpp"
    "${NOESIS_VKRENDERDEVICE}/Src/Render.VKRenderDevice.cpp"
    "${NOESIS_VKRENDERDEVICE}/Src/VKRenderDevice.cpp"
)
add_dependencies(VulkanClear CubeShaders)

target_compile_features(VulkanClear PRIVATE cxx_std_20)
target_compile_definitions(VulkanClear PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0601
    NS_APP_PROVIDERS_API=
    NS_APP_APPLICATIONLAUNCHER_API=
    NS_RENDER_VKRENDERDEVICE_API=
    BUILDING_GRANNY_STATIC=1
)
target_include_directories(VulkanClear PRIVATE
    "${NOESIS_SDK}/Include"
    "${NOESIS_PROVIDERS}/Include"
    "${NOESIS_VKRENDERDEVICE}/Include"
    "${NOESIS_SDK}/Src/Packages/App/ApplicationLauncher/Include"
    "${GRANNY_SDK_SOURCE}/winxx/win64"
    "${GRANNY_SDK_SOURCE}/winxx"
    "${GRANNY_SDK_SOURCE}"
)
target_link_libraries(VulkanClear PRIVATE
    Vulkan::Vulkan
    Boost::system
    protocol
    "${GRANNY_STATIC_LIB}"
    "${NOESIS_SDK}/Lib/windows_x86_64/Noesis.lib"
    Ws2_32
    Gdi32
    Advapi32
)

# Debug keeps a console for stderr logs. Release uses the Windows subsystem.
set_target_properties(VulkanClear PROPERTIES
    WIN32_EXECUTABLE "$<NOT:$<CONFIG:Debug>>"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

add_custom_command(TARGET VulkanClear POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${NOESIS_SDK}/Bin/windows_x86_64/Noesis.dll"
        "$<TARGET_FILE_DIR:VulkanClear>/Noesis.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/xaml"
        "$<TARGET_FILE_DIR:VulkanClear>/xaml"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/fonts"
        "$<TARGET_FILE_DIR:VulkanClear>/fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Character"
        "$<TARGET_FILE_DIR:VulkanClear>/Character"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Maps"
        "$<TARGET_FILE_DIR:VulkanClear>/Maps"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/textureset"
        "$<TARGET_FILE_DIR:VulkanClear>/textureset"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/ymi work"
        "$<TARGET_FILE_DIR:VulkanClear>/ymi work"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${GENERATED_SHADER_DIR}"
        "$<TARGET_FILE_DIR:VulkanClear>/shaders"
)
