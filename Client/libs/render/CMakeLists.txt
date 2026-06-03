add_library(client_render STATIC
    AssetLibrary.cpp
    AssetLibrary.h
    CubeRenderer.cpp
    CubeRenderer.h
    MapEditorTypes.h
    NameplateRenderer.cpp
    NameplateRenderer.h
    NoesisLayer.cpp
    NoesisLayer.h
    OffscreenSceneRenderer.cpp
    OffscreenSceneRenderer.h
    TerrainRenderer.cpp
    TerrainRenderer.h
    WarriorRenderer.cpp
    WarriorRenderer.h
    WorldCamera.h
    WorldComponents.h
    "${NOESIS_APPLICATIONLAUNCHER}/Src/DelegateCommand.cpp"
    "${NOESIS_APPLICATIONLAUNCHER}/Src/NotifyPropertyChangedBase.cpp"
    "${NOESIS_PROVIDERS}/Src/FastLZ.cpp"
    "${NOESIS_VKRENDERDEVICE}/Src/Render.VKRenderDevice.cpp"
    "${NOESIS_VKRENDERDEVICE}/Src/VKRenderDevice.cpp"
)

target_include_directories(client_render
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CLIENT_LIBS_DIR}
        ${STB_INCLUDE_DIR}
        "${NOESIS_SDK}/Include"
        "${NOESIS_PROVIDERS}/Include"
        "${NOESIS_VKRENDERDEVICE}/Include"
        "${NOESIS_SDK}/Src/Packages/App/ApplicationLauncher/Include"
)

target_compile_definitions(client_render
    PUBLIC
        NS_APP_PROVIDERS_API=
        NS_APP_APPLICATIONLAUNCHER_API=
        NS_RENDER_VKRENDERDEVICE_API=
        $<$<PLATFORM_ID:Windows>:WIN32_LEAN_AND_MEAN>
        $<$<PLATFORM_ID:Windows>:NOMINMAX>
        $<$<PLATFORM_ID:Windows>:_WIN32_WINNT=0x0601>
)

target_link_libraries(client_render
    PUBLIC
        client_platform
        client_asset
        mapdata
        fastgltf::fastgltf
        ${CLIENT_OZZ_TARGETS}
        ${CLIENT_FLECS_TARGET}
)

if(WIN32)
    target_link_libraries(client_render
        PUBLIC
            Vulkan::Vulkan
            "${NOESIS_SDK}/Lib/windows_x86_64/Noesis.lib"
            Gdi32
            Advapi32
            Comdlg32
    )
elseif(ANDROID)
    target_link_libraries(client_render
        PUBLIC
            Noesis_imported
            vulkan
            log
            android
    )
endif()

target_compile_features(client_render PUBLIC cxx_std_20)
