#pragma once

// IXVulkanDevice — Vulkan backend for IXRHIDevice (strangler adapter, Phase 2).
//
// Borrows the live VulkanDevice (frame loop, swapchain, validation layers stay
// there until the Phase-3 frame-loop migration) and implements every IXRHI
// creation call with real Vulkan objects. Renderers see only IXRHIDevice.
//
// Vk* in THIS header is allowed: Engine/Graphics/Vulkan is the backend module.
// It must not leak further: renderer/editor headers take IXRHI types only.
//
// Migration exceptions inventoried here:
// - E2 Loop(): borrowed VulkanDevice for the frame owner + bridge. Dies with the
//   frame-loop migration.
// (E1 SetPipelineRenderPass was removed in Phase 3A: renderers name their pass
// through the borrowed ixrhi::IXRHIRenderPass token instead.)

#include "IXRHIDevice.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <mutex>

class VulkanDevice;

namespace ixvulkan
{

class IXVulkanDevice final : public ixrhi::IXRHIDevice
{
public:
    explicit IXVulkanDevice(VulkanDevice& loop);
    ~IXVulkanDevice() override;

    IXVulkanDevice(const IXVulkanDevice&) = delete;
    IXVulkanDevice& operator=(const IXVulkanDevice&) = delete;

    // ---- ixrhi::IXRHIDevice ----
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
    const ixrhi::IXRHICapabilities& GetCapabilities() const override { return m_capabilities; }

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

private:
    void QueryCapabilities();
    void UploadTextureBytes(VkImage image,
                            std::uint32_t width,
                            std::uint32_t height,
                            ixrhi::IXRHIFormat format,
                            const void* bytes,
                            std::size_t byteCount) const;

    VulkanDevice* m_loop = nullptr; // borrowed frame-loop owner (E2)
    ixrhi::IXRHICapabilities m_capabilities;
    PFN_vkSetDebugUtilsObjectNameEXT m_setDebugName = nullptr;
    VkCommandPool m_uploadPool = VK_NULL_HANDLE; // transient uploads (Dedicated)
    mutable std::mutex m_uploadMutex;
};

#define IXVULKAN_CHECK(device, call) (device).CheckVk((call), #call, __FILE__, __LINE__)

} // namespace ixvulkan
