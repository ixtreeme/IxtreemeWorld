add_library(client_render STATIC
    AssetLibrary.cpp
    AssetLibrary.h
    CubeRenderer.cpp
    CubeRenderer.h
    EditorImGui.cpp
    EditorImGui.h
    MapEditorTypes.h
    WorldLabelRenderer.cpp
    WorldLabelRenderer.h
    OffscreenSceneRenderer.cpp
    OffscreenSceneRenderer.h
    ProjectManager.cpp
    ProjectManager.h
    RmlUiLayer.cpp
    RmlUiLayer.h
    RuntimeSession.cpp
    RuntimeSession.h
    RuntimeUiAdapter.cpp
    RuntimeUiAdapter.h
    SceneManager.cpp
    SceneManager.h
    SpatialIndex.cpp
    SpatialIndex.h
    TerrainRenderer.cpp
    TerrainRenderer.h
    IconsFontAwesome6.h
    UIHelpers.h
    WaterBodyIO.cpp
    WaterBodyIO.h
    StaticMeshRenderer.cpp
    StaticMeshRenderer.h
    SkinnedMeshRenderer.cpp
    SkinnedMeshRenderer.h
    WorldCamera.h
    WorldComponents.h
)

if(IXTREEME_WITH_EDITOR)
    target_sources(client_render PRIVATE UIHelpers.cpp)
endif()

target_include_directories(client_render
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CLIENT_LIBS_DIR}
        ${STB_INCLUDE_DIR}
)

target_compile_definitions(client_render
    PUBLIC
        $<$<BOOL:${IXTREEME_WITH_EDITOR}>:IXTREEME_WITH_EDITOR=1>
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
        RmlUi::RmlUi
        ${CLIENT_OZZ_TARGETS}
        ${CLIENT_FLECS_TARGET}
)

if(WIN32)
    target_link_libraries(client_render
        PUBLIC
            Vulkan::Vulkan
            Gdi32
            Advapi32
    )
    if(IXTREEME_WITH_EDITOR)
        target_link_libraries(client_render
            PUBLIC
                imgui::imgui
                imguizmo::imguizmo
                Comdlg32
        )
    endif()
elseif(ANDROID)
    target_link_libraries(client_render
        PUBLIC
            vulkan
            log
            android
    )
endif()

target_compile_features(client_render PUBLIC cxx_std_20)
