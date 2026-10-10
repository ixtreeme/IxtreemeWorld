#pragma once

// IXVulkan backend resource objects. Vulkan-module only; renderers use the
// IXRHI base classes. RAII: destruction releases Vulkan objects in the same
// order the pre-migration code used (view/sampler before image, etc.).

#include "IXRHIBuffer.h"
#include "IXRHIQuery.h"
#include "IXRHIShader.h"
#include "IXRHITexture.h"
#include "IXVulkanConversions.h"

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ixvulkan
{

class IXVulkanDevice;

class IXVulkanBuffer final : public ixrhi::IXRHIBuffer
{
public:
    IXVulkanBuffer(IXVulkanDevice& device,
                   VkBuffer buffer,
                   VkDeviceMemory memory,
                   std::uint64_t sizeBytes,
                   ixrhi::IXRHIBufferUsage usage,
                   std::string debugName);
    ~IXVulkanBuffer() override;

    void Write(std::uint64_t dstOffsetBytes, const void* src, std::size_t byteCount) override;
    void Read(std::uint64_t srcOffsetBytes, void* dst, std::size_t byteCount) override;
    void* HostAddress() override { return Mapped(); }
    std::uint64_t SizeBytes() const override { return m_sizeBytes; }
    ixrhi::IXRHIBufferUsage Usage() const override { return m_usage; }
    const std::string& DebugName() const override { return m_debugName; }

    VkBuffer Native() const { return m_buffer; }

private:
    // Host-visible (coherent) memory stays mapped from the first Write/Read on: mapping and unmapping
    // on every write (per-draw uniforms, per-frame instance data) was a driver round trip each time.
    void* Mapped();

    IXVulkanDevice* m_device = nullptr; // borrowed backend (outlives resources)
    VkBuffer m_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    void* m_mapped = nullptr;
    std::uint64_t m_sizeBytes = 0;
    ixrhi::IXRHIBufferUsage m_usage = ixrhi::IXRHIBufferUsage::None;
    std::string m_debugName;
};

class IXVulkanTexture final : public ixrhi::IXRHITexture
{
public:
    IXVulkanTexture(IXVulkanDevice& device,
                    VkImage image,
                    VkDeviceMemory memory,
                    VkImageView view,
                    std::uint32_t width,
                    std::uint32_t height,
                    std::uint32_t mipLevels,
                    std::uint32_t arrayLayers,
                    ixrhi::IXRHIFormat format,
                    std::string debugName);
    ~IXVulkanTexture() override;

    std::uint32_t Width() const override { return m_width; }
    std::uint32_t Height() const override { return m_height; }
    std::uint32_t MipLevels() const override { return m_mipLevels; }
    std::uint32_t ArrayLayers() const override { return m_arrayLayers; }
    ixrhi::IXRHIFormat Format() const override { return m_format; }
    const std::string& DebugName() const override { return m_debugName; }
    std::uint64_t AllocatedBytes() const override { return m_allocatedBytes; }

    VkImage Native() const { return m_image; }
    VkImageView NativeView() const { return m_view; }
    // The view shaders sample through: a depth+stencil attachment's own view names both aspects,
    // which a sampled view must not, so such a texture also gets a depth-only one.
    VkImageView NativeSampledView() const { return m_sampledView != VK_NULL_HANDLE ? m_sampledView : m_view; }
    void SetSampledView(VkImageView view) { m_sampledView = view; }  // owned (destroyed with the texture)

private:
    IXVulkanDevice* m_device = nullptr;
    VkImage m_image = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    VkImageView m_view = VK_NULL_HANDLE;
    VkImageView m_sampledView = VK_NULL_HANDLE;
    std::uint64_t m_allocatedBytes = 0;
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    std::uint32_t m_mipLevels = 1;
    std::uint32_t m_arrayLayers = 1;
    ixrhi::IXRHIFormat m_format = ixrhi::IXRHIFormat::Undefined;
    std::string m_debugName;
};

class IXVulkanSampler final : public ixrhi::IXRHISampler
{
public:
    IXVulkanSampler(IXVulkanDevice& device, VkSampler sampler, std::string debugName);
    ~IXVulkanSampler() override;

    const std::string& DebugName() const override { return m_debugName; }
    VkSampler Native() const { return m_sampler; }

private:
    IXVulkanDevice* m_device = nullptr;
    VkSampler m_sampler = VK_NULL_HANDLE;
    std::string m_debugName;
};

class IXVulkanShader final : public ixrhi::IXRHIShader
{
public:
    IXVulkanShader(IXVulkanDevice& device,
                   VkShaderModule module,
                   ixrhi::IXRHIShaderStage stage,
                   std::string entryPoint,
                   std::string debugName);
    ~IXVulkanShader() override;

    ixrhi::IXRHIShaderStage Stage() const override { return m_stage; }
    const std::string& EntryPoint() const override { return m_entryPoint; }
    const std::string& DebugName() const override { return m_debugName; }
    VkShaderModule Native() const { return m_module; }

private:
    IXVulkanDevice* m_device = nullptr;
    VkShaderModule m_module = VK_NULL_HANDLE;
    ixrhi::IXRHIShaderStage m_stage = ixrhi::IXRHIShaderStage::None;
    std::string m_entryPoint;
    std::string m_debugName;
};

// Non-blocking staged upload (see ixrhi::IXRHIBufferUpload). Owns its transient
// pool + fence; destruction before Take() waits and releases everything.
class IXVulkanBufferUpload final : public ixrhi::IXRHIBufferUpload
{
public:
    IXVulkanBufferUpload(IXVulkanDevice& device,
                         const ixrhi::IXRHIBufferDesc& desc,
                         const void* src,
                         std::size_t byteCount);
    ~IXVulkanBufferUpload() override;

    bool IsReady() override;
    std::shared_ptr<ixrhi::IXRHIBuffer> Take() override;

private:
    void ReleaseStaging();

    IXVulkanDevice* m_device = nullptr;
    std::shared_ptr<IXVulkanBuffer> m_target;
    VkBuffer m_staging = VK_NULL_HANDLE;
    VkDeviceMemory m_stagingMemory = VK_NULL_HANDLE;
    VkCommandPool m_pool = VK_NULL_HANDLE;
    VkFence m_fence = VK_NULL_HANDLE;
    bool m_ready = false;
    bool m_taken = false;
};

class IXVulkanOcclusionQueries final : public ixrhi::IXRHIOcclusionQueries
{
public:
    IXVulkanOcclusionQueries(IXVulkanDevice& device, VkQueryPool pool, std::uint32_t count);
    ~IXVulkanOcclusionQueries() override;

    std::uint32_t Count() const override { return m_count; }
    void Reset(ixrhi::IXRHICommandList& cmd, std::uint32_t first, std::uint32_t count) override;
    void Begin(ixrhi::IXRHICommandList& cmd, std::uint32_t index) override;
    void End(ixrhi::IXRHICommandList& cmd, std::uint32_t index) override;
    bool TryGetResult(std::uint32_t index, std::uint64_t& samples) override;

private:
    IXVulkanDevice* m_device = nullptr;
    VkQueryPool m_pool = VK_NULL_HANDLE;
    std::uint32_t m_count = 0;
};

} // namespace ixvulkan
