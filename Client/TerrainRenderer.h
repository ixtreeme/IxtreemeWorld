#pragma once

#include "VulkanDevice.h"
#include "WorldCamera.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

class TerrainRenderer
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
        VkFormat format = VK_FORMAT_UNDEFINED;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t mipLevels = 0;
        std::string name;
    };

    struct TerrainLayer
    {
        uint32_t textureIndex = 0;
        Texture diffuse;
        Texture mask;
        float tilingU = 1.0f;
        float tilingV = 1.0f;
        uint32_t coverage = 0;
    };

    struct MovementBounds
    {
        bool valid = false;
        float minX = 0.0f;
        float maxX = 0.0f;
        float minZ = 0.0f;
        float maxZ = 0.0f;
    };

    bool Create(VulkanDevice& device);
    bool LoadMap(VulkanDevice& device, const std::string& mapDirectory, int32_t serverX, int32_t serverY);
    bool RecreatePipeline(VulkanDevice& device);
    void Render(VulkanDevice& device, const WorldCamera& camera);
    MovementBounds GetMovementBounds() const;
    float SampleHeightAt(float localX, float localZ) const;
    float SampleHeight(WorldVec3 position) const;
    void Destroy();

private:
    static constexpr uint32_t kFramesInFlight = 2;

    struct Vertex
    {
        float position[3];
        float texUv[2];
        float maskUv[2];
    };

    struct UniformBlock
    {
        WorldMat4 mvp;
    };

    bool CreateBuffers(VulkanDevice& device);
    bool CreateFlatBuffers(VulkanDevice& device);
    bool CreateMapBuffers(VulkanDevice& device, const std::string& mapDirectory, int32_t serverX, int32_t serverY);
    bool CreateFallbackTexture(VulkanDevice& device);
    bool CreateFallbackMask(VulkanDevice& device);
    bool LoadDominantTerrainTexture(VulkanDevice& device, const std::string& mapDirectory);
    bool LoadTileIndices(const std::string& mapDirectory);
    bool BuildTerrainLayers(VulkanDevice& device, const std::string& mapDirectory);
    bool GenerateLayerMask(VulkanDevice& device, TerrainLayer& layer);
    bool CreateDescriptors();
    void UpdateDescriptors();
    bool CreatePipeline(VulkanDevice& device);
    void DestroyPipeline();
    void DestroyBuffer(Buffer& buffer);
    void DestroyTexture(Texture& texture);
    void DestroyTerrainLayers();
    void UpdateUniform(uint32_t frameIndex, const WorldCamera& camera);

    VkDevice m_device = VK_NULL_HANDLE;
    Buffer m_vertexBuffer;
    Buffer m_indexBuffer;
    std::array<Buffer, kFramesInFlight> m_uniformBuffers{};
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kFramesInFlight> m_descriptorSets{};
    std::vector<VkDescriptorSet> m_layerDescriptorSets;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    Texture m_baseTexture;
    Texture m_fallbackMask;
    std::vector<TerrainLayer> m_layers;
    std::vector<uint8_t> m_tileIndices;
    uint32_t m_tileGridWidth = 0;
    uint32_t m_tileGridHeight = 0;
    uint32_t m_indexCount = 0;
    uint32_t m_heightGridWidth = 0;
    uint32_t m_heightGridHeight = 0;
    uint32_t m_mapSizeX = 0;
    uint32_t m_mapSizeY = 0;
    float m_cellScaleMeters = 2.0f;
    float m_spawnLocalXcm = 0.0f;
    float m_spawnLocalYcm = 0.0f;
    float m_spawnHeightCm = 0.0f;
    bool m_mapLoaded = false;
    std::vector<float> m_heightCmGrid;
};
