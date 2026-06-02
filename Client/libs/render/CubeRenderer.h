#pragma once

#include "VulkanDevice.h"

#include <array>

namespace client::asset {
class IAssetReader;
}

class CubeRenderer
{
public:
    struct Buffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
    };

    bool Create(VulkanDevice& device, client::asset::IAssetReader& assets);
    bool RecreatePipeline(VulkanDevice& device);
    void Render(VulkanDevice& device, double timeSeconds);
    void Destroy();

private:
    static constexpr uint32_t kFramesInFlight = 2;

    bool CreateBuffers(VulkanDevice& device);
    bool CreateDescriptors(VulkanDevice& device);
    bool CreatePipeline(VulkanDevice& device);
    void DestroyPipeline();
    void DestroyBuffer(Buffer& buffer);
    void UpdateUniform(uint32_t frameIndex, double timeSeconds, float aspect);

    VkDevice m_device = VK_NULL_HANDLE;
    client::asset::IAssetReader* m_assets = nullptr;
    Buffer m_vertexBuffer;
    Buffer m_indexBuffer;
    std::array<Buffer, kFramesInFlight> m_uniformBuffers{};
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kFramesInFlight> m_descriptorSets{};
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
};
