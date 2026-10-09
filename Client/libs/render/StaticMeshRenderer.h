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
#include "SunShadow.h"
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
        float uvTiling[2] = {1.0f, 1.0f}; // the material asset's UV transform (overrides replace it)
        float uvOffset[2] = {0.0f, 0.0f};
    };

    struct PreparedInstance;

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
        // Optional: what this instance's records hold that no view changes (PrepareInstance), kept
        // by the caller while the instance is unchanged. Draws then skip building its matrix and
        // looking up its materials; one made by another renderer, or before the material assets
        // changed, is not used.
        const PreparedInstance* prepared = nullptr;
    };
    // The instances of one batch draw. Pointers: the caller keeps its instances (and their material
    // lists) where they are instead of copying them into every batch of every pass.
    using InstanceList = std::vector<const Instance*>;

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

    struct PreparedInstance
    {
        const StaticMeshRenderer* renderer = nullptr;  // whose material slots these are
        std::uint64_t materialRevision = 0;            // MaterialAssetManager::Revision() read at
        std::vector<InstanceBlock> slots;              // per material slot; mvp is the view's
        bool hasTransparentDraws = false;
    };

    StaticMeshRenderer();
    ~StaticMeshRenderer();

    // The instance's view-independent records (see Instance::prepared). Made again when the instance
    // or MaterialAssetManager::Revision() changes; cheap to keep, about 200 bytes per material slot.
    // True when they differ from what out held (a material loaded for another model changes none).
    bool PrepareInstance(const Instance& instance, PreparedInstance& out) const;

    bool Create(ixrhi::IXRHIDevice& rhi,
                client::asset::IAssetReader& assets,
                const std::string& modelPath);
    // Create in two halves, so a model loads off the render thread: LoadCpu on any thread (parses
    // the model and decodes its textures, touching no device; false: not a static model or failed,
    // see Status()), then FinishGpu on the render thread (buffers, textures, descriptors, pipelines).
    // deferUploads: the vertex and index buffers and the textures are made empty and their data
    // staged instead of uploaded and waited for (a model finished mid-game waited for the frames in
    // flight); RecordPendingUploads copies it in, and the model draws nothing until it has.
    bool LoadCpu(client::asset::IAssetReader& assets, const std::string& modelPath);
    bool FinishGpu(ixrhi::IXRHIDevice& rhi, bool deferUploads = false);
    bool HasPendingUploads() const { return m_pendingUploads != nullptr; }
    // Once a frame, outside render passes, before the model is drawn in it: the staged uploads
    // (FinishGpu's, once), and the material textures decoded since (made and copied in; usable by
    // the frame's draws). Staging goes once the frame that copied it is done.
    void RecordPendingUploads(ixrhi::IXRHICommandList& cmd, const ixrhi::IXRHIFrameInfo& frame);
    bool RecreatePipeline(ixrhi::IXRHIDevice& rhi);
    // Borrowed target pass (offscreen scene pass); null = backend default.
    void SetTargetPass(const ixrhi::IXRHIRenderPass* pass) { m_targetPass = pass; }
    void SetLightingState(const LightingState& lighting) { m_lightingState = lighting; }
    // The sun shadow cascades the lit draws sample. Set before the first draw (the map is bound in
    // every draw) and each frame (cascades follow the camera).
    void SetSunShadow(const SunShadowReceive& shadow);
    // These instances into one sun shadow cascade, depth only (alpha-masked materials keep their
    // cut-outs). Call inside the cascade's render pass (viewport set by its owner). shadowTexelMeters:
    // the cascade's texel size; each instance is drawn at the coarsest shadow detail level that stays
    // within half a texel at its scale (BuildLods; 0: the model's own triangles).
    void RenderShadowCasters(ixrhi::IXRHICommandList& cmd,
        const ixrhi::IXRHIFrameInfo& frame,
        const WorldMat4& lightViewProj,
        const InstanceList& instances,
        const ixrhi::IXRHIRenderPass* shadowPass,
        float shadowTexelMeters = 0.0f);
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
        const InstanceList& instances,
        std::uint32_t targetWidth = 0,
        std::uint32_t targetHeight = 0);
    void RenderLodBatchInWorld(ixrhi::IXRHICommandList& cmd,
        const ixrhi::IXRHIFrameInfo& frame,
        double timeSeconds,
        const WorldCamera& camera,
        const InstanceList& instances,
        const LodConfig& lodConfig,
        std::uint64_t configHash,
        std::uint32_t lodLevel,
        std::uint32_t targetWidth = 0,
        std::uint32_t targetHeight = 0);
    // Whether any of the instance's draws has an alpha-blended (Blend) material. The batches above
    // leave those out: RenderTransparentInWorld draws them, in the frame's back-to-front order.
    bool HasTransparentDraws(const Instance& instance) const;
    // The instance's alpha-blended draws, over what is already drawn: depth tested but not written,
    // back faces before front faces. Call after the opaque scene, the farthest instance first.
    void RenderTransparentInWorld(ixrhi::IXRHICommandList& cmd,
        const ixrhi::IXRHIFrameInfo& frame,
        double timeSeconds,
        const WorldCamera& camera,
        const Instance& instance,
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
    bool HasDescriptors() const { return !m_bindPages.empty() && m_bindLayout != nullptr; }
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
    // Bind sets per page and frame in flight (see BindPage), and the most pages a renderer makes.
    static constexpr uint32_t kUniformSlots = 64;
    static constexpr uint32_t kMaxBindPages = 64;
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
        std::optional<Guid> baseColorTextureGuid;  // the material's base colour texture (diagnostics)
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
        std::optional<Guid> baseColorTextureGuid;  // the material's base colour texture (diagnostics)
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
    void DecodeTextures(const std::string& modelPath);  // into m_decodedTextures (LoadCpu)
    // Detail levels (LoadCpu): each opaque submesh simplified to within kLodErrors[level] of its
    // surface, in model units, each level from the one before; a level that takes off less than a
    // quarter of the one before is that one again. Two sets of them: the sun shadow's, of the
    // surface welded across its uv and normal seams (a depth pass reads positions only), and the
    // view's, the seams kept (its texture and lighting stay). For the shadow, an alpha-masked
    // submesh of many small separate pieces (leaf cards) has the coarsest level too:
    // kShadowCardKeep of the pieces, each grown about its centre to cover kShadowCardCover of the
    // area of those left out (the shadow's density then matches the full crown's: the grown pieces
    // overlap each other less). The view draws an instance at the coarsest level whose error, at its
    // scale and nearest depth, projects under kViewLodMaxErrorPixels.
    static constexpr std::size_t kLodLevels = 3;
    static constexpr std::array<float, kLodLevels> kLodErrors = {0.005f, 0.016f, 0.064f};
    static constexpr float kShadowCardKeep = 0.25f;
    static constexpr float kShadowCardCover = 0.7f;
    static constexpr float kViewLodMaxErrorPixels = 0.5f;
    void BuildLods();
    bool UploadDecodedTextures(ixrhi::IXRHIDevice& rhi);
    // staging: when given, the texture is made empty and its texels go there (a deferred upload).
    bool UploadTexture(ixrhi::IXRHIDevice& rhi, const RgbaImage& source, Texture& texture,
                       std::shared_ptr<ixrhi::IXRHIBuffer>* staging = nullptr);
    // FinishGpu's deferred uploads: the data, and the frame that recorded the copies.
    struct PendingUploads
    {
        std::shared_ptr<ixrhi::IXRHIBuffer> vertexStaging;
        std::shared_ptr<ixrhi::IXRHIBuffer> indexStaging;
        std::shared_ptr<ixrhi::IXRHIBuffer> lodIndexStaging;
        std::shared_ptr<ixrhi::IXRHIBuffer> shadowCardVertexStaging;
        std::array<std::shared_ptr<ixrhi::IXRHIBuffer>, 3> textureStaging{};  // diffuse, normal, orm
        std::uint64_t recordedFrame = std::numeric_limits<std::uint64_t>::max();
    };
    // Whether the model's data is on the GPU for this frame's draws (copied, or the copies recorded).
    bool UploadsRecorded() const
    {
        return !m_pendingUploads || m_pendingUploads->recordedFrame != std::numeric_limits<std::uint64_t>::max();
    }
    const Texture* EnsureMaterialTexture(ixrhi::IXRHIDevice& rhi,
        const std::optional<Guid>& guid,
        ixrhi::IXRHIFormat format,
        const char* role);
    MaterialTextureViews ResolveMaterialTextureViews(ixrhi::IXRHIDevice& rhi,
        const Instance& instance,
        std::uint32_t materialSlot);
    // Textures/samplers each bind set's descriptors point at (baseColor, normal, orm), so a draw
    // re-binding the same material skips the descriptor writes. A bind group keeps every resource
    // ever bound to it alive, so a stored pointer cannot be reused by another texture meanwhile.
    struct BoundSlotTexture
    {
        const ixrhi::IXRHITexture* texture = nullptr;
        const ixrhi::IXRHISampler* sampler = nullptr;
    };
    // Every draw of a frame binds a set of its own: a set must not change once the frame's command
    // list has bound it. Sets come in pages of kUniformSlots per frame in flight; a frame drawing more
    // than the pages hold adds one, kept for the frames after.
    struct BindPage
    {
        std::unique_ptr<ixrhi::IXRHIBindGroup> group;  // set = frame index * kUniformSlots + slot
        std::shared_ptr<ixrhi::IXRHIBuffer> uniforms;  // every set's UniformBlock (kUniformStride apart)
        std::array<std::array<BoundSlotTexture, 3>, kFramesInFlight * kUniformSlots> textures{};
        // The instance buffer binding 4 points at: re-pointed when the set is next taken, after a
        // frame grew the buffer (never while a frame that bound the set may still run).
        std::array<const ixrhi::IXRHIBuffer*, kFramesInFlight * kUniformSlots> instances{};
    };
    struct BindSlot
    {
        BindPage* page = nullptr;
        std::uint32_t set = 0;
        std::uint32_t id = 0;  // page * sets per page + set (diagnostics)
    };
    // Starts this renderer's set and instance cursors over when the frame is a new one.
    void BeginFrameSlots(const ixrhi::IXRHIFrameInfo& frame);
    // The frame's next set, its instance binding current; none past kMaxBindPages (logged once).
    std::optional<BindSlot> NextBindSlot(uint32_t frameIndex);
    BindPage* AddBindPage(ixrhi::IXRHIDevice& rhi);
    void UpdateMaterialTextureDescriptors(const BindSlot& slot, const MaterialTextureViews& textures);
    bool CreateBindGroup(ixrhi::IXRHIDevice& rhi);
    bool CreatePipeline(ixrhi::IXRHIDevice& rhi);
    bool CreateShadowPipelines(ixrhi::IXRHIDevice& rhi, const ixrhi::IXRHIRenderPass* shadowPass);
    // The instance's record for one material slot in the view (its prepared one when usable).
    void FillInstanceBlock(const WorldMat4& viewProjection,
        const Instance& instance,
        std::uint32_t materialSlot,
        InstanceBlock& out) const;
    bool PreparedUsable(const Instance& instance) const;
    // FillInstanceBlock for every instance into out[0, instances.size()): those with a usable
    // prepared record in parallel (large batches), the others (their materials are looked up, which
    // is not thread-safe) on this thread after.
    void FillInstanceBlocks(const WorldMat4& viewProjection,
        const InstanceList& instances,
        std::uint32_t materialSlot,
        InstanceBlock* out) const;
    // Appends instance blocks at this frame's cursor; returns the first record's index.
    std::optional<std::uint32_t> AppendInstanceBlocks(uint32_t frameIndex, const std::vector<InstanceBlock>& blocks);
    bool EnsureInstanceCapacity(ixrhi::IXRHIDevice& rhi, uint32_t frameIndex, std::uint32_t requiredRecords);
    void DestroyPipeline();
    void UpdateWorldUniform(const BindSlot& slot,
        const WorldCamera& camera,
        const Instance& instance,
        double timeSeconds,
        uint32_t materialSlot);

    ixrhi::IXRHIDevice* m_rhi = nullptr;
    const ixrhi::IXRHIRenderPass* m_targetPass = nullptr; // borrowed (frame owner)
    client::asset::IAssetReader* m_assets = nullptr;
    std::shared_ptr<ixrhi::IXRHIBuffer> m_vertexBuffer;
    std::unique_ptr<PendingUploads> m_pendingUploads;
    bool m_deferUploads = false;  // FinishGpu's, for CreateBuffers and UploadDecodedTextures
    std::shared_ptr<ixrhi::IXRHIBuffer> m_indexBuffer;
    // The detail levels' indices (BuildLods), and per level and draw its span in them (indexCount 0:
    // the draw's own indices; none for a model too small or without opaque submeshes), the shadow's
    // and the view's. The leaf cards' spans in them index their own vertices. None for a model with
    // nothing to take off.
    std::vector<std::uint32_t> m_lodIndices;
    std::array<std::vector<MeshDraw>, kLodLevels> m_shadowLodDraws;
    std::array<std::vector<MeshDraw>, kLodLevels> m_viewLodDraws;
    float m_modelRadius = 0.0f;  // the farthest point of the bounds from the model's origin
    std::vector<Vertex> m_shadowCardVertices;
    std::vector<MeshDraw> m_shadowCardDraws;
    std::shared_ptr<ixrhi::IXRHIBuffer> m_lodIndexBuffer;
    std::shared_ptr<ixrhi::IXRHIBuffer> m_shadowCardVertexBuffer;
    // Grown when a frame appends more records than it holds. The records a frame wrote before stay
    // in the old buffer, which the sets bound to it keep alive.
    std::array<std::shared_ptr<ixrhi::IXRHIBuffer>, kFramesInFlight> m_instanceBuffers{};
    std::array<std::uint32_t, kFramesInFlight> m_instanceBufferCapacity{};
    Texture m_texture;
    Texture m_normalTexture;
    Texture m_ormTexture;
    // Material textures by (role, texture GUID). Looked up for every draw of every pass, so the key
    // builds no strings.
    struct MaterialTextureKey
    {
        std::uint8_t role = 0;  // see MaterialTextureRoleIndex
        Guid guid;
        bool operator==(const MaterialTextureKey& other) const { return role == other.role && guid == other.guid; }
    };
    struct MaterialTextureKeyHash
    {
        std::size_t operator()(const MaterialTextureKey& key) const noexcept
        {
            return std::hash<Guid>{}(key.guid) ^ (static_cast<std::size_t>(key.role) * 0x9e3779b97f4a7c15ull);
        }
    };
    std::unordered_map<MaterialTextureKey, Texture, MaterialTextureKeyHash> m_materialTextureCache;
    // Material textures on their way: decoded on a loading thread (EnsureMaterialTexture starts it, at
    // draw time), then made and copied in by RecordPendingUploads; the model's own textures stand in.
    struct MaterialTextureLoad;
    std::unordered_map<MaterialTextureKey, std::unique_ptr<MaterialTextureLoad>, MaterialTextureKeyHash>
        m_materialTextureLoads;
    // Their staging, kept until the frame that copied it is done.
    struct RetiredStaging
    {
        std::shared_ptr<ixrhi::IXRHIBuffer> buffer;
        std::uint64_t frame = 0;
    };
    std::vector<RetiredStaging> m_materialTextureStaging;
    // A texture's copy from its staging, with the layout changes around it (outside render passes).
    static void RecordTextureUpload(ixrhi::IXRHICommandList& cmd, const Texture& texture, const ixrhi::IXRHIBuffer& staging);
    std::unordered_set<MaterialTextureKey, MaterialTextureKeyHash> m_failedMaterialTextureKeys;
    std::vector<LastMaterialBinding> m_lastMaterialBindings;
    std::unique_ptr<ixrhi::IXRHIBindGroupLayout> m_bindLayout;
    std::vector<std::unique_ptr<BindPage>> m_bindPages;
    bool m_loggedBindPagesFull = false;
    // Pipeline variants (same 5 as before; mask reuses the lit fragment shader):
    // opaque, alpha-mask, unlit, unlit alpha-mask, selection outline.
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_pipeline;
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_maskPipeline;
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_unlitPipeline;
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_unlitMaskPipeline;
    // The alpha-masked draws in two steps (RenderLodBatchInWorld): their depth, alpha-tested by a shader
    // that does nothing else and writes no colour; then their colour where that depth is theirs, not
    // written (so the hardware rejects a hidden texel before shading it).
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_maskDepthPipeline;
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_maskOnDepthPipeline;
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_unlitMaskOnDepthPipeline;
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_outlinePipeline;
    // Alpha-blended (Blend materials): [lit, unlit] x [back faces, front faces].
    std::array<std::array<std::unique_ptr<ixrhi::IXRHIGraphicsPipeline>, 2>, 2> m_blendPipelines;
    // Sun shadow pass (depth only), baked against the cascades' pass: opaque, and alpha-masked.
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_shadowPipeline;
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_shadowMaskPipeline;
    const ixrhi::IXRHIRenderPass* m_shadowPass = nullptr;
    bool m_shadowPipelinesFailed = false;
    SunShadowReceive m_sunShadow;
    const ixrhi::IXRHITexture* m_boundSunShadowTexture = nullptr;  // what binding 5 holds in every set
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
    std::array<RgbaImage, 3> m_decodedTextures;  // diffuse, normal, orm: LoadCpu -> FinishGpu
    // A glTF's parsed asset, LoadCpu -> FinishGpu: registering its materials (the material manager
    // and the asset database are not thread-safe) is the render thread's work.
    struct PendingMaterialImport;
    std::unique_ptr<PendingMaterialImport> m_pendingMaterialImport;
    std::uint32_t m_materialSlotCount = 1;
    std::string m_alphaModeName = "opaque";
    std::array<float, 3> m_boundsMin = {0.0f, 0.0f, 0.0f};
    std::array<float, 3> m_boundsMax = {0.0f, 0.0f, 0.0f};
    LightingState m_lightingState;
    LoadStatus m_status = LoadStatus::NotLoaded;
    // The frame the cursors below count in (its frame number: a renderer drawn every other frame
    // meets the same frame index again without the frame between).
    std::uint64_t m_worldRenderFrameNumber = std::numeric_limits<std::uint64_t>::max();
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
