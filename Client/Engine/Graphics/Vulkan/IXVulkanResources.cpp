// IXVulkan resource objects: RAII destruction + host writes.

#include "IXVulkanResources.h"

#include "IXVulkanDevice.h"

#include <cstring>

namespace ixvulkan
{

IXVulkanBuffer::IXVulkanBuffer(IXVulkanDevice& device,
                               VkBuffer buffer,
                               VkDeviceMemory memory,
                               std::uint64_t sizeBytes,
                               ixrhi::IXRHIBufferUsage usage,
                               std::string debugName)
    : m_device(&device)
    , m_buffer(buffer)
    , m_memory(memory)
    , m_sizeBytes(sizeBytes)
    , m_usage(usage)
    , m_debugName(std::move(debugName))
{
}

IXVulkanBuffer::~IXVulkanBuffer()
{
    if (m_device == nullptr)
        return;
    const VkDevice native = m_device->NativeDevice();
    if (m_buffer != VK_NULL_HANDLE)
        vkDestroyBuffer(native, m_buffer, nullptr);
    if (m_memory != VK_NULL_HANDLE)
        vkFreeMemory(native, m_memory, nullptr);
}

void IXVulkanBuffer::Write(std::uint64_t dstOffsetBytes, const void* src, std::size_t byteCount)
{
    if (src == nullptr || byteCount == 0 || dstOffsetBytes + byteCount > m_sizeBytes)
        return; // InvalidArgument-class misuse: ignore (validated at higher level)
    void* mapped = nullptr;
    IXVULKAN_CHECK(*m_device,
        vkMapMemory(m_device->NativeDevice(),
            m_memory,
            static_cast<VkDeviceSize>(dstOffsetBytes),
            static_cast<VkDeviceSize>(byteCount),
            0,
            &mapped));
    std::memcpy(mapped, src, byteCount);
    vkUnmapMemory(m_device->NativeDevice(), m_memory);
}

IXVulkanTexture::IXVulkanTexture(IXVulkanDevice& device,
                                 VkImage image,
                                 VkDeviceMemory memory,
                                 VkImageView view,
                                 std::uint32_t width,
                                 std::uint32_t height,
                                 ixrhi::IXRHIFormat format,
                                 std::string debugName)
    : m_device(&device)
    , m_image(image)
    , m_memory(memory)
    , m_view(view)
    , m_width(width)
    , m_height(height)
    , m_format(format)
    , m_debugName(std::move(debugName))
{
}

IXVulkanTexture::~IXVulkanTexture()
{
    if (m_device == nullptr)
        return;
    const VkDevice native = m_device->NativeDevice();
    if (m_view != VK_NULL_HANDLE)
        vkDestroyImageView(native, m_view, nullptr);
    if (m_image != VK_NULL_HANDLE)
        vkDestroyImage(native, m_image, nullptr);
    if (m_memory != VK_NULL_HANDLE)
        vkFreeMemory(native, m_memory, nullptr);
}

IXVulkanSampler::IXVulkanSampler(IXVulkanDevice& device, VkSampler sampler, std::string debugName)
    : m_device(&device)
    , m_sampler(sampler)
    , m_debugName(std::move(debugName))
{
}

IXVulkanSampler::~IXVulkanSampler()
{
    if (m_device != nullptr && m_sampler != VK_NULL_HANDLE)
        vkDestroySampler(m_device->NativeDevice(), m_sampler, nullptr);
}

IXVulkanShader::IXVulkanShader(IXVulkanDevice& device,
                               VkShaderModule module,
                               ixrhi::IXRHIShaderStage stage,
                               std::string entryPoint,
                               std::string debugName)
    : m_device(&device)
    , m_module(module)
    , m_stage(stage)
    , m_entryPoint(std::move(entryPoint))
    , m_debugName(std::move(debugName))
{
}

IXVulkanShader::~IXVulkanShader()
{
    if (m_device != nullptr && m_module != VK_NULL_HANDLE)
        vkDestroyShaderModule(m_device->NativeDevice(), m_module, nullptr);
}

} // namespace ixvulkan
