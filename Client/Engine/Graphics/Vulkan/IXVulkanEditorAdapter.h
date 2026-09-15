#pragma once

// IXVulkanEditorAdapter — backend-specific editor integration (Phase 3B, §12).
//
// Owns everything ImGui's Vulkan backend requires: descriptor pool, Win32 +
// Vulkan backend init/shutdown, backend NewFrame/DrawFrame, and UI texture
// registration (scene/game slots + asset-preview pool) against the generic
// EditorGraphicsBridge. The ONLY place allowed to call ImGui_ImplVulkan_*
// texture/backend functions and to resolve native views/samplers from IXRHI
// resources for UI display.
//
// EditorImGui + panels consume the Vk-free IEditorTextureProvider interface
// only. Main-thread only (ImGui constraint). Dies with the frame-loop
// migration (Phase 3C), when UI presentation moves behind IXRHI.

#include "EditorGraphicsBridge.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#if defined(_WIN32)
#include <windows.h>
#endif

class VulkanDevice;

namespace ixrhi
{
class IXRHIDevice;
}

namespace ixvulkan
{

class IXVulkanEditorAdapter final : public ixeditor::graphics::IEditorTextureProvider
{
public:
    static std::unique_ptr<IXVulkanEditorAdapter> Create(VulkanDevice& loop,
                                                         ixrhi::IXRHIDevice& rhi);

    ~IXVulkanEditorAdapter() override;

    IXVulkanEditorAdapter(const IXVulkanEditorAdapter&) = delete;
    IXVulkanEditorAdapter& operator=(const IXVulkanEditorAdapter&) = delete;

    // Backend lifecycle (called by the frame owner, not editor UI).
    // windowHandle is HWND on Windows, null elsewhere. Idempotent shutdown.
    bool CreateBackend(void* windowHandle);
    void ShutdownBackend();
    void OnRenderPassChanged();
    void DrawFrame(VkCommandBuffer cmd, std::uint64_t frameNumber);
#if defined(_WIN32)
    // Forwards to the Win32 backend's message handler (backend ready only).
    bool HandleWin32Message(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result);
#endif

    // ---- IEditorTextureProvider (Vk-free) ----
    bool IsReady() const override;
    void BeginBackendFrame() override;
    bool IsBackendFrameActive() const override { return m_frameActive; }
    void SetSceneViewTexture(std::shared_ptr<ixrhi::IXRHITexture> texture,
                             std::shared_ptr<ixrhi::IXRHISampler> sampler,
                             std::uint32_t width,
                             std::uint32_t height) override;
    void SetGameViewTexture(std::shared_ptr<ixrhi::IXRHITexture> texture,
                            std::shared_ptr<ixrhi::IXRHISampler> sampler,
                            std::uint32_t width,
                            std::uint32_t height) override;
    void* GetSceneViewTexture() override;
    void GetSceneViewSize(std::uint32_t& width, std::uint32_t& height) override;
    void* GetGameViewTexture() override;
    void GetGameViewSize(std::uint32_t& width, std::uint32_t& height) override;
    ixeditor::graphics::EditorTextureHandle UploadPreviewTexture(const void* rgba8,
                                                                  std::uint32_t width,
                                                                  std::uint32_t height,
                                                                  const char* tag) override;
    void ReleasePreviewTexture(ixeditor::graphics::EditorTextureHandle handle) override;
    void ReleaseAllPreviewTextures() override;
    void* GetPreviewTexture(ixeditor::graphics::EditorTextureHandle handle) override;

    std::size_t RegisteredTextureCount() const;

private:
    IXVulkanEditorAdapter(VulkanDevice& loop, ixrhi::IXRHIDevice& rhi);

    struct ViewSlot
    {
        std::shared_ptr<ixrhi::IXRHITexture> texture;
        std::shared_ptr<ixrhi::IXRHISampler> sampler;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        ixeditor::graphics::EditorTextureHandle handle;
        void* uiId = nullptr; // backend UI descriptor (ImTextureID), untyped here
    };

    struct PreviewEntry
    {
        ixeditor::graphics::EditorTextureHandle handle;
        std::shared_ptr<ixrhi::IXRHITexture> texture;
        std::shared_ptr<ixrhi::IXRHISampler> sampler;
        void* uiId = nullptr;
    };

    bool CreateDescriptorPool();
    void SetViewSlot(ViewSlot& slot,
                     std::shared_ptr<ixrhi::IXRHITexture> texture,
                     std::shared_ptr<ixrhi::IXRHISampler> sampler,
                     std::uint32_t width,
                     std::uint32_t height,
                     const char* logTag);
    void ClearViewSlot(ViewSlot& slot);
    void* RegisterUiTexture(ixrhi::IXRHITexture& texture,
                            ixrhi::IXRHISampler& sampler,
                            const char* tag,
                            ixeditor::graphics::EditorTextureHandle& outHandle);
    void UnregisterUiTexture(void*& uiId,
                             ixeditor::graphics::EditorTextureHandle& handle);

    VulkanDevice* m_loop = nullptr; // borrowed frame-loop owner (E2)
    ixrhi::IXRHIDevice* m_rhi = nullptr; // borrowed IXRHI backend
    ixeditor::graphics::EditorGraphicsBridge m_bridge;

    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    bool m_backendReady = false;
    bool m_frameActive = false;

    ViewSlot m_sceneView;
    ViewSlot m_gameView;
    std::unordered_map<std::uint64_t, PreviewEntry> m_previews;

    std::uint64_t m_lastLoggedFrame = 0;
};

} // namespace ixvulkan
