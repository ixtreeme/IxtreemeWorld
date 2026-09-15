# Engine/Graphics — IXRHI contract + Vulkan backend (Phases 2-3C).
#
# Dependency (strict, one direction):
#   IXEngineIXRHI   — pure contract, knows no Vulkan (INTERFACE, header-only).
#   IXEngineVulkan  — backend OWNING the frame contract (acquire/record/submit/
#                     present/sync/timestamps/swapchain object) + legacy device
#                     infrastructure use. Renderers depend on IXRHI only.
# Forbidden: IXRHI -> Vulkan (headers verified by IXRHISmoke TU).

add_library(IXEngineIXRHI INTERFACE)

target_include_directories(IXEngineIXRHI
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/IXRHI
)

target_compile_features(IXEngineIXRHI INTERFACE cxx_std_20)

add_library(IXEngineVulkan STATIC
    Vulkan/IXVulkanConversions.cpp
    Vulkan/IXVulkanConversions.h
    Vulkan/IXVulkanDevice.cpp
    Vulkan/IXVulkanDevice.h
    Vulkan/IXVulkanResources.cpp
    Vulkan/IXVulkanResources.h
    Vulkan/IXVulkanBinding.cpp
    Vulkan/IXVulkanBinding.h
    Vulkan/IXVulkanPipeline.cpp
    Vulkan/IXVulkanPipeline.h
    Vulkan/IXVulkanRenderPass.cpp
    Vulkan/IXVulkanRenderPass.h
    Vulkan/IXVulkanRenderTarget.cpp
    Vulkan/IXVulkanRenderTarget.h
    Vulkan/IXVulkanCommandList.cpp
    Vulkan/IXVulkanCommandList.h
    Vulkan/IXVulkanFrame.cpp
    Vulkan/IXVulkanFrameTracker.h
    Vulkan/IXVulkanSwapchain.cpp
    Vulkan/IXVulkanSwapchain.h
    Vulkan/IXVulkanSync.h
    Vulkan/IXVulkanBridge.cpp
    Vulkan/IXVulkanBridge.h
    Vulkan/IXVulkan.h
)

target_include_directories(IXEngineVulkan
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/IXRHI
        ${CMAKE_CURRENT_SOURCE_DIR}/Vulkan
        ${CLIENT_LIBS_DIR}
)

target_link_libraries(IXEngineVulkan
    PUBLIC
        IXEngineIXRHI
    PRIVATE
        IXEnginePlatform
)

if(WIN32)
    target_link_libraries(IXEngineVulkan PUBLIC Vulkan::Vulkan)
elseif(ANDROID)
    target_link_libraries(IXEngineVulkan PUBLIC vulkan log android)
endif()

target_compile_features(IXEngineVulkan PUBLIC cxx_std_20)

# Backend timestamp capture honors the same verbose-diagnostics gate as the
# legacy loop did (pool created only with IXTREEME_DEBUG_LOGS).
target_compile_definitions(IXEngineVulkan
    PUBLIC
        $<$<BOOL:${IXTREEME_VERBOSE_DIAGNOSTICS}>:IXTREEME_DEBUG_LOGS=1>
)

# Editor Vulkan adapter (ImGui backend integration). Editor-only: needs imgui
# bindings, exactly like the editor panels in IXEngineRender.
if(IXTREEME_WITH_EDITOR)
    target_sources(IXEngineVulkan PRIVATE
        Vulkan/IXVulkanEditorAdapter.cpp
        Vulkan/IXVulkanEditorAdapter.h
    )
    target_link_libraries(IXEngineVulkan
        PUBLIC
            IXEngineEditorGraphics
        PRIVATE
            imgui::imgui
    )
endif()

if(BUILD_TESTING)
    add_subdirectory(Tests)
endif()
