add_library(IXEngineRender STATIC
    EditorImGui.cpp
    EditorImGui.h
    editor_panels/EditorImGuiAnimatorPanels.inl
    editor_panels/EditorImGuiAssetBrowserPanels.inl
    editor_panels/EditorImGuiHierarchyPanels.inl
    editor_panels/EditorImGuiInspectorPanels.inl
    editor_panels/EditorImGuiMaterialPanels.inl
    editor_panels/EditorImGuiMenuToolbarPanels.inl
    editor_panels/EditorImGuiPanelDispatcher.inl
    editor_panels/EditorImGuiProjectPanels.inl
    editor_panels/EditorImGuiSceneViewPanels.inl
    MapEditorTypes.h
    WorldLabelRenderer.cpp
    WorldLabelRenderer.h
    ParticleRenderer.cpp
    ParticleRenderer.h
    OffscreenSceneRenderer.cpp
    OffscreenSceneRenderer.h
    ViewImpostorRenderer.cpp
    ViewImpostorRenderer.h
    RmlUiLayer.cpp
    RmlUiLayer.h
    RuntimeSession.cpp
    RuntimeSession.h
    RuntimeUiAdapter.cpp
    RuntimeUiAdapter.h
    SceneManager.cpp
    SceneManager.h
    SceneLayerAuthoring.cpp
    SceneLayerAuthoring.h
    SceneLayerGround.cpp
    SceneLayerGround.h
    SceneWorldPackage.cpp
    SceneWorldPackage.h
    SelectionOutlineRenderer.cpp
    SelectionOutlineRenderer.h
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
    SkyRenderer.cpp
    SkyRenderer.h
    GodRayRenderer.cpp
    GodRayRenderer.h
    tools/tree/TreeGeneratorPanel.h
    tools/tree/TreePreviewRenderer.h
    tools/tree/TreeTexturePalette.cpp
    tools/tree/TreeTexturePalette.h
    WorldCamera.h
    WorldComponents.h
)

# Editor-only translation units. These pull in ImGui/ImGuizmo (only fetched when the editor is on),
# so they must NOT be compiled into a runtime (IXTREEME_WITH_EDITOR=OFF) build. EditorImGui.cpp stays
# in the unconditional list because it self-guards (full body under the editor flag, link stubs in the
# #else branch) so the always-compiled engine can reference EditorImGui symbols in both builds.
if(IXTREEME_WITH_EDITOR)
    target_sources(IXEngineRender PRIVATE
        UIHelpers.cpp
        tools/tree/TreeGeneratorPanel.cpp
        tools/tree/TreePreviewRenderer.cpp
    )
endif()

target_include_directories(IXEngineRender
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CLIENT_LIBS_DIR}
        ${STB_INCLUDE_DIR}
)

target_compile_definitions(IXEngineRender
    PUBLIC
        $<$<BOOL:${IXTREEME_WITH_EDITOR}>:IXTREEME_WITH_EDITOR=1>
        $<$<BOOL:${IXTREEME_VERBOSE_DIAGNOSTICS}>:IXTREEME_DEBUG_LOGS=1>
        $<$<PLATFORM_ID:Windows>:WIN32_LEAN_AND_MEAN>
        $<$<PLATFORM_ID:Windows>:NOMINMAX>
        $<$<PLATFORM_ID:Windows>:_WIN32_WINNT=0x0601>
        IXTREEME_TREE_PRESET_DIR="${CLIENT_ROOT}/assets/internal/tree_presets"
    PRIVATE
        IXTREEME_MESHOPT_FETCHCONTENT=1
        IXTREEME_MESHOPT_OPTIMIZED_DEBUG=1
)

target_link_libraries(IXEngineRender
    PUBLIC
        IXEnginePlatform
        IXEngineAsset
        IXEngineCommon
        IXEngineJobs
        IXEngineMath
        IXEnginePhysics
        IXEngineAudio
        IXEngineScript
        IXEngineParticle
        IXEngineImportExport
        IXEngineMapData
        fastgltf::fastgltf
        assimp::assimp
        IXEngineTree::IXEngineTree
        meshoptimizer::meshoptimizer
        RmlUi::RmlUi
        IXEngineIXRHI
        IXEngineEditorGraphics
        ${CLIENT_OZZ_TARGETS}
        ${CLIENT_FLECS_TARGET}
)

# NOTE: no Vulkan backend dependency here. Generic render code sees IXRHI
# only; Vulkan headers/libs reach this target transitively via
# IXEnginePlatform (VulkanDevice.h) where still needed. The app links
# IXEngineVulkan directly for the backend factory/adapter.
if(WIN32)
    target_link_libraries(IXEngineRender
        PUBLIC
            Gdi32
            Advapi32
    )
    if(IXTREEME_WITH_EDITOR)
        target_link_libraries(IXEngineRender
            PUBLIC
                imgui::imgui
                imguizmo::imguizmo
                Comdlg32
        )
    endif()
elseif(ANDROID)
    target_link_libraries(IXEngineRender
        PUBLIC
            log
            android
    )
endif()

target_compile_features(IXEngineRender PUBLIC cxx_std_20)

if(BUILD_TESTING AND NOT ANDROID)
    add_executable(ViewImpostorTest tests/ViewImpostorTest.cpp)
    target_link_libraries(ViewImpostorTest PRIVATE IXEngineRender)
    target_compile_features(ViewImpostorTest PRIVATE cxx_std_20)
    if(MSVC)
        set_target_properties(ViewImpostorTest PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME ViewImpostorTest COMMAND ViewImpostorTest)
    set_tests_properties(ViewImpostorTest PROPERTIES TIMEOUT 30)

    add_executable(TreeImpostorTest tests/TreeImpostorTest.cpp)
    target_link_libraries(TreeImpostorTest PRIVATE IXEngineRender)
    target_compile_features(TreeImpostorTest PRIVATE cxx_std_20)
    if(MSVC)
        set_target_properties(TreeImpostorTest PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME TreeImpostorTest COMMAND TreeImpostorTest)
    set_tests_properties(TreeImpostorTest PROPERTIES TIMEOUT 90)

    add_executable(FbxImportTest tests/FbxImportTest.cpp)
    target_link_libraries(FbxImportTest PRIVATE IXEngineRender)
    target_compile_features(FbxImportTest PRIVATE cxx_std_20)
    if(MSVC)
        set_target_properties(FbxImportTest PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME FbxImportTest COMMAND FbxImportTest)
    set_tests_properties(FbxImportTest PROPERTIES TIMEOUT 60)

    add_executable(ExrImageTest tests/ExrImageTest.cpp)
    target_link_libraries(ExrImageTest PRIVATE IXEngineRender)
    target_compile_features(ExrImageTest PRIVATE cxx_std_20)
    if(MSVC)
        set_target_properties(ExrImageTest PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME ExrImageTest COMMAND ExrImageTest)

    add_executable(SceneLayerGroundTest tests/SceneLayerGroundTest.cpp)
    target_link_libraries(SceneLayerGroundTest PRIVATE IXEngineRender)
    target_compile_features(SceneLayerGroundTest PRIVATE cxx_std_20)
    if(MSVC)
        set_target_properties(SceneLayerGroundTest PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME SceneLayerGroundTest COMMAND SceneLayerGroundTest)

    add_executable(SceneLayerAuthoringTest tests/SceneLayerAuthoringTest.cpp)
    target_link_libraries(SceneLayerAuthoringTest PRIVATE IXEngineRender)
    target_compile_features(SceneLayerAuthoringTest PRIVATE cxx_std_20)
    if(MSVC)
        set_target_properties(SceneLayerAuthoringTest PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME SceneLayerAuthoringTest COMMAND SceneLayerAuthoringTest)
    add_executable(SceneWorldPackageTest tests/SceneWorldPackageTest.cpp)
    target_link_libraries(SceneWorldPackageTest PRIVATE IXEngineRender)
    target_compile_features(SceneWorldPackageTest PRIVATE cxx_std_20)
    if(MSVC)
        set_target_properties(SceneWorldPackageTest PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME SceneWorldPackageTest COMMAND SceneWorldPackageTest)
endif()
