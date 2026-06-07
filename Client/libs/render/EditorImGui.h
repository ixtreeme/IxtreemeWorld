#pragma once

#include "InputEvent.h"

#if defined(_WIN32) && !defined(VK_USE_PLATFORM_WIN32_KHR)
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <vulkan/vulkan.h>

#include <cstdint>

#if defined(_WIN32)
#include <windows.h>
#endif

class VulkanDevice;

class EditorImGui
{
public:
    EditorImGui() = default;
    ~EditorImGui();

#if defined(_WIN32)
    bool Create(VulkanDevice& device, HWND hwnd);
    bool HandleWin32Message(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result);
#else
    bool Create(VulkanDevice& device, void* windowHandle);
#endif

    void BeginFrame(bool editorModeActive);
    void Render(VulkanDevice& device);
    void OnRenderPassChanged(VulkanDevice& device);
    bool WantsInputCapture(const InputEvent& event) const;
    void Destroy();

private:
    bool CreateDescriptorPool(VulkanDevice& device);
    bool InitVulkanBackend(VulkanDevice& device);
    void RenderDemoPanels();

    VkDevice m_device = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    bool m_initialized = false;
    bool m_vulkanBackendReady = false;
    bool m_frameActive = false;
    bool m_editorModeActive = false;
    bool m_showDemoWindow = true;
    uint64_t m_lastLoggedFrame = UINT64_MAX;
};
