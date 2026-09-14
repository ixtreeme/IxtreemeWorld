// IXVulkan command lists + frame bridge + fence/semaphore + swapchain adapter.

#include "IXVulkanCommandList.h"

#include "IXVulkanBinding.h"
#include "IXVulkanConversions.h"
#include "IXVulkanDevice.h"
#include "IXVulkanPipeline.h"
#include "IXVulkanResources.h"
#include "IXVulkanSync.h"
#include "VulkanDevice.h"

namespace ixvulkan
{

IXVulkanCommandList::IXVulkanCommandList(IXVulkanDevice& device, VkCommandBuffer borrowed)
    : m_device(&device)
    , m_cmd(borrowed)
    , m_ownedPool(VK_NULL_HANDLE)
    , m_recording(true) // loop already began it
{
}

IXVulkanCommandList::IXVulkanCommandList(IXVulkanDevice& device, VkCommandPool pool, VkCommandBuffer owned)
    : m_device(&device)
    , m_cmd(owned)
    , m_ownedPool(pool)
    , m_recording(false)
{
}

IXVulkanCommandList::~IXVulkanCommandList()
{
    // Borrowed buffers stay owned by the frame loop. Owned buffers are freed
    // with their pool (dedicated per list).
    if (m_ownedPool != VK_NULL_HANDLE && m_device != nullptr)
        vkDestroyCommandPool(m_device->NativeDevice(), m_ownedPool, nullptr);
}

void IXVulkanCommandList::Begin()
{
    if (m_ownedPool == VK_NULL_HANDLE || m_recording)
        return; // borrowed: accepted no-op
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    IXVULKAN_CHECK(*m_device, vkBeginCommandBuffer(m_cmd, &begin));
    m_recording = true;
}

void IXVulkanCommandList::End()
{
    if (m_ownedPool == VK_NULL_HANDLE || !m_recording)
        return; // borrowed: accepted no-op
    IXVULKAN_CHECK(*m_device, vkEndCommandBuffer(m_cmd));
    m_recording = false;
}

void IXVulkanCommandList::SetViewport(float x, float y, float width, float height)
{
    VkViewport viewport{};
    viewport.x = x;
    viewport.y = y;
    viewport.width = width;
    viewport.height = height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(m_cmd, 0, 1, &viewport);
}

void IXVulkanCommandList::SetScissor(std::uint32_t x,
                                     std::uint32_t y,
                                     std::uint32_t width,
                                     std::uint32_t height)
{
    const VkRect2D scissor{{static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)},
        {width, height}};
    vkCmdSetScissor(m_cmd, 0, 1, &scissor);
}

void IXVulkanCommandList::SetGraphicsPipeline(const ixrhi::IXRHIGraphicsPipeline& pipeline)
{
    auto* native = dynamic_cast<const IXVulkanGraphicsPipeline*>(&pipeline);
    if (native == nullptr)
        return;
    m_lastLayout = native->NativeLayout();
    vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, native->Native());
}

void IXVulkanCommandList::SetVertexBuffer(std::uint32_t slot,
                                          const ixrhi::IXRHIBuffer& buffer,
                                          std::uint64_t offsetBytes)
{
    auto* native = dynamic_cast<const IXVulkanBuffer*>(&buffer);
    if (native == nullptr)
        return;
    const VkBuffer handle = native->Native();
    const VkDeviceSize offset = static_cast<VkDeviceSize>(offsetBytes);
    vkCmdBindVertexBuffers(m_cmd, slot, 1, &handle, &offset);
}

void IXVulkanCommandList::SetIndexBuffer(const ixrhi::IXRHIBuffer& buffer,
                                           std::uint64_t offsetBytes,
                                           bool thirtyTwoBit)
{
    auto* native = dynamic_cast<const IXVulkanBuffer*>(&buffer);
    if (native == nullptr)
        return;
    vkCmdBindIndexBuffer(m_cmd,
        native->Native(),
        static_cast<VkDeviceSize>(offsetBytes),
        thirtyTwoBit ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
}
void IXVulkanCommandList::BindGroup(std::uint32_t layoutSet,
                                    const ixrhi::IXRHIBindGroup& group,
                                    std::uint32_t slotIndex)
{
    auto* native = dynamic_cast<const IXVulkanBindGroup*>(&group);
    if (native == nullptr || m_lastLayout == VK_NULL_HANDLE)
        return;
    const VkDescriptorSet set = native->NativeSet(slotIndex);
    vkCmdBindDescriptorSets(m_cmd,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        m_lastLayout,
        layoutSet,
        1,
        &set,
        0,
        nullptr);
}

void IXVulkanCommandList::PushConstants(const void* data, std::size_t byteCount)
{
    if (data == nullptr || byteCount == 0 || m_lastLayout == VK_NULL_HANDLE)
        return;
    vkCmdPushConstants(m_cmd,
        m_lastLayout,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        static_cast<std::uint32_t>(byteCount),
        data);
}

void IXVulkanCommandList::Draw(std::uint32_t vertexCount,
                               std::uint32_t instanceCount,
                               std::uint32_t firstVertex,
                               std::uint32_t firstInstance)
{
    vkCmdDraw(m_cmd, vertexCount, instanceCount, firstVertex, firstInstance);
}

void IXVulkanCommandList::DrawIndexed(std::uint32_t indexCount,
                                      std::uint32_t instanceCount,
                                      std::uint32_t firstIndex,
                                      std::int32_t vertexOffset,
                                      std::uint32_t firstInstance)
{
    vkCmdDrawIndexed(m_cmd, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void IXVulkanCommandList::Dispatch(std::uint32_t groupsX, std::uint32_t groupsY, std::uint32_t groupsZ)
{
    vkCmdDispatch(m_cmd, groupsX, groupsY, groupsZ);
}

std::unique_ptr<ixrhi::IXRHICommandList> IXVulkanDevice::CreateCommandList()
{
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = Loop().GetGraphicsQueueFamily();
    VkCommandPool pool = VK_NULL_HANDLE;
    CheckVk(vkCreateCommandPool(NativeDevice(), &poolInfo, nullptr, &pool),
        "vkCreateCommandPool(list)",
        __FILE__,
        __LINE__);
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    CheckVk(vkAllocateCommandBuffers(NativeDevice(), &alloc, &cmd),
        "vkAllocateCommandBuffers(list)",
        __FILE__,
        __LINE__);
    return std::make_unique<IXVulkanCommandList>(*this, pool, cmd);
}

std::unique_ptr<ixrhi::IXRHIFence> IXVulkanDevice::CreateFence(bool signaled)
{
    VkFenceCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    create.flags = signaled ? VK_FENCE_CREATE_SIGNALED_BIT : 0;
    VkFence fence = VK_NULL_HANDLE;
    CheckVk(vkCreateFence(NativeDevice(), &create, nullptr, &fence), "vkCreateFence", __FILE__, __LINE__);
    return std::make_unique<IXVulkanFence>(*this, fence);
}

std::unique_ptr<ixrhi::IXRHISemaphore> IXVulkanDevice::CreateSemaphore()
{
    VkSemaphoreCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    CheckVk(vkCreateSemaphore(NativeDevice(), &create, nullptr, &semaphore),
        "vkCreateSemaphore",
        __FILE__,
        __LINE__);
    return std::make_unique<IXVulkanSemaphore>(*this, semaphore);
}

IXVulkanFence::IXVulkanFence(IXVulkanDevice& device, VkFence fence)
    : m_device(&device)
    , m_fence(fence)
{
}

IXVulkanFence::~IXVulkanFence()
{
    if (m_device != nullptr && m_fence != VK_NULL_HANDLE)
        vkDestroyFence(m_device->NativeDevice(), m_fence, nullptr);
}

void IXVulkanFence::Wait()
{
    IXVULKAN_CHECK(*m_device,
        vkWaitForFences(m_device->NativeDevice(), 1, &m_fence, VK_TRUE, UINT64_MAX));
}

void IXVulkanFence::Reset()
{
    IXVULKAN_CHECK(*m_device, vkResetFences(m_device->NativeDevice(), 1, &m_fence));
}

IXVulkanSemaphore::IXVulkanSemaphore(IXVulkanDevice& device, VkSemaphore semaphore)
    : m_device(&device)
    , m_semaphore(semaphore)
{
}

IXVulkanSemaphore::~IXVulkanSemaphore()
{
    if (m_device != nullptr && m_semaphore != VK_NULL_HANDLE)
        vkDestroySemaphore(m_device->NativeDevice(), m_semaphore, nullptr);
}

IXVulkanSwapchain::IXVulkanSwapchain(IXVulkanDevice& device) : m_device(&device)
{
}

std::uint32_t IXVulkanSwapchain::Width() const
{
    return m_device->Loop().GetSwapchainExtent().width;
}

std::uint32_t IXVulkanSwapchain::Height() const
{
    return m_device->Loop().GetSwapchainExtent().height;
}

ixrhi::IXRHIFormat IXVulkanSwapchain::ColorFormat() const
{
    return FromVkFormat(m_device->Loop().GetSwapchainFormat());
}

ixrhi::IXRHIFormat IXVulkanSwapchain::DepthFormat() const
{
    return FromVkFormat(m_device->Loop().GetDepthStencilFormat());
}

std::uint32_t IXVulkanSwapchain::ImageCount() const
{
    return m_device->Loop().GetSwapchainImageCount();
}

bool IXVulkanSwapchain::RequestResize(std::uint32_t width, std::uint32_t height)
{
    return m_device->Loop().Resize(width, height);
}

} // namespace ixvulkan
