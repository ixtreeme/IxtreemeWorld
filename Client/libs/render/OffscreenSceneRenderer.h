#pragma once

#include "platform/VulkanDevice.h"

#include <cstdint>

namespace client::asset {
class IAssetReader;
}

class OffscreenSceneRenderer
{
public:
    bool Create(VulkanDevice& device, client::asset::IAssetReader& assets);
    bool Recreate(VulkanDevice& device);
    void BeginMainPass(VulkanDevice& device, bool clear = true);
    void EndMainPass(VulkanDevice& device);
    void SnapshotScene(VulkanDevice& device);
    void RenderComposite(VulkanDevice& device);
    void Destroy();

    bool IsReady() const { return m_ready; }
    VkRenderPass GetRenderPass() const { return m_renderPass; }
    VkImageView GetSceneColorView() const { return m_colorView; }
    VkImageView GetSceneColorSnapshotView() const { return m_sceneColorSnapshotView; }
    VkImageView GetSceneDepthSnapshotView() const { return m_sceneDepthSnapshotView; }
    VkSampler GetLinearSampler() const { return m_sampler; }
    VkExtent2D GetExtent() const { return m_extent; }

private:
    bool CreateRenderPass(VulkanDevice& device);
    bool CreateImages(VulkanDevice& device);
    bool CreateFramebuffer(VulkanDevice& device);
    bool CreateSampler();
    bool CreateDescriptorResources();
    bool CreatePipeline(VulkanDevice& device);
    void UpdateDescriptor();

    VkDevice m_device = VK_NULL_HANDLE;
    client::asset::IAssetReader* m_assets = nullptr;
    VkExtent2D m_extent{};
    VkFormat m_colorFormat = VK_FORMAT_UNDEFINED;
    VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;

    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    VkImage m_colorImage = VK_NULL_HANDLE;
    VkDeviceMemory m_colorMemory = VK_NULL_HANDLE;
    VkImageView m_colorView = VK_NULL_HANDLE;
    VkImage m_depthImage = VK_NULL_HANDLE;
    VkDeviceMemory m_depthMemory = VK_NULL_HANDLE;
    VkImageView m_depthView = VK_NULL_HANDLE;
    VkImage m_sceneColorSnapshot = VK_NULL_HANDLE;
    VkDeviceMemory m_sceneColorSnapshotMemory = VK_NULL_HANDLE;
    VkImageView m_sceneColorSnapshotView = VK_NULL_HANDLE;
    VkImage m_sceneDepthSnapshot = VK_NULL_HANDLE;
    VkDeviceMemory m_sceneDepthSnapshotMemory = VK_NULL_HANDLE;
    VkImageView m_sceneDepthSnapshotView = VK_NULL_HANDLE;
    VkFramebuffer m_framebuffer = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkRenderPass m_loadRenderPass = VK_NULL_HANDLE;

    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;

    bool m_ready = false;
    bool m_passActive = false;
    bool m_snapshotsReady = false;
};
