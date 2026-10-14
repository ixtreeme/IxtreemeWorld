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
    FogSettings.h
    FogSettings.cpp
    FogMath.h
    FogMath.cpp
    WorldLabelRenderer.cpp
    WorldLabelRenderer.h
    ParticleRenderer.cpp
    ParticleRenderer.h
    OffscreenSceneRenderer.cpp
    OffscreenSceneRenderer.h
    OcclusionCuller.cpp
    OcclusionCuller.h
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
    TerrainSplatPaint.cpp
    TerrainSplatPaint.h
    IconsFontAwesome6.h
    UIHelpers.h
    WaterBodyIO.cpp
    WaterBodyIO.h
    StaticMeshRenderer.cpp
    StaticMeshRenderer.h
    MeshSurfaceQuery.h
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
    # Both offline documents are embedded into the editor. The packaged runtime
    # does not compile this browser or require a documentation directory.
    set(EDITOR_HELP_ROOT "${CLIENT_ROOT}/../docs/editor-guide")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${EDITOR_HELP_ROOT}/hu.md" "${EDITOR_HELP_ROOT}/en.md")
    file(READ "${EDITOR_HELP_ROOT}/hu.md" EDITOR_HELP_HU)
    file(READ "${EDITOR_HELP_ROOT}/en.md" EDITOR_HELP_EN)
    configure_file(EditorHelpContent.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/EditorHelpContent.h" @ONLY)
    target_include_directories(IXEngineRender PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_sources(IXEngineRender PRIVATE
        EditorHelp.cpp
        EditorHelp.h
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
    add_executable(GodRayQualityTest tests/GodRayQualityTest.cpp)
    target_link_libraries(GodRayQualityTest PRIVATE IXEngineRender)
    if(WIN32)
        target_link_libraries(GodRayQualityTest PRIVATE IXEngineVulkan)
        target_compile_definitions(GodRayQualityTest PRIVATE IX_RAYS_TEST_ASSETS_ROOT="${CLIENT_ROOT}"
            IX_RAYS_TEST_SHADER_ROOT="${CMAKE_CURRENT_BINARY_DIR}")
        set(_ray_test_output "${CMAKE_CURRENT_BINARY_DIR}/god_ray_quality_test.spv")
        add_custom_command(OUTPUT "${_ray_test_output}"
            COMMAND "${DXC_EXECUTABLE}" -spirv -T cs_6_0 -E CSMain
                -Fo "${_ray_test_output}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/GodRayQualityTest.hlsl"
            DEPENDS tests/GodRayQualityTest.hlsl VERBATIM)
        target_sources(GodRayQualityTest PRIVATE "${_ray_test_output}")
        add_dependencies(GodRayQualityTest IXEngineShaders)
    endif()
    if(MSVC)
        set_target_properties(GodRayQualityTest PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME GodRayQualityTest COMMAND GodRayQualityTest)

    add_executable(WaterViewTest tests/WaterViewTest.cpp)
    target_link_libraries(WaterViewTest PRIVATE IXEngineRender)
    if(WIN32)
        target_link_libraries(WaterViewTest PRIVATE IXEngineVulkan)
        target_compile_definitions(WaterViewTest PRIVATE IX_WATER_TEST_ASSETS_ROOT="${CLIENT_ROOT}"
            IX_WATER_TEST_SHADER_ROOT="${CMAKE_CURRENT_BINARY_DIR}")
        foreach(_water_test_entry IN ITEMS VSMain PSMain CSMain)
            if(_water_test_entry STREQUAL "VSMain")
                set(_water_test_profile vs_6_0)
            elseif(_water_test_entry STREQUAL "PSMain")
                set(_water_test_profile ps_6_0)
            else()
                set(_water_test_profile cs_6_0)
            endif()
            set(_water_test_output "${CMAKE_CURRENT_BINARY_DIR}/water_test_${_water_test_entry}.spv")
            add_custom_command(OUTPUT "${_water_test_output}"
                COMMAND "${DXC_EXECUTABLE}" -spirv -T ${_water_test_profile} -E ${_water_test_entry}
                    -Fo "${_water_test_output}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/WaterViewTest.hlsl"
                DEPENDS tests/WaterViewTest.hlsl VERBATIM)
            target_sources(WaterViewTest PRIVATE "${_water_test_output}")
        endforeach()
        add_dependencies(WaterViewTest IXEngineShaders)
    endif()
    if(MSVC)
        set_target_properties(WaterViewTest PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME WaterViewTest COMMAND WaterViewTest)

    add_executable(FogSystemTest tests/FogSystemTest.cpp)
    target_link_libraries(FogSystemTest PRIVATE IXEngineRender)
    if(WIN32)
        target_link_libraries(FogSystemTest PRIVATE IXEngineVulkan)
        target_compile_definitions(FogSystemTest PRIVATE IX_FOG_TEST_ASSETS_ROOT="${CLIENT_ROOT}"
            IX_FOG_TEST_SHADER_ROOT="${CMAKE_CURRENT_BINARY_DIR}")
        foreach(_fog_test_entry IN ITEMS CSMain SampleCS)
            set(_fog_test_output "${CMAKE_CURRENT_BINARY_DIR}/fog_test_${_fog_test_entry}.spv")
            add_custom_command(OUTPUT "${_fog_test_output}"
                COMMAND "${DXC_EXECUTABLE}" -spirv -T cs_6_0 -E ${_fog_test_entry}
                    -I "${CLIENT_ROOT}/shaders" -Fo "${_fog_test_output}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/FogMathTest.hlsl"
                DEPENDS tests/FogMathTest.hlsl "${CLIENT_ROOT}/shaders/Fog.hlsli" VERBATIM)
            target_sources(FogSystemTest PRIVATE "${_fog_test_output}")
        endforeach()
        add_dependencies(FogSystemTest IXEngineShaders)
    endif()
    if(MSVC)
        set_target_properties(FogSystemTest PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME FogSystemTest COMMAND FogSystemTest)

    add_executable(ParticleTurbulenceTest tests/ParticleTurbulenceTest.cpp)
    target_link_libraries(ParticleTurbulenceTest PRIVATE IXEngineRender IXEngineParticle)
    target_compile_features(ParticleTurbulenceTest PRIVATE cxx_std_20)
    if(WIN32)
        target_link_libraries(ParticleTurbulenceTest PRIVATE IXEngineVulkan)
        target_compile_definitions(ParticleTurbulenceTest PRIVATE IX_PARTICLE_TEST_ASSETS_ROOT="${CLIENT_ROOT}")
    endif()
    if(MSVC)
        set_target_properties(ParticleTurbulenceTest PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME ParticleTurbulenceTest COMMAND ParticleTurbulenceTest)

    add_executable(OcclusionCullerTest tests/OcclusionCullerTest.cpp)
    target_link_libraries(OcclusionCullerTest PRIVATE IXEngineRender)
    if(WIN32)
        target_link_libraries(OcclusionCullerTest PRIVATE IXEngineVulkan)
        target_compile_definitions(OcclusionCullerTest PRIVATE IX_OCCLUSION_TEST_ASSETS_ROOT="${CLIENT_ROOT}")
    endif()
    if(MSVC)
        set_target_properties(OcclusionCullerTest PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME OcclusionCullerTest COMMAND OcclusionCullerTest)
    add_executable(StaticMeshLeafLodTest tests/StaticMeshLeafLodTest.cpp)
    target_link_libraries(StaticMeshLeafLodTest PRIVATE IXEngineRender)
    if(MSVC)
        set_target_properties(StaticMeshLeafLodTest PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME StaticMeshLeafLodTest COMMAND StaticMeshLeafLodTest)
    add_executable(StaticMeshLodGenerationTest tests/StaticMeshLodGenerationTest.cpp)
    target_link_libraries(StaticMeshLodGenerationTest PRIVATE IXEngineMeshSystem)
    if(WIN32)
        target_link_libraries(StaticMeshLodGenerationTest PRIVATE IXEngineVulkan)
        target_compile_definitions(StaticMeshLodGenerationTest PRIVATE IX_LOD_TEST_ASSETS_ROOT="${CLIENT_ROOT}")
    endif()
    if(MSVC)
        set_target_properties(StaticMeshLodGenerationTest PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME StaticMeshLodGenerationTest COMMAND StaticMeshLodGenerationTest)
    add_executable(HierarchyCacheTest tests/HierarchyCacheTest.cpp)
    target_link_libraries(HierarchyCacheTest PRIVATE IXEngineRender)
    if(MSVC)
        set_target_properties(HierarchyCacheTest PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME HierarchyCacheTest COMMAND HierarchyCacheTest)
    if(IXTREEME_WITH_EDITOR AND WIN32)
        add_executable(EditorHelpTest tests/EditorHelpTest.cpp)
        target_link_libraries(EditorHelpTest PRIVATE IXEngineRender)
        if(MSVC)
            set_target_properties(EditorHelpTest PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME EditorHelpTest COMMAND EditorHelpTest)
        add_executable(HierarchyUiTest tests/HierarchyUiTest.cpp)
        target_link_libraries(HierarchyUiTest PRIVATE IXEngineRender)
        if(MSVC)
            set_target_properties(HierarchyUiTest PROPERTIES
                MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME HierarchyUiTest COMMAND HierarchyUiTest)
    endif()
    add_executable(TerrainSplatPaintTest tests/TerrainSplatPaintTest.cpp)
    target_link_libraries(TerrainSplatPaintTest PRIVATE IXEngineRender)
    target_compile_features(TerrainSplatPaintTest PRIVATE cxx_std_20)
    if(MSVC)
        set_target_properties(TerrainSplatPaintTest PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME TerrainSplatPaintTest COMMAND TerrainSplatPaintTest)

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
