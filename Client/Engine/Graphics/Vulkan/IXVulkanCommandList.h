#pragma once

// IXVulkan command lists. Two flavors, one renderer-visible interface:
// - Borrowed: wraps the in-flight frame VkCommandBuffer (already recording
//   inside the loop's render pass). Begin()/End() are accepted no-ops.
// - Owned: allocated from the backend's dedicated pool; Begin()/End() bracket
//   recording (for future offscreen/secondary work; unused by Phase-2 renderers).

#include "IXRHICommandList.h"

#include <vulkan/vulkan.h>

namespace ixvulkan
{

class IXVulkanDevice;

class IXVulkanCommandList final : public ixrhi::IXRHICommandList
{
public:
    // Borrowed frame buffer (non-owning). owned=false + pool null.
    IXVulkanCommandList(IXVulkanDevice& device, VkCommandBuffer borrowed);
    // Owned buffer from the backend pool.
    IXVulkanCommandList(IXVulkanDevice& device, VkCommandPool pool, VkCommandBuffer owned);
    ~IXVulkanCommandList() override;

    void Begin() override;
    void End() override;
    void SetViewport(float x, float y, float width, float height) override;
    void SetScissor(std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height) override;
    void SetGraphicsPipeline(const ixrhi::IXRHIGraphicsPipeline& pipeline) override;
    void SetVertexBuffer(std::uint32_t slot,
                         const ixrhi::IXRHIBuffer& buffer,
                         std::uint64_t offsetBytes) override;
    void SetIndexBuffer(const ixrhi::IXRHIBuffer& buffer,
                        std::uint64_t offsetBytes,
                        bool thirtyTwoBit) override;
    void BindGroup(std::uint32_t layoutSet,
                   const ixrhi::IXRHIBindGroup& group,
                   std::uint32_t slotIndex) override;
    void PushConstants(const void* data, std::size_t byteCount) override;
    void Draw(std::uint32_t vertexCount,
              std::uint32_t instanceCount,
              std::uint32_t firstVertex,
              std::uint32_t firstInstance) override;
    void DrawIndexed(std::uint32_t indexCount,
                     std::uint32_t instanceCount,
                     std::uint32_t firstIndex,
                     std::int32_t vertexOffset,
                     std::uint32_t firstInstance) override;
    void Dispatch(std::uint32_t groupsX, std::uint32_t groupsY, std::uint32_t groupsZ) override;
    void TransitionTexture(ixrhi::IXRHITexture& texture,
                           ixrhi::IXRHIImageLayout from,
                           ixrhi::IXRHIImageLayout to) override;
    void CopyTexture(const ixrhi::IXRHITexture& src, ixrhi::IXRHITexture& dst) override;

    VkCommandBuffer Native() const { return m_cmd; }

private:
    IXVulkanDevice* m_device = nullptr;
    VkCommandBuffer m_cmd = VK_NULL_HANDLE;
    VkCommandPool m_ownedPool = VK_NULL_HANDLE; // null when borrowed
    VkPipelineLayout m_lastLayout = VK_NULL_HANDLE; // stashed for PushConstants
    bool m_recording = false;
};

} // namespace ixvulkan
