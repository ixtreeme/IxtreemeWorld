# Editor/Graphics — editor graphics bridge (Phase 3B, §10).
# Generic registry (Vk-free, ImGui-free). The Vulkan editor adapter lives in
# Engine/Graphics/Vulkan (IXVulkanEditorAdapter) and implements the provider
# interface against this registry. Editor code depends on the bridge, never on
# backend types.
add_library(IXEngineEditorGraphics STATIC
    EditorGraphicsBridge.cpp
    EditorGraphicsBridge.h
)

target_include_directories(IXEngineEditorGraphics
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(IXEngineEditorGraphics
    PUBLIC
        IXEngineIXRHI
)

target_compile_features(IXEngineEditorGraphics PUBLIC cxx_std_20)
