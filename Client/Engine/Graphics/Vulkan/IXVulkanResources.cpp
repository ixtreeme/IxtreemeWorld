// IXVulkan resource objects: RAII destruction + host writes.

#include "IXVulkanResources.h"

#include "IXVulkanDevice.h"
#include "VulkanDevice.h"

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

void IXVulkanBuffer::Read(std::uint64_t srcOffsetBytes, void* dst, std::size_t byteCount)
{
    if (dst == nullptr || byteCount == 0 || srcOffsetBytes + byteCount > m_sizeBytes)
        return;
    void* mapped = nullptr;
    IXVULKAN_CHECK(*m_device,
        vkMapMemory(m_device->NativeDevice(),
            m_memory,
            static_cast<VkDeviceSize>(srcOffsetBytes),
            static_cast<VkDeviceSize>(byteCount),
            0,
            &mapped));
    std::memcpy(dst, mapped, byteCount);
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

IXVulkanBufferUpload::IXVulkanBufferUpload(IXVulkanDevice& device,
                                           const ixrhi::IXRHIBufferDesc& desc,
                                           const void* src,
                                           std::size_t byteCount)
    : m_device(&device)
{
    const std::size_t bytes =
        byteCount < desc.sizeBytes ? byteCount : static_cast<std::size_t>(desc.sizeBytes);

    VkBufferCreateInfo targetInfo{};
    targetInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    targetInfo.size = static_cast<VkDeviceSize>(desc.sizeBytes);
    targetInfo.usage = ToVkBufferUsage(desc.usage) | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    targetInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer target = VK_NULL_HANDLE;
    IXVULKAN_CHECK(device, vkCreateBuffer(device.NativeDevice(), &targetInfo, nullptr, &target));
    VkDeviceMemory targetMemory = VK_NULL_HANDLE;
    device.AllocateAndBind(target, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, targetMemory);
    device.SetDebugName(VK_OBJECT_TYPE_BUFFER,
        reinterpret_cast<std::uint64_t>(target),
        desc.debugName.c_str());
    m_target = std::make_shared<IXVulkanBuffer>(device,
        target,
        targetMemory,
        desc.sizeBytes,
        desc.usage,
        desc.debugName);

    VkBufferCreateInfo stagingInfo{};
    stagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    stagingInfo.size = static_cast<VkDeviceSize>(bytes);
    stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    IXVULKAN_CHECK(device,
        vkCreateBuffer(device.NativeDevice(), &stagingInfo, nullptr, &m_staging));
    device.AllocateAndBind(m_staging,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        m_stagingMemory);
    void* mapped = nullptr;
    IXVULKAN_CHECK(device,
        vkMapMemory(device.NativeDevice(), m_stagingMemory, 0, stagingInfo.size, 0, &mapped));
    std::memcpy(mapped, src, bytes);
    vkUnmapMemory(device.NativeDevice(), m_stagingMemory);

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = device.Loop().GetGraphicsQueueFamily();
    IXVULKAN_CHECK(device,
        vkCreateCommandPool(device.NativeDevice(), &poolInfo, nullptr, &m_pool));
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = m_pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    IXVULKAN_CHECK(device, vkAllocateCommandBuffers(device.NativeDevice(), &alloc, &cmd));
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    IXVULKAN_CHECK(device, vkBeginCommandBuffer(cmd, &begin));
    VkBufferCopy region{};
    region.size = static_cast<VkDeviceSize>(bytes);
    vkCmdCopyBuffer(cmd, m_staging, target, 1, &region);
    IXVULKAN_CHECK(device, vkEndCommandBuffer(cmd));

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    IXVULKAN_CHECK(device, vkCreateFence(device.NativeDevice(), &fenceInfo, nullptr, &m_fence));
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    // No wait: completion is polled via IsReady (parity with the old LOD path).
    IXVULKAN_CHECK(device,
        vkQueueSubmit(device.Loop().GetGraphicsQueue(), 1, &submit, m_fence));
}

IXVulkanBufferUpload::~IXVulkanBufferUpload()
{
    if (m_device == nullptr)
        return;
    const VkDevice native = m_device->NativeDevice();
    if (!m_taken && m_fence != VK_NULL_HANDLE)
        vkWaitForFences(native, 1, &m_fence, VK_TRUE, UINT64_MAX);
    // If taken, staging was already released in Take(); target (if untaken)
    // frees itself via shared_ptr after we return.
    ReleaseStaging();
    if (m_fence != VK_NULL_HANDLE)
        vkDestroyFence(native, m_fence, nullptr);
    m_target.reset();
}

bool IXVulkanBufferUpload::IsReady()
{
    if (m_taken || m_ready || m_device == nullptr || m_fence == VK_NULL_HANDLE)
        return m_ready && !m_taken;
    const VkResult status = vkGetFenceStatus(m_device->NativeDevice(), m_fence);
    if (status == VK_SUCCESS)
        m_ready = true;
    return m_ready;
}

std::shared_ptr<ixrhi::IXRHIBuffer> IXVulkanBufferUpload::Take()
{
    if (m_taken || !IsReady())
        return nullptr;
    // Fence is signaled; wait is immediate, keeps teardown race-free.
    vkWaitForFences(m_device->NativeDevice(), 1, &m_fence, VK_TRUE, UINT64_MAX);
    ReleaseStaging();
    vkDestroyFence(m_device->NativeDevice(), m_fence, nullptr);
    m_fence = VK_NULL_HANDLE;
    m_taken = true;
    return m_target;
}

void IXVulkanBufferUpload::ReleaseStaging()
{
    if (m_device == nullptr)
        return;
    const VkDevice native = m_device->NativeDevice();
    if (m_pool != VK_NULL_HANDLE)
        vkDestroyCommandPool(native, m_pool, nullptr);
    m_pool = VK_NULL_HANDLE;
    if (m_staging != VK_NULL_HANDLE)
        vkDestroyBuffer(native, m_staging, nullptr);
    m_staging = VK_NULL_HANDLE;
    if (m_stagingMemory != VK_NULL_HANDLE)
        vkFreeMemory(native, m_stagingMemory, nullptr);
    m_stagingMemory = VK_NULL_HANDLE;
}

} // namespace ixvulkan
