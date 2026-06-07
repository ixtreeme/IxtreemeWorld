#pragma once

#include "NativeWindow.h"

#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

class VulkanDevice
{
public:
    bool Create(NativeWindow& window, uint32_t width, uint32_t height);
    void BeginFrame();
    void BeginSwapchainRenderPass();
    void EndFrame();
    bool Resize(uint32_t width, uint32_t height);
    void WaitIdle();
    void Destroy();

    bool IsFrameActive() const { return m_frameStarted && !m_skipFrame; }
    VkInstance GetInstance() const { return m_instance; }
    VkPhysicalDevice GetPhysicalDevice() const { return m_physicalDevice; }
    VkDevice GetDevice() const { return m_device; }
    VkQueue GetGraphicsQueue() const { return m_graphicsQueue; }
    uint32_t GetGraphicsQueueFamily() const { return m_queueFamilies.graphics; }
    VkRenderPass GetRenderPass() const { return m_renderPass; }
    VkCommandBuffer GetCommandBuffer() const { return m_commandBuffers[m_currentFrame]; }
    uint32_t GetFrameIndex() const { return m_currentFrame; }
    VkExtent2D GetSwapchainExtent() const { return m_swapchainExtent; }
    VkFormat GetSwapchainFormat() const { return m_swapchainFormat; }
    VkFormat GetDepthStencilFormat() const { return m_depthStencilFormat; }
    VkSurfaceTransformFlagBitsKHR GetSurfaceTransform() const { return m_currentTransform; }
    uint32_t GetSwapchainImageCount() const { return static_cast<uint32_t>(m_swapchainImages.size()); }
    uint64_t GetFrameNumber() const { return m_frameNumber; }
    uint64_t GetSafeFrameNumber() const { return m_safeFrameNumber; }
    bool SupportsSamplerAnisotropy() const { return m_samplerAnisotropySupported; }
    float GetMaxSamplerAnisotropy() const { return m_maxSamplerAnisotropy; }
    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }
    bool IsSwapchainFormatSrgb() const;
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;

private:
    static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;

    struct QueueFamilies
    {
        uint32_t graphics = UINT32_MAX;
        uint32_t present = UINT32_MAX;
        bool Complete() const { return graphics != UINT32_MAX && present != UINT32_MAX; }
    };

    struct SwapchainSupport
    {
        VkSurfaceCapabilitiesKHR capabilities{};
        std::vector<VkSurfaceFormatKHR> formats;
        std::vector<VkPresentModeKHR> presentModes;
    };

    bool CreateInstance(NativeWindow& window);
    bool CreateDebugMessenger();
    bool CreateSurface(NativeWindow& window);
    bool PickPhysicalDevice();
    bool CreateLogicalDevice();
    bool CreateSwapchainObjects(uint32_t width, uint32_t height);
    bool CreateSwapchain(uint32_t width, uint32_t height);
    bool CreateImageViews();
    bool CreateDepthStencilImages();
    bool CreateRenderPass();
    bool CreateFramebuffers();
    bool CreateCommandPool();
    bool CreateCommandBuffers();
    bool CreateSyncObjects();

    void DestroySwapchainObjects();
    bool RecreateSwapchain(uint32_t width, uint32_t height);

    QueueFamilies FindQueueFamilies(VkPhysicalDevice device) const;
    SwapchainSupport QuerySwapchainSupport(VkPhysicalDevice device) const;
    bool IsDeviceSuitable(VkPhysicalDevice device, QueueFamilies* outFamilies) const;
    bool CheckDeviceExtensionSupport(VkPhysicalDevice device) const;
    VkSurfaceFormatKHR ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const;
    VkExtent2D ChooseExtent(const VkSurfaceCapabilitiesKHR& caps, uint32_t width, uint32_t height) const;
    VkFormat FindDepthStencilFormat() const;

    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    VkQueue m_presentQueue = VK_NULL_HANDLE;
    QueueFamilies m_queueFamilies{};

    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkFormat m_swapchainFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D m_swapchainExtent{};
    VkSurfaceTransformFlagBitsKHR m_currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    std::vector<VkImage> m_swapchainImages;
    std::vector<VkImageView> m_swapchainImageViews;
    std::vector<VkFramebuffer> m_framebuffers;
    VkFormat m_depthStencilFormat = VK_FORMAT_UNDEFINED;
    std::vector<VkImage> m_depthStencilImages;
    std::vector<VkDeviceMemory> m_depthStencilMemory;
    std::vector<VkImageView> m_depthStencilImageViews;

    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> m_commandBuffers;

    VkSemaphore m_imageAvailable[MAX_FRAMES_IN_FLIGHT]{};
    std::vector<VkSemaphore> m_renderFinished;
    VkFence m_inFlightFences[MAX_FRAMES_IN_FLIGHT]{};
    std::vector<VkFence> m_imagesInFlight;

    uint32_t m_currentFrame = 0;
    uint32_t m_imageIndex = 0;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint64_t m_frameNumber = 0;
    uint64_t m_safeFrameNumber = 0;
    bool m_frameStarted = false;
    bool m_skipFrame = false;
    bool m_renderPassStarted = false;
    bool m_swapchainDirty = false;
    bool m_validationEnabled = false;
    bool m_samplerAnisotropySupported = false;
    float m_maxSamplerAnisotropy = 1.0f;
};
