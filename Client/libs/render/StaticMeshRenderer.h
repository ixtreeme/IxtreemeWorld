#pragma once

#include "MapEditorTypes.h"
#include "VulkanDevice.h"
#include "WorldCamera.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
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

    struct MaterialDefaults
    {
        float baseColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        float metallic = 1.0f;
        float roughness = 1.0f;
        float normalStrength = 1.0f;
        float aoStrength = 1.0f;
        float emissive[3] = {0.0f, 0.0f, 0.0f};
        std::string alphaMode = "opaque";
        float alphaCutoff = 0.5f;
    };

    struct Instance
    {
        std::uint32_t entityId = 0;
        WorldVec3 position{};
        float rotation[3] = {0.0f, 0.0f, 0.0f};
        float scale[3] = {1.0f, 1.0f, 1.0f};
        std::array<float, 4> tint = {1.0f, 1.0f, 1.0f, 1.0f};
        std::vector<std::string> materialSlots;
        std::vector<MeshSceneEntity::MaterialOverride> materialOverrides;
    };

    struct LodDiagnostics
    {
        bool bufferKnown = false;
        bool bufferValid = false;
        bool pendingUpload = false;
        std::uint32_t levelCount = 0;
        std::array<std::size_t, LodConfig::MaxLevels> levelTris{};
        std::array<std::size_t, LodConfig::MaxLevels> levelIndices{};
        std::size_t vertexCount = 0;
        const char* source = "none";
    };

    enum class LodBufferSource
    {
        Preview,
        Cache,
        Commit
    };

    StaticMeshRenderer() = default;
    ~StaticMeshRenderer();

    bool Create(VulkanDevice& device, client::asset::IAssetReader& assets, const std::string& modelPath);
    bool RecreatePipeline(VulkanDevice& device);
    void SetMainRenderPass(VkRenderPass renderPass);
    void SetLightingState(const LightingState& lighting) { m_lightingState = lighting; }
    void RenderInWorld(VulkanDevice& device, double timeSeconds, const WorldCamera& camera, const Instance& instance);
    void RenderBatchInWorld(VulkanDevice& device,
        double timeSeconds,
        const WorldCamera& camera,
        const std::vector<Instance>& instances);
    void RenderLodBatchInWorld(VulkanDevice& device,
        double timeSeconds,
        const WorldCamera& camera,
        const std::vector<Instance>& instances,
        const LodConfig& lodConfig,
        std::uint64_t configHash,
        std::uint32_t lodLevel);
    void RequestLodQualityBuild(const LodConfig& lodConfig, std::uint64_t configHash, std::uint32_t entityId);
    void Destroy();

    bool IsLoaded() const { return m_status == LoadStatus::LoadedStatic; }
    LoadStatus Status() const { return m_status; }
    bool IsSkinnedModel() const { return m_status == LoadStatus::UnsupportedSkinned; }
    bool HasPipeline() const { return m_pipeline != VK_NULL_HANDLE; }
    bool HasVertexBuffer() const { return m_vertexBuffer.buffer != VK_NULL_HANDLE; }
    bool HasIndexBuffer() const { return m_indexBuffer.buffer != VK_NULL_HANDLE; }
    bool HasTexture() const { return m_texture.image != VK_NULL_HANDLE && m_texture.view != VK_NULL_HANDLE; }
    bool HasDescriptors() const { return m_descriptorPool != VK_NULL_HANDLE && m_descriptorSetLayout != VK_NULL_HANDLE; }
    std::size_t VertexCount() const { return m_vertices.size(); }
    std::size_t IndexCount() const { return m_indices.size(); }
    std::size_t TriangleCount() const { return m_indices.size() / 3u; }
    std::size_t TriangleCountForLod(std::uint64_t configHash, std::uint32_t lodLevel) const;
    LodDiagnostics GetLodDiagnostics(std::uint64_t configHash) const;
    std::size_t DrawCount() const { return m_draws.size(); }
    std::uint32_t MaterialSlotCount() const { return std::max<std::uint32_t>(1u, m_materialSlotCount); }
    void DumpMaterialState(const char* entityName, const Instance& instance) const;
    std::uint32_t LastSubmittedDrawCalls() const { return m_lastSubmittedDrawCalls; }
    std::uint32_t LastSubmittedInstances() const { return m_lastSubmittedInstances; }
    std::uint32_t LastSubmittedIndexCount() const { return m_lastSubmittedIndexCount; }
    bool LastUsedFullResFallback() const { return m_lastUsedFullResFallback; }
    std::uint32_t LastMaterialUniformUpdates() const { return m_lastMaterialUniformUpdates; }
    std::uint32_t LastOverrideActiveDraws() const { return m_lastOverrideActiveDraws; }
    std::size_t LastInstanceBufferBytes() const { return m_lastInstanceBufferBytes; }
    bool LastInstanceBufferRebuilt() const { return m_lastInstanceBufferRebuilt; }
    const char* AlphaModeName() const { return m_alphaModeName.c_str(); }
    const std::array<float, 3>& BoundsMin() const { return m_boundsMin; }
    const std::array<float, 3>& BoundsMax() const { return m_boundsMax; }
    const std::string& TextureName() const { return m_texture.name; }

    static bool DetectSkinnedGltf(client::asset::IAssetReader& assets,
        const std::string& modelPath,
        bool& outSkinned,
        std::string* error);

private:
    static constexpr uint32_t kFramesInFlight = 2;
    static constexpr uint32_t kUniformSlots = 64;
    static constexpr uint32_t kInitialInstanceCapacity = 256;

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
        uint32_t materialSlot = 0;
        uint32_t vertexCount = 0;
    };

    struct LodMeshDraw
    {
        uint32_t firstIndex = 0;
        uint32_t indexCount = 0;
        uint32_t materialSlot = 0;
        uint32_t sourceDraw = 0;
    };

    struct LodIndexBuffer
    {
        Buffer buffer;
        std::vector<uint32_t> indices;
        std::vector<LodMeshDraw> draws;
        std::array<std::size_t, LodConfig::MaxLevels> triangles{};
        std::uint32_t levels = 1;
        bool generated = false;
        bool quality = false;
        LodBufferSource source = LodBufferSource::Preview;
    };

    struct LodCpuSet
    {
        std::uint64_t configHash = 0;
        LodConfig config;
        std::vector<uint32_t> indices;
        std::vector<LodMeshDraw> draws;
        std::array<std::size_t, LodConfig::MaxLevels> triangles{};
        std::uint32_t levels = 1;
        bool quality = false;
        bool writeCache = false;
        LodBufferSource source = LodBufferSource::Preview;
        std::uint32_t diagnosticEntityId = 0;
        double decimateMs = 0.0;
        double cacheWriteMs = 0.0;
    };

    struct LodBuildRequest
    {
        LodConfig config;
        std::uint64_t configHash = 0;
        bool quality = false;
        std::uint32_t diagnosticEntityId = 0;
    };

    struct PendingLodUpload
    {
        std::uint64_t configHash = 0;
        std::uint32_t diagnosticEntityId = 0;
        LodIndexBuffer lodSet;
        Buffer staging;
        VkCommandPool commandPool = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        double uploadMs = 0.0;
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
    bool EnsureLodBuffers(VulkanDevice& device, const LodConfig& config, std::uint64_t configHash, std::uint32_t entityId);
    bool ApplyPendingLodResult(VulkanDevice& device, std::uint64_t configHash);
    bool QueueLodUpload(VulkanDevice& device, std::uint64_t configHash, LodCpuSet&& cpuSet);
    bool PollPendingLodUploads(std::uint64_t configHash);
    bool HasPendingLodUpload(std::uint64_t configHash) const;
    void RequestLodBuild(const LodConfig& config, std::uint64_t configHash, bool quality, std::uint32_t entityId);
    void StartLodWorker();
    void StopLodWorker();
    void LodWorkerMain();
    bool EnsureLodPreviewProxy();
    LodCpuSet BuildLodCpuSet(const LodConfig& config, std::uint64_t configHash, bool quality, std::uint32_t entityId);
    bool LoadLodCpuCache(std::uint64_t configHash, LodCpuSet& out) const;
    void WriteLodCpuCache(const LodCpuSet& set) const;
    std::filesystem::path LodCachePath(const std::string& modelPath, std::uint64_t configHash) const;
    bool CreateTextures(VulkanDevice& device, const std::string& modelPath);
    bool CreateDescriptors();
    bool CreatePipeline(VulkanDevice& device);
    bool EnsureInstanceCapacity(VulkanDevice& device, uint32_t frameIndex, std::uint32_t requiredRecords);
    void UpdateInstanceDescriptorSets(uint32_t frameIndex);
    void DestroyPipeline();
    void DestroyBuffer(Buffer& buffer);
    void DestroyTexture(Texture& texture);
    void UpdateWorldUniform(uint32_t frameIndex,
        uint32_t uniformSlot,
        const WorldCamera& camera,
        const Instance& instance,
        double timeSeconds,
        uint32_t materialSlot);

    VkDevice m_device = VK_NULL_HANDLE;
    client::asset::IAssetReader* m_assets = nullptr;
    VkRenderPass m_mainRenderPass = VK_NULL_HANDLE;
    Buffer m_vertexBuffer;
    Buffer m_indexBuffer;
    std::array<std::array<Buffer, kUniformSlots>, kFramesInFlight> m_uniformBuffers{};
    std::array<Buffer, kFramesInFlight> m_instanceBuffers{};
    std::array<std::uint32_t, kFramesInFlight> m_instanceBufferCapacity{};
    Texture m_texture;
    Texture m_normalTexture;
    Texture m_ormTexture;
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    std::array<std::array<VkDescriptorSet, kUniformSlots>, kFramesInFlight> m_descriptorSets{};
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    std::vector<Vertex> m_vertices;
    std::vector<uint32_t> m_indices;
    std::vector<MeshDraw> m_draws;
    std::unordered_map<std::uint64_t, LodIndexBuffer> m_lodBuffers;
    std::vector<Buffer> m_retiredLodBuffers;
    std::vector<PendingLodUpload> m_pendingLodUploads;
    std::string m_modelPath;
    bool m_lodProxyBuilt = false;
    std::vector<std::uint32_t> m_lodProxyIndices;
    std::vector<MeshDraw> m_lodProxyDraws;
    mutable std::mutex m_lodMutex;
    std::condition_variable m_lodCv;
    std::thread m_lodWorker;
    bool m_lodWorkerStop = false;
    bool m_lodRequestPending = false;
    LodBuildRequest m_lodRequest;
    std::uint64_t m_lodRunningHash = 0;
    bool m_lodRunningQuality = false;
    std::uint32_t m_lodCoalescedDropped = 0;
    std::unordered_map<std::uint64_t, LodCpuSet> m_lodPendingResults;
    std::vector<MaterialDefaults> m_materialDefaults;
    std::uint32_t m_materialSlotCount = 1;
    std::string m_alphaModeName = "opaque";
    std::array<float, 3> m_boundsMin = {0.0f, 0.0f, 0.0f};
    std::array<float, 3> m_boundsMax = {0.0f, 0.0f, 0.0f};
    LightingState m_lightingState;
    LoadStatus m_status = LoadStatus::NotLoaded;
    uint32_t m_worldRenderFrameIndex = std::numeric_limits<uint32_t>::max();
    uint32_t m_worldUniformCursor = 0;
    std::uint32_t m_lastSubmittedDrawCalls = 0;
    std::uint32_t m_lastSubmittedInstances = 0;
    std::uint32_t m_lastSubmittedIndexCount = 0;
    bool m_lastUsedFullResFallback = false;
    std::uint32_t m_lastMaterialUniformUpdates = 0;
    std::uint32_t m_lastOverrideActiveDraws = 0;
    std::size_t m_lastInstanceBufferBytes = 0;
    bool m_lastInstanceBufferRebuilt = false;
};
