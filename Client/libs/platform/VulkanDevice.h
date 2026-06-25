#pragma once

#include "NativeWindow.h"

#include <vulkan/vulkan.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <vector>

class VulkanDevice
{
public:
    enum class GpuTimestampPoint : uint32_t
    {
        FrameBegin = 0,
        ShadowPassBegin,
        ShadowCascade0Begin,
        ShadowCascade0End,
        ShadowCascade1Begin,
        ShadowCascade1End,
        ShadowCascade2Begin,
        ShadowCascade2End,
        ShadowCascade3Begin,
        ShadowCascade3End,
        ShadowPassEnd,
        WaterReflectionBegin,
        WaterReflectionEnd,
        TerrainMainBegin,
        TerrainMainEnd,
        SceneOtherBegin,
        SceneOtherEnd,
        CompositeBegin,
        CompositeEnd,
        RmlUiBegin,
        RmlUiEnd,
        ImGuiBegin,
        ImGuiEnd,
        FrameEnd,
        Count
    };

    static constexpr uint32_t GpuTimestampPointCount = static_cast<uint32_t>(GpuTimestampPoint::Count);

    struct GpuTimestampResults
    {
        bool valid = false;
        uint64_t frameNumber = 0;
        std::array<bool, GpuTimestampPointCount> pointValid{};
        std::array<double, GpuTimestampPointCount> pointMs{};
    };

    struct CpuFrameTimingResults
    {
        bool valid = false;
        uint64_t frameNumber = 0;
        double acquireImageMs = 0.0;
        double waitForFencesMs = 0.0;
        double renderLoopCpuWorkMs = 0.0;
        double submitMs = 0.0;
        double presentMs = 0.0;
        double totalCpuFrameMs = 0.0;
    };

    bool Create(NativeWindow& window, uint32_t width, uint32_t height);
    void BeginFrame();
    void BeginSwapchainRenderPass(const char* passName = "other");
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
    // False when the swapchain fell back to FIFO (vsync) — i.e. FPS is capped to the
    // monitor refresh. True for IMMEDIATE/MAILBOX (uncapped).
    bool IsPresentUncapped() const { return m_swapchainPresentMode != VK_PRESENT_MODE_FIFO_KHR; }
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
    static constexpr uint32_t MaxFramesInFlight() { return MAX_FRAMES_IN_FLIGHT; }
    void RequestGpuFrameCapture();
    bool IsGpuFrameCaptureActive() const { return m_gpuCaptureActive; }
    void WriteGpuTimestamp(GpuTimestampPoint point);
    bool ConsumeGpuFrameCaptureResults(GpuTimestampResults& gpu, CpuFrameTimingResults& cpu);

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
    bool CreateTimestampQueryPool();
    void BeginGpuFrameCaptureCommands();
    void FinishGpuFrameCaptureAfterSubmit();

    void DestroySwapchainObjects();
    bool RecreateSwapchain(uint32_t width, uint32_t height);
    int FindSwapchainImageIndex(VkImage image) const;
    void LogSwapchainImageTransition(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout, const char* passName) const;

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
    VkPresentModeKHR m_swapchainPresentMode = VK_PRESENT_MODE_FIFO_KHR;
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
    VkQueryPool m_timestampQueryPool = VK_NULL_HANDLE;
    float m_timestampPeriodNs = 0.0f;

    VkSemaphore m_imageAvailable[MAX_FRAMES_IN_FLIGHT]{};
    std::vector<VkSemaphore> m_renderFinished;
    VkFence m_inFlightFences[MAX_FRAMES_IN_FLIGHT]{};
    std::vector<VkFence> m_imagesInFlight;

    uint32_t m_currentFrame = 0;
    uint32_t m_imageIndex = 0;
    int m_lastAcquiredImageIndex = -1;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint64_t m_frameNumber = 0;
    uint64_t m_safeFrameNumber = 0;
    bool m_frameStarted = false;
    bool m_skipFrame = false;
    bool m_renderPassStarted = false;
    bool m_swapchainDirty = false;
    bool m_acquiredThisFrame = false;
    bool m_swapchainTransitionThisFrame = false;
    const char* m_activeSwapchainPass = "none";
    bool m_validationEnabled = false;
    bool m_samplerAnisotropySupported = false;
    float m_maxSamplerAnisotropy = 1.0f;
    bool m_gpuCaptureRequested = false;
    bool m_gpuCaptureActive = false;
    bool m_gpuCaptureResultsReady = false;
    std::array<bool, GpuTimestampPointCount> m_gpuCapturePointWritten{};
    GpuTimestampResults m_lastGpuCaptureResults{};
    CpuFrameTimingResults m_lastCpuFrameTimingResults{};
    CpuFrameTimingResults m_activeCpuFrameTiming{};
    std::chrono::steady_clock::time_point m_cpuFrameStartTime{};
    std::chrono::steady_clock::time_point m_cpuRenderWorkStartTime{};
};
