#pragma once

// IXVulkanDevice — Vulkan backend OWNING the IXRHI graphics frame contract.
//
// Phase 3C authority: frame acquisition, per-frame command ownership,
// submission, presentation, frames-in-flight sync, GPU timestamps and the main
// swapchain object live HERE. The legacy VulkanDevice keeps device/queue/
// swapchain-handle infrastructure (§39 debt) plus per-frame migration shims
// synced by this backend; its own BeginFrame/EndFrame loop is dormant.
//
// E2 Loop() is RETIRED as frame authority: Loop() remains only as infra
// access for the backend itself (device/queues/swapchain/images). Generic
// application/renderer code drives IXRHIDevice::BeginFrame/EndFrame and never
// touches the legacy loop.
//
// Vk* in THIS header is allowed: Engine/Graphics/Vulkan is the backend module.
// It must not leak further: renderer/editor headers take IXRHI types only.

#pragma once

// IXVulkanDevice — Vulkan backend owning the IXRHI graphics frame contract.
//
// Phase 3C authority: frame acquisition, per-frame command ownership,
// submission, presentation, frames-in-flight sync, GPU timestamps and the main
// swapchain object live HERE. The legacy VulkanDevice keeps device/queue/
// swapchain-handle infrastructure (§39 debt) plus migration shims synced by
// this backend every frame; its own BeginFrame/EndFrame loop is dormant.
//
// Vk* in THIS header is allowed: Engine/Graphics/Vulkan is the backend module.
// It must not leak further: renderer/editor headers take IXRHI types only.

#include "IXRHIDevice.h"
#include "IXVulkanFrameTracker.h"

#include <vulkan/vulkan.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>

class VulkanDevice;

namespace ixvulkan
{

class IXVulkanSwapchain;

class IXVulkanSwapchain;

// One preallocated frames-in-flight slot (§49: no per-frame allocation).
struct IXVulkanFrameSlot
{
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    std::unique_ptr<ixrhi::IXRHICommandList> list; // borrowed-mode wrapper
    VkFence fence = VK_NULL_HANDLE; // signaled when idle
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
};

class IXVulkanDevice final : public ixrhi::IXRHIDevice
{
public:
    explicit IXVulkanDevice(VulkanDevice& loop);
    ~IXVulkanDevice() override;

    IXVulkanDevice(const IXVulkanDevice&) = delete;
    IXVulkanDevice& operator=(const IXVulkanDevice&) = delete;

    // ---- ixrhi::IXRHIDevice (resources) ----
    std::shared_ptr<ixrhi::IXRHIBuffer> CreateBuffer(const ixrhi::IXRHIBufferDesc& desc,
                                                     const void* initialDataOrNull,
                                                     std::size_t initialBytes) override;
    std::unique_ptr<ixrhi::IXRHIBufferUpload> UploadBufferAsync(const ixrhi::IXRHIBufferDesc& desc,
                                                                const void* src,
                                                                std::size_t byteCount) override;
    bool IsTextureFormatSupported(ixrhi::IXRHIFormat format,
                                  ixrhi::IXRHITextureUsage usage) const override;
    std::shared_ptr<ixrhi::IXRHITexture> CreateTexture(const ixrhi::IXRHITextureDesc& desc,
                                                       const void* initialDataOrNull,
                                                       std::size_t initialBytes) override;
    std::shared_ptr<ixrhi::IXRHISampler> CreateSampler(const ixrhi::IXRHISamplerDesc& desc) override;
    std::shared_ptr<ixrhi::IXRHIShader> CreateShader(const ixrhi::IXRHIShaderDesc& desc) override;
    std::unique_ptr<ixrhi::IXRHIBindGroupLayout> CreateBindGroupLayout(
        const std::vector<ixrhi::IXRHIBinding>& bindings) override;
    std::unique_ptr<ixrhi::IXRHIBindGroup> CreateBindGroup(const ixrhi::IXRHIBindGroupLayout& layout,
                                                           std::uint32_t maxSets) override;
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> CreateGraphicsPipeline(
        const ixrhi::IXRHIGraphicsPipelineDesc& desc) override;
    std::unique_ptr<ixrhi::IXRHIComputePipeline> CreateComputePipeline(
        const ixrhi::IXRHIComputePipelineDesc& desc) override;
    std::unique_ptr<ixrhi::IXRHICommandList> CreateCommandList() override;
    std::unique_ptr<ixrhi::IXRHIFence> CreateFence(bool signaled) override;
    std::unique_ptr<ixrhi::IXRHISemaphore> CreateSemaphore() override;
    std::unique_ptr<ixrhi::IXRHIRenderTarget> CreateRenderTarget(
        const ixrhi::IXRHIRenderTargetDesc& desc) override;
    void WaitIdle() override;

    // ---- ixrhi::IXRHIDevice (frame lifecycle: backend authority) ----
    ixrhi::IXRHIFrame BeginFrame() override;
    void EndFrame(const ixrhi::IXRHIFrame& frame) override;
    bool RequestResize(std::uint32_t width, std::uint32_t height) override;
    std::uint32_t GetFramesInFlight() const override;
    std::uint64_t GetSwapchainGeneration() const override;
    ixrhi::IXRHISwapchain& GetMainSwapchain() override;
    ixrhi::IXRHIRenderTarget* GetMainRenderTarget() override;
    const ixrhi::IXRHIRenderPass* GetMainPass() const override;
    void WriteTimestamp(ixrhi::IXRHITimestampPoint point) override;
    void WriteTimestamp(std::uint32_t pointIndex) override;
    void RequestGpuFrameCapture() override;
    bool TryReadTimestamps(ixrhi::IXRHITimestampResults& gpu,
                           ixrhi::IXRHICpuFrameTiming& cpu) override;
    const ixrhi::IXRHICapabilities& GetCapabilities() const override { return m_capabilities; }

    // Releases ALL backend Vulkan objects (frame contexts, swapchain object,
    // query pool, upload pool). Idempotent; must precede legacy device
    // teardown (see shutdown order in the class comment above Shutdown's
    // implementation). The destructor calls it as a backstop.
    void Shutdown() override;

    // ---- backend-internal helpers (Vulkan module only) ----
    VulkanDevice& Loop() const { return *m_loop; }
    VkDevice NativeDevice() const;
    VkRenderPass ResolveRenderPass(const ixrhi::IXRHIRenderPass* pass) const;
    void SetDebugName(VkObjectType type, std::uint64_t handle, const char* name) const;
    void CheckVk(VkResult result, const char* call, const char* file, int line) const;

    // VMA insertion point (Phase 3): route these two through an allocator without
    // touching renderer or resource-class code.
    std::uint32_t FindMemoryType(std::uint32_t typeBits, VkMemoryPropertyFlags props) const;
    void AllocateAndBind(VkBuffer buffer, VkMemoryPropertyFlags props, VkDeviceMemory& out) const;
    void AllocateAndBind(VkImage image, VkMemoryPropertyFlags props, VkDeviceMemory& out) const;
    // Internally synchronized buffer copy (setup-time staging); not for use
    // inside a recording command list.
    void CopyBufferSync(VkBuffer src, VkBuffer dst, VkDeviceSize size) const;
    // Shared pass/framebuffer construction for render targets (offscreen +
    // swapchain main targets bake compatible passes from the same helper).
    // forPresent selects the main-window shape (color final PRESENT_SRC,
    // depth store DONT_CARE, legacy-compatible dependency) instead of the
    // sampled-target shape (color final SHADER_READ_ONLY).
    VkRenderPass CreateCompatRenderPass(ixrhi::IXRHIFormat colorFormat,
                                        ixrhi::IXRHIFormat depthFormat,
                                        ixrhi::IXRHILoadOp colorLoad,
                                        ixrhi::IXRHIStoreOp colorStore,
                                        ixrhi::IXRHILoadOp depthLoad,
                                        ixrhi::IXRHIStoreOp depthStore,
                                        const char* debugName,
                                        bool forPresent = false) const;
    VkFramebuffer CreateFramebufferFor(VkRenderPass pass,
                                       VkImageView colorView,
                                       VkImageView depthViewOrNull,
                                       std::uint32_t width,
                                       std::uint32_t height) const;

private:
    void QueryCapabilities();
    void UploadTextureBytes(VkImage image,
                            std::uint32_t width,
                            std::uint32_t height,
                            ixrhi::IXRHIFormat format,
                            const void* bytes,
                            std::size_t byteCount) const;
    // Frame authority internals.
    bool EnsureFrameSlot(std::uint32_t slot);
    bool EnsureSwapchainObjects();
    void TeardownFrameObjects();
    void SyncLegacyFrameState() const;
    void BeginCaptureForFrame();
    void FinishCaptureAfterSubmit();
    void CreateTimestampPool();
    // Synchronous backend-driven (re)build for the requested size. Returns
    // false (with dirty set, except for zero size) when nothing was built.
    bool RecreateSwapchainNow(std::uint32_t width, std::uint32_t height);

    VulkanDevice* m_loop = nullptr; // borrowed device/queue/swapchain infrastructure
    ixrhi::IXRHICapabilities m_capabilities;
    PFN_vkSetDebugUtilsObjectNameEXT m_setDebugName = nullptr;
    VkCommandPool m_uploadPool = VK_NULL_HANDLE; // transient uploads (Dedicated)
    mutable std::mutex m_uploadMutex;

    // ---- Frame authority state (preallocated, reused; §49) ----
    std::unique_ptr<IXVulkanSwapchain> m_swapchain;
    std::array<IXVulkanFrameSlot, IXVulkanFrameTracker::kSlots> m_slots{};
    bool m_slotsReady = false;
    std::vector<VkSemaphore> m_renderFinished; // per swapchain image
    std::vector<VkFence> m_imagesInFlight; // per swapchain image
    IXVulkanFrameTracker m_tracker;
    bool m_frameActive = false;
    std::uint32_t m_activeSlot = 0;
    std::uint32_t m_activeImage = 0;
    std::uint64_t m_activeToken = 0;
    bool m_swapchainDirty = false;
    bool m_pendingResize = false;
    std::uint32_t m_pendingWidth = 0;
    std::uint32_t m_pendingHeight = 0;
    bool m_haveRequestedSize = false;
    std::uint32_t m_requestedWidth = 0;
    std::uint32_t m_requestedHeight = 0;
    bool m_shutDown = false;

    // ---- GPU timestamps (capture-on-request; pools owned here) ----
    VkQueryPool m_queryPool = VK_NULL_HANDLE;
    float m_queryPeriodNs = 0.0f;
    bool m_captureRequested = false;
    bool m_captureActive = false;
    bool m_captureResultsReady = false;
    std::array<bool, ixrhi::IXRHI_MAX_TIMESTAMP_POINTS> m_captureWritten{};
    ixrhi::IXRHITimestampResults m_lastResults{};
    ixrhi::IXRHICpuFrameTiming m_lastCpuTiming{};
    ixrhi::IXRHICpuFrameTiming m_activeCpuTiming{};
    std::chrono::steady_clock::time_point m_cpuFrameStart{};
    std::chrono::steady_clock::time_point m_cpuWorkStart{};
};

#define IXVULKAN_CHECK(device, call) (device).CheckVk((call), #call, __FILE__, __LINE__)

// Minimum generic factory seam (Phase 3C, §117): backend selection lives
// here. Today this always builds Vulkan; a future D3D12 backend is selected
// at this point without changing renderer code. No plugin framework. The
// legacy loop device stays caller-owned (documented infrastructure debt).

std::unique_ptr<ixrhi::IXRHIDevice> CreateDevice(VulkanDevice& loop);

} // namespace ixvulkan
