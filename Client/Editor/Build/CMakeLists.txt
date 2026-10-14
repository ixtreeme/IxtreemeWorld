# Editor/Build — build-pipeline service boundary.
# UI (EditorImGui + editor_panels/*.inl) triggers builds; this lib implements them.
# Depends only on IXEnginePlatform (platform::RunProcess). Must NOT depend on
# IXEngineRender / EditorImGui / ProjectManager / SceneManager / Vulkan renderers.
add_library(IXEngineEditorBuild STATIC
    BuildService.cpp
    BuildService.h
    GamePackager.cpp
    GamePackager.h
)

target_include_directories(IXEngineEditorBuild
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CLIENT_LIBS_DIR}
)

target_link_libraries(IXEngineEditorBuild
    PUBLIC
        IXEnginePlatform
)

target_compile_features(IXEngineEditorBuild PUBLIC cxx_std_20)
