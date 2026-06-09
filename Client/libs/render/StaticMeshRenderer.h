#pragma once

#include "MapEditorTypes.h"
#include "VulkanDevice.h"
#include "WorldCamera.h"

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace client::asset {
class IAssetReader;
}

class StaticMeshRenderer
{
public:
    enum class LoadStatus
    {
        NotLoaded,
        LoadedStatic,
        UnsupportedSkinned,
        Failed
    };

    struct Buffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
    };

    struct Instance
    {
        WorldVec3 position{};
        float rotation[3] = {0.0f, 0.0f, 0.0f};
        float scale[3] = {1.0f, 1.0f, 1.0f};
        std::array<float, 4> tint = {1.0f, 1.0f, 1.0f, 1.0f};
    };

    StaticMeshRenderer() = default;
    ~StaticMeshRenderer();

    bool Create(VulkanDevice& device, client::asset::IAssetReader& assets, const std::string& modelPath);
    bool RecreatePipeline(VulkanDevice& device);
    void SetMainRenderPass(VkRenderPass renderPass);
    void SetLightingState(const LightingState& lighting) { m_lightingState = lighting; }
    void RenderInWorld(VulkanDevice& device, double timeSeconds, const WorldCamera& camera, const Instance& instance);
    void Destroy();

    bool IsLoaded() const { return m_status == LoadStatus::LoadedStatic; }
    LoadStatus Status() const { return m_status; }
    bool IsSkinnedModel() const { return m_status == LoadStatus::UnsupportedSkinned; }

    static bool DetectSkinnedGltf(client::asset::IAssetReader& assets,
        const std::string& modelPath,
        bool& outSkinned,
        std::string* error);

private:
    static constexpr uint32_t kFramesInFlight = 2;
    static constexpr uint32_t kUniformSlots = 64;

    struct Vertex
    {
        float position[3];
        float normal[3];
        float uv[2];
    };

    struct MeshDraw
    {
        uint32_t firstIndex = 0;
        uint32_t indexCount = 0;
    };

    struct Texture
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t mipLevels = 0;
        std::string name;
    };

    bool LoadStaticGltfMesh(const std::string& modelPath);
    bool CreateBuffers(VulkanDevice& device);
    bool CreateTexture(VulkanDevice& device, const std::string& modelPath);
    bool CreateDescriptors();
    bool CreatePipeline(VulkanDevice& device);
    void DestroyPipeline();
    void DestroyBuffer(Buffer& buffer);
    void DestroyTexture(Texture& texture);
    void UpdateWorldUniform(uint32_t frameIndex,
        uint32_t uniformSlot,
        const WorldCamera& camera,
        const Instance& instance,
        double timeSeconds);

    VkDevice m_device = VK_NULL_HANDLE;
    client::asset::IAssetReader* m_assets = nullptr;
    VkRenderPass m_mainRenderPass = VK_NULL_HANDLE;
    Buffer m_vertexBuffer;
    Buffer m_indexBuffer;
    std::array<std::array<Buffer, kUniformSlots>, kFramesInFlight> m_uniformBuffers{};
    Texture m_texture;
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    std::array<std::array<VkDescriptorSet, kUniformSlots>, kFramesInFlight> m_descriptorSets{};
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    std::vector<Vertex> m_vertices;
    std::vector<uint32_t> m_indices;
    std::vector<MeshDraw> m_draws;
    LightingState m_lightingState;
    LoadStatus m_status = LoadStatus::NotLoaded;
    uint32_t m_worldRenderFrameIndex = std::numeric_limits<uint32_t>::max();
    uint32_t m_worldUniformCursor = 0;
};
