#pragma once

// StaticMeshRenderer — Phase-3A IXRHI-native production path.
//
// ZERO Vk* dependency (verified by grep): vertex/index/uniform/storage buffers,
// textures, samplers, shaders, 5 pipeline variants and bind groups are IXRHI
// objects; draws record through ixrhi::IXRHICommandList. Rendering behavior
// (passes, cull/depth/blend state, instancing, LOD fallback, outline pass) is
// unchanged.
//
// Frame contract: Render* takes the recording command list (owned frame list
// from the IXRHI frame context) and an IXRHIFrameInfo snapshot. Pipelines
// bake against m_targetPass (offscreen scene pass, borrowed) or the backend
// default (swapchain pass) when null.

#include "AssetDatabase.h"
#include "MapEditorTypes.h"
#include "WorldCamera.h"

#include "IXRHIBinding.h"
#include "IXRHIBuffer.h"
#include "IXRHICommandList.h"
#include "IXRHIDevice.h"
#include "IXRHIPipeline.h"
#include "IXRHIRenderPass.h"
#include "IXRHITexture.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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

    struct RgbaImage
    {
        std::string name;
        uint32_t width = 0;
        uint32_t height = 0;
        ixrhi::IXRHIFormat format = ixrhi::IXRHIFormat::R8G8B8A8Unorm;
        std::vector<uint8_t> pixels;
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
        bool unlit = false;
    };

    struct Instance
    {
        std::uint32_t entityId = 0;
        WorldVec3 position{};
        float rotation[3] = {0.0f, 0.0f, 0.0f};
        float scale[3] = {1.0f, 1.0f, 1.0f};
        std::array<float, 4> tint = {1.0f, 1.0f, 1.0f, 1.0f};
        bool selectedForOutline = false;
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

    // Per-instance GPU record (model/mvp, tint, material). Layout must match
    // the shader's storage block; also the per-frame CPU mirror element type.
    struct InstanceBlock
    {
        WorldMat4 mvp;
        WorldMat4 model;
        float tint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        float materialBaseColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        float materialParams[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        float materialEmissive[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float materialUv[4] = {1.0f, 1.0f, 0.0f, 0.0f};
        float materialAlpha[4] = {0.0f, 0.5f, 0.0f, 0.0f};
    };

    StaticMeshRenderer() = default;
    ~StaticMeshRenderer();

    bool Create(ixrhi::IXRHIDevice& rhi,
                client::asset::IAssetReader& assets,
                const std::string& modelPath);
    bool RecreatePipeline(ixrhi::IXRHIDevice& rhi);
    // Borrowed target pass (offscreen scene pass); null = backend default.
    void SetTargetPass(const ixrhi::IXRHIRenderPass* pass) { m_targetPass = pass; }
    void SetLightingState(const LightingState& lighting) { m_lightingState = lighting; }
    void RenderInWorld(ixrhi::IXRHICommandList& cmd,
        const ixrhi::IXRHIFrameInfo& frame,
        double timeSeconds,
        const WorldCamera& camera,
        const Instance& instance,
        std::uint32_t targetWidth = 0,
        std::uint32_t targetHeight = 0);
    void RenderBatchInWorld(ixrhi::IXRHICommandList& cmd,
        const ixrhi::IXRHIFrameInfo& frame,
        double timeSeconds,
        const WorldCamera& camera,
        const std::vector<Instance>& instances,
        std::uint32_t targetWidth = 0,
        std::uint32_t targetHeight = 0);
    void RenderLodBatchInWorld(ixrhi::IXRHICommandList& cmd,
        const ixrhi::IXRHIFrameInfo& frame,
        double timeSeconds,
        const WorldCamera& camera,
        const std::vector<Instance>& instances,
        const LodConfig& lodConfig,
        std::uint64_t configHash,
        std::uint32_t lodLevel,
        std::uint32_t targetWidth = 0,
        std::uint32_t targetHeight = 0);
    void RequestLodQualityBuild(const LodConfig& lodConfig, std::uint64_t configHash, std::uint32_t entityId);
    void Destroy();

    bool IsLoaded() const { return m_status == LoadStatus::LoadedStatic; }
    LoadStatus Status() const { return m_status; }
    bool IsSkinnedModel() const { return m_status == LoadStatus::UnsupportedSkinned; }
    bool HasPipeline() const { return m_pipeline != nullptr; }
    bool HasVertexBuffer() const { return m_vertexBuffer != nullptr; }
    bool HasIndexBuffer() const { return m_indexBuffer != nullptr; }
    bool HasTexture() const { return m_texture.image != nullptr; }
    bool HasDescriptors() const { return m_bindGroup != nullptr && m_bindLayout != nullptr; }
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
    bool CopyPhysicsMesh(std::vector<std::array<float, 3>>& outVertices, std::vector<std::uint32_t>& outIndices) const;

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
        std::shared_ptr<ixrhi::IXRHIBuffer> buffer;
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
        LodIndexBuffer lodSet; // buffer filled by Take() on completion
        std::unique_ptr<ixrhi::IXRHIBufferUpload> upload;
        double uploadMs = 0.0;
    };

    struct Texture
    {
        std::shared_ptr<ixrhi::IXRHITexture> image;
        std::shared_ptr<ixrhi::IXRHISampler> sampler;
        ixrhi::IXRHIFormat format = ixrhi::IXRHIFormat::Undefined;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t mipLevels = 0;
        std::string name;
    };

    struct MaterialTexture
    {
        std::shared_ptr<ixrhi::IXRHITexture> texture;
        std::shared_ptr<ixrhi::IXRHISampler> sampler;
    };

    struct MaterialTextureViews
    {
        MaterialTexture baseColor;
        MaterialTexture normal;
        MaterialTexture orm;
        std::string resolvedMaterial = "gltf_baked";
        std::string baseColorTextureGuid = "EMPTY";
        std::string alphaMode = "OPAQUE";
        float alphaCutoff = 0.5f;
        const char* fragmentShaderAlphaPath = "none";
        bool unlit = false;
    };

    struct LastMaterialBinding
    {
        std::uint32_t sourceSubmesh = 0;
        std::uint32_t materialSlot = 0;
        std::uint32_t bindSlot = 0;
        std::string baseColorTexture;
        std::string normalTexture;
        std::string ormTexture;
        std::string resolvedMaterial = "gltf_baked";
        std::string baseColorTextureGuid = "EMPTY";
        std::string alphaMode = "OPAQUE";
        float alphaCutoff = 0.5f;
        const char* fragmentShaderAlphaPath = "none";
        bool unlit = false;
        std::string pipelineName;
        bool boundBeforeDraw = false;
    };

    bool LoadStaticGltfMesh(const std::string& modelPath);
    bool LoadStaticFbxMesh(const std::string& modelPath);
    bool LoadBuiltinPrimitiveMesh(const std::string& modelPath);
    bool CreateBuffers(ixrhi::IXRHIDevice& rhi);
    bool EnsureLodBuffers(const LodConfig& config, std::uint64_t configHash, std::uint32_t entityId);
    bool ApplyPendingLodResult(std::uint64_t configHash);
    bool QueueLodUpload(std::uint64_t configHash, LodCpuSet&& cpuSet);
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
    bool CreateTextures(ixrhi::IXRHIDevice& rhi, const std::string& modelPath);
    bool UploadTexture(ixrhi::IXRHIDevice& rhi, const RgbaImage& source, Texture& texture);
    const Texture* EnsureMaterialTexture(ixrhi::IXRHIDevice& rhi,
        const std::optional<Guid>& guid,
        ixrhi::IXRHIFormat format,
        const char* role);
    MaterialTextureViews ResolveMaterialTextureViews(ixrhi::IXRHIDevice& rhi,
        const Instance& instance,
        std::uint32_t materialSlot);
    void UpdateMaterialTextureDescriptors(uint32_t frameIndex,
        uint32_t uniformSlot,
        const MaterialTextureViews& textures);
    bool CreateBindGroup(ixrhi::IXRHIDevice& rhi);
    bool CreatePipeline(ixrhi::IXRHIDevice& rhi);
    bool EnsureInstanceCapacity(ixrhi::IXRHIDevice& rhi, uint32_t frameIndex, std::uint32_t requiredRecords);
    void UpdateInstanceDescriptorSets(uint32_t frameIndex);
    void DestroyPipeline();
    void UpdateWorldUniform(uint32_t frameIndex,
        uint32_t uniformSlot,
        const WorldCamera& camera,
        const Instance& instance,
        double timeSeconds,
        uint32_t materialSlot);

    ixrhi::IXRHIDevice* m_rhi = nullptr;
    const ixrhi::IXRHIRenderPass* m_targetPass = nullptr; // borrowed (frame owner)
    client::asset::IAssetReader* m_assets = nullptr;
    std::shared_ptr<ixrhi::IXRHIBuffer> m_vertexBuffer;
    std::shared_ptr<ixrhi::IXRHIBuffer> m_indexBuffer;
    std::array<std::array<std::shared_ptr<ixrhi::IXRHIBuffer>, kUniformSlots>, kFramesInFlight> m_uniformBuffers{};
    std::array<std::shared_ptr<ixrhi::IXRHIBuffer>, kFramesInFlight> m_instanceBuffers{};
    std::array<std::uint32_t, kFramesInFlight> m_instanceBufferCapacity{};
    // CPU mirror of instance records per frame (lets growth preserve already
    // written records without GPU readback; same bytes as the old map-copy).
    std::array<std::vector<InstanceBlock>, kFramesInFlight> m_instanceMirror{};
    Texture m_texture;
    Texture m_normalTexture;
    Texture m_ormTexture;
    std::unordered_map<std::string, Texture> m_materialTextureCache;
    std::unordered_set<std::string> m_failedMaterialTextureKeys;
    std::vector<LastMaterialBinding> m_lastMaterialBindings;
    std::unique_ptr<ixrhi::IXRHIBindGroupLayout> m_bindLayout;
    std::unique_ptr<ixrhi::IXRHIBindGroup> m_bindGroup;
    // Pipeline variants (same 5 as before; mask reuses the lit fragment shader):
    // opaque, alpha-mask, unlit, unlit alpha-mask, selection outline.
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_pipeline;
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_maskPipeline;
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_unlitPipeline;
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_unlitMaskPipeline;
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_outlinePipeline;
    std::vector<Vertex> m_vertices;
    std::vector<uint32_t> m_indices;
    std::vector<MeshDraw> m_draws;
    std::unordered_map<std::uint64_t, LodIndexBuffer> m_lodBuffers;
    std::vector<std::shared_ptr<ixrhi::IXRHIBuffer>> m_retiredLodBuffers;
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
    // Per-frame write cursor into the instance storage buffer. The same renderer can be
    // drawn multiple times per frame (Scene-view batch + Game-view per-entity, into one
    // command buffer); each call appends its instance blocks here instead of overwriting
    // offset 0, so earlier draws still read their own transforms at GPU execute.
    uint32_t m_worldInstanceCursor = 0;
    std::uint32_t m_lastSubmittedDrawCalls = 0;
    std::uint32_t m_lastSubmittedInstances = 0;
    std::uint32_t m_lastSubmittedIndexCount = 0;
    bool m_lastUsedFullResFallback = false;
    std::uint32_t m_lastMaterialUniformUpdates = 0;
    std::uint32_t m_lastOverrideActiveDraws = 0;
    std::size_t m_lastInstanceBufferBytes = 0;
    bool m_lastInstanceBufferRebuilt = false;
};
