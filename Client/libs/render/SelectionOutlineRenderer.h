#pragma once

#include "VulkanDevice.h"
#include "WorldCamera.h"

#include <array>
#include <cstdint>
#include <vector>

namespace client::asset {
class IAssetReader;
}

class SelectionOutlineRenderer
{
public:
    struct Buffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
    };

    struct Line
    {
        WorldVec3 a{};
        WorldVec3 b{};
        std::array<float, 4> color = {1.0f, 0.61f, 0.07f, 1.0f};
    };

    bool Create(VulkanDevice& device, client::asset::IAssetReader& assets);
    bool RecreatePipeline(VulkanDevice& device);
    void SetMainRenderPass(VkRenderPass renderPass);
    void Render(VulkanDevice& device, const WorldCamera& camera, const std::vector<Line>& lines, VkExtent2D targetExtent = {});
    void Destroy();

private:
    static constexpr uint32_t kFramesInFlight = 2;

    struct Vertex
    {
        float position[3];
        float color[4];
    };

    struct UniformBlock
    {
        WorldMat4 mvp;
    };

    bool CreateBuffers(VulkanDevice& device);
    bool CreateDescriptors();
    bool CreatePipeline(VulkanDevice& device);
    void DestroyPipeline();
    void DestroyBuffer(Buffer& buffer);
    void UpdateUniform(uint32_t frameIndex, const WorldCamera& camera);

    VkDevice m_device = VK_NULL_HANDLE;
    client::asset::IAssetReader* m_assets = nullptr;
    VkRenderPass m_mainRenderPass = VK_NULL_HANDLE;
    Buffer m_vertexBuffers[kFramesInFlight]{};
    Buffer m_uniformBuffers[kFramesInFlight]{};
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_descriptorSets[kFramesInFlight]{};
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
};
