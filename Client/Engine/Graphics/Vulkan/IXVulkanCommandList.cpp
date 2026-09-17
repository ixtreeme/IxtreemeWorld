// IXVulkan command lists + frame bridge + fence/semaphore + swapchain adapter.

#include "IXVulkanCommandList.h"

#include "IXVulkanBinding.h"
#include "IXVulkanConversions.h"
#include "IXVulkanDevice.h"
#include "IXVulkanPipeline.h"
#include "IXVulkanResources.h"
#include "IXVulkanSync.h"
#include "VulkanDevice.h"

#include <cassert>
#include <functional>

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
    assert(native != nullptr && "foreign IXRHIGraphicsPipeline used with IXVulkan backend");
    if (native == nullptr)
        return;
    m_lastLayout = native->NativeLayout();
    m_lastPushStages = native->PushStages();
    m_lastBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, native->Native());
}

void IXVulkanCommandList::SetComputePipeline(const ixrhi::IXRHIComputePipeline& pipeline)
{
    auto* native = dynamic_cast<const IXVulkanComputePipeline*>(&pipeline);
    assert(native != nullptr && "foreign IXRHIComputePipeline used with IXVulkan backend");
    if (native == nullptr)
        return;
    m_lastLayout = native->NativeLayout();
    m_lastPushStages = native->PushStages();
    m_lastBindPoint = VK_PIPELINE_BIND_POINT_COMPUTE;
    vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, native->Native());
}

void IXVulkanCommandList::SetVertexBuffer(std::uint32_t slot,
                                          const ixrhi::IXRHIBuffer& buffer,
                                          std::uint64_t offsetBytes)
{
    auto* native = dynamic_cast<const IXVulkanBuffer*>(&buffer);
    assert(native != nullptr && "foreign IXRHIBuffer used with IXVulkan backend");
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
    assert(native != nullptr && "foreign IXRHIBuffer used with IXVulkan backend");
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
    assert(native != nullptr && "foreign IXRHIBindGroup used with IXVulkan backend");
    if (native == nullptr || m_lastLayout == VK_NULL_HANDLE)
        return;
    const VkDescriptorSet set = native->NativeSet(slotIndex);
    vkCmdBindDescriptorSets(m_cmd,
        m_lastBindPoint,
        m_lastLayout,
        layoutSet,
        1,
        &set,
        0,
        nullptr);
}

void IXVulkanCommandList::PushConstants(const void* data, std::size_t byteCount)
{
    // Push only against stages declared by the bound pipeline's layout;
    // without a declared range this is a safe no-op (never an invalid call).
    if (data == nullptr || byteCount == 0 || m_lastLayout == VK_NULL_HANDLE || m_lastPushStages == 0)
        return;
    vkCmdPushConstants(m_cmd,
        m_lastLayout,
        m_lastPushStages,
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

namespace
{

void LayoutStageAccess(ixrhi::IXRHIImageLayout layout,
                       VkPipelineStageFlags& stage,
                       VkAccessFlags& access)
{
    using L = ixrhi::IXRHIImageLayout;
    switch (layout)
    {
    case L::Undefined:
        stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        access = 0;
        break;
    case L::TransferSrc:
        stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        access = VK_ACCESS_TRANSFER_READ_BIT;
        break;
    case L::TransferDst:
        stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        access = VK_ACCESS_TRANSFER_WRITE_BIT;
        break;
    case L::ShaderReadOnly:
        stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        access = VK_ACCESS_SHADER_READ_BIT;
        break;
    case L::ColorAttachment:
        stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        break;
    case L::DepthStencilAttachment:
        stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        break;
    case L::Present:
        stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        access = 0;
        break;
    }
}

} // namespace

void IXVulkanCommandList::TransitionTexture(ixrhi::IXRHITexture& texture,
                                            ixrhi::IXRHIImageLayout from,
                                            ixrhi::IXRHIImageLayout to)
{
    auto* native = dynamic_cast<IXVulkanTexture*>(&texture);
    assert(native != nullptr && "foreign IXRHITexture used with IXVulkan backend");
    if (native == nullptr)
        return;
    VkPipelineStageFlags srcStage = 0;
    VkAccessFlags srcAccess = 0;
    VkPipelineStageFlags dstStage = 0;
    VkAccessFlags dstAccess = 0;
    LayoutStageAccess(from, srcStage, srcAccess);
    LayoutStageAccess(to, dstStage, dstAccess);

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = ToVkImageLayout(from);
    barrier.newLayout = ToVkImageLayout(to);
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = native->Native();
    barrier.subresourceRange.aspectMask = ToVkAspectMask(native->Format());
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(m_cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void IXVulkanCommandList::CopyTexture(const ixrhi::IXRHITexture& src, ixrhi::IXRHITexture& dst)
{
    auto* nativeSrc = dynamic_cast<const IXVulkanTexture*>(&src);
    auto* nativeDst = dynamic_cast<IXVulkanTexture*>(&dst);
    assert(nativeSrc != nullptr && nativeDst != nullptr && "foreign IXRHITexture used with IXVulkan backend");
    if (nativeSrc == nullptr || nativeDst == nullptr)
        return;
    const VkImageAspectFlags aspect = ToVkAspectMask(nativeSrc->Format());
    VkImageCopy copy{};
    copy.srcSubresource.aspectMask = aspect;
    copy.srcSubresource.layerCount = 1;
    copy.dstSubresource.aspectMask = aspect;
    copy.dstSubresource.layerCount = 1;
    copy.extent = {nativeSrc->Width(), nativeSrc->Height(), 1};
    vkCmdCopyImage(m_cmd,
        nativeSrc->Native(),
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        nativeDst->Native(),
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &copy);
}

void IXVulkanCommandList::ClearDepth(float depth,
                                     std::uint32_t x,
                                     std::uint32_t y,
                                     std::uint32_t width,
                                     std::uint32_t height)
{
    if (width == 0 || height == 0)
        return;
    VkClearAttachment clear{};
    clear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    clear.clearValue.depthStencil.depth = depth;
    VkClearRect rect{};
    rect.rect.offset = {static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)};
    rect.rect.extent = {width, height};
    rect.baseArrayLayer = 0;
    rect.layerCount = 1;
    vkCmdClearAttachments(m_cmd, 1, &clear, 1, &rect);
}

void IXVulkanCommandList::CopyBuffer(const ixrhi::IXRHIBuffer& src,
                                     ixrhi::IXRHIBuffer& dst,
                                     std::uint64_t byteCount)
{
    auto* nativeSrc = dynamic_cast<const IXVulkanBuffer*>(&src);
    auto* nativeDst = dynamic_cast<IXVulkanBuffer*>(&dst);
    assert(nativeSrc != nullptr && nativeDst != nullptr && "foreign IXRHIBuffer used with IXVulkan backend");
    if (nativeSrc == nullptr || nativeDst == nullptr || byteCount == 0)
        return;
    VkBufferCopy copy{};
    copy.size = static_cast<VkDeviceSize>(byteCount);
    vkCmdCopyBuffer(m_cmd, nativeSrc->Native(), nativeDst->Native(), 1, &copy);
}

namespace
{

void BufferStageAccess(ixrhi::IXRHIBufferState state,
                       VkPipelineStageFlags& stage,
                       VkAccessFlags& access)
{
    using S = ixrhi::IXRHIBufferState;
    switch (state)
    {
    case S::Undefined:
        stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        access = 0;
        break;
    case S::ShaderRead:
        stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
        access = VK_ACCESS_SHADER_READ_BIT;
        break;
    case S::ShaderWrite:
        stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        access = VK_ACCESS_SHADER_WRITE_BIT;
        break;
    case S::VertexRead:
        stage = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
        access = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        break;
    case S::IndexRead:
        stage = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
        access = VK_ACCESS_INDEX_READ_BIT;
        break;
    case S::UniformRead:
        stage = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        access = VK_ACCESS_UNIFORM_READ_BIT;
        break;
    case S::TransferSrc:
        stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        access = VK_ACCESS_TRANSFER_READ_BIT;
        break;
    case S::TransferDst:
        stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        access = VK_ACCESS_TRANSFER_WRITE_BIT;
        break;
    }
}

} // namespace

void IXVulkanCommandList::TransitionBuffer(ixrhi::IXRHIBuffer& buffer,
                                           ixrhi::IXRHIBufferState from,
                                           ixrhi::IXRHIBufferState to)
{
    auto* native = dynamic_cast<IXVulkanBuffer*>(&buffer);
    assert(native != nullptr && "foreign IXRHIBuffer used with IXVulkan backend");
    if (native == nullptr)
        return;
    VkPipelineStageFlags srcStage = 0;
    VkAccessFlags srcAccess = 0;
    VkPipelineStageFlags dstStage = 0;
    VkAccessFlags dstAccess = 0;
    BufferStageAccess(from, srcStage, srcAccess);
    BufferStageAccess(to, dstStage, dstAccess);

    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = native->Native();
    barrier.offset = 0;
    barrier.size = static_cast<VkDeviceSize>(native->SizeBytes());
    vkCmdPipelineBarrier(m_cmd, srcStage, dstStage, 0, 0, nullptr, 1, &barrier, 0, nullptr);
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

void IXVulkanDevice::ExecuteAndWait(const std::function<void(ixrhi::IXRHICommandList&)>& record)
{
    if (!record)
        return;
    auto list = CreateCommandList();
    if (!list)
        return;
    list->Begin();
    record(*list);
    list->End();

    auto* native = dynamic_cast<IXVulkanCommandList*>(list.get());
    if (native == nullptr)
        return;
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    IXVULKAN_CHECK(*this, vkCreateFence(NativeDevice(), &fenceInfo, nullptr, &fence));
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    VkCommandBuffer cmd = native->Native();
    submit.pCommandBuffers = &cmd;
    IXVULKAN_CHECK(*this, vkQueueSubmit(Loop().GetGraphicsQueue(), 1, &submit, fence));
    IXVULKAN_CHECK(*this, vkWaitForFences(NativeDevice(), 1, &fence, VK_TRUE, UINT64_MAX));
    vkDestroyFence(NativeDevice(), fence, nullptr);
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

} // namespace ixvulkan