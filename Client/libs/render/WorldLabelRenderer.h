#pragma once

#include "VulkanDevice.h"
#include "WorldCamera.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace client::asset {
class IAssetReader;
}

class WorldLabelRenderer
{
public:
    struct Buffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
    };

    struct Texture
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    struct Label
    {
        WorldVec3 position{};
        std::string text;
        std::array<float, 4> color = {0.92f, 0.96f, 1.0f, 1.0f};
        bool selected = false;
    };

    bool Create(VulkanDevice& device, client::asset::IAssetReader& assets);
    bool RecreatePipeline(VulkanDevice& device);
    void Render(VulkanDevice& device, const WorldCamera& camera, const std::vector<Label>& worldLabels);
    void Destroy();

private:
    static constexpr uint32_t kFramesInFlight = 2;

    struct Vertex
    {
        float position[3];
        float uv[2];
        float color[4];
    };

    struct UniformBlock
    {
        WorldMat4 mvp;
    };

    struct Glyph
    {
        float u0 = 0.0f;
        float v0 = 0.0f;
        float u1 = 0.0f;
        float v1 = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        float advance = 0.0f;
    };

    bool CreateBuffers(VulkanDevice& device);
    bool CreateFontAtlas(VulkanDevice& device);
    bool CreateDescriptors();
    bool CreatePipeline(VulkanDevice& device);
    void DestroyPipeline();
    void DestroyBuffer(Buffer& buffer);
    void DestroyTexture(Texture& texture);
    void UpdateUniform(uint32_t frameIndex, const WorldCamera& camera);
    void BuildVertices(const WorldCamera& camera, const std::vector<Label>& worldLabels, std::vector<Vertex>& vertices) const;
    void AppendLine(std::vector<Vertex>& vertices, WorldVec3 origin, WorldVec3 right, WorldVec3 up,
        const std::string& text, float pixelScale, const float color[4], float fade = 1.0f) const;
    void AppendQuad(std::vector<Vertex>& vertices, WorldVec3 origin, WorldVec3 right, WorldVec3 up,
        float width, float height, const float color[4], float fade = 1.0f) const;

    VkDevice m_device = VK_NULL_HANDLE;
    client::asset::IAssetReader* m_assets = nullptr;
    Buffer m_vertexBuffers[kFramesInFlight]{};
    std::array<Buffer, kFramesInFlight> m_uniformBuffers{};
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kFramesInFlight> m_descriptorSets{};
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    Texture m_fontAtlas;
    std::array<Glyph, 128> m_glyphs{};
};
