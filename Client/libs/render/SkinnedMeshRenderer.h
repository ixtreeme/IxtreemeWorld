#pragma once

// SkinnedMeshRenderer — Phase-3D IXRHI-native migration (compute + graphics).
//
// ZERO Vk* dependency: rest/palette/output/index/uniform buffers, textures,
// samplers, shaders, compute + graphics pipelines and bind groups are IXRHI
// objects; skinning dispatches and draws record through ixrhi::IXRHICommandList.
//
// Skinning model (preserved exactly, NOT redesigned):
//   CPU (Ozz, animation module): sampling, hierarchy evaluation (LocalToModel),
//       bone palette generation (model * inverse-bind, row-major Mat4).
//   GPU (compute): linear-blend skinning of rest vertices by palette.
//   Graphics consumes the compute output buffer as its vertex buffer.
// Ozz types never cross into IXRHI.
//
// Frame contract: Skin*/Render* take the recording command list (owned frame
// list from the IXRHI frame context) and an IXRHIFrameInfo snapshot. Compute
// dispatches record into the same graphics command buffer in the pre-pass
// (same queue, no async compute — parity). Per-frame/per-slot palette + output
// buffers preserve the frames-in-flight hazard discipline.

#include "WorldCamera.h"
#include "MapEditorTypes.h"
#include "SunShadow.h"

#include "IXRHIBinding.h"
#include "IXRHIBuffer.h"
#include "IXRHICommandList.h"
#include "IXRHIDevice.h"
#include "IXRHIPipeline.h"
#include "IXRHIRenderPass.h"
#include "IXRHITexture.h"

#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/base/maths/simd_math.h>
#include <ozz/base/maths/soa_transform.h>
#include <ozz/base/span.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace client::asset {
class IAssetReader;
}

namespace ozz::animation {
class Skeleton;
}

class SkinnedMeshRenderer
{
public:
    SkinnedMeshRenderer();
    ~SkinnedMeshRenderer();

    enum class MotionState : uint32_t
    {
        Idle = 0,
        Walk = 1,
        Run = 2
    };

    bool Create(ixrhi::IXRHIDevice& rhi,
                client::asset::IAssetReader& assets,
                const std::string& modelPath);
    // Create in two halves, so a model loads off the render thread: LoadCpu on any thread (the mesh,
    // its skeleton and clips, a CPU skin for the bounds, its textures decoded; touches no device),
    // then FinishGpu on the render thread (buffers, compute skinning, textures, descriptors,
    // pipelines).
    bool LoadCpu(client::asset::IAssetReader& assets, const std::string& modelPath);
    bool FinishGpu(ixrhi::IXRHIDevice& rhi);
    bool RecreatePipeline(ixrhi::IXRHIDevice& rhi);
    // Borrowed IXRHI pass token; null = backend default (swapchain pass).
    void SetTargetPass(const ixrhi::IXRHIRenderPass* pass) { m_targetPass = pass; }
    void Skin(ixrhi::IXRHICommandList& cmd,
              const ixrhi::IXRHIFrameInfo& frame,
              double timeSeconds);
    void SkinInstance(ixrhi::IXRHICommandList& cmd,
                      const ixrhi::IXRHIFrameInfo& frame,
                      uint32_t skinSlot,
                      MotionState state,
                      float animTimeSeconds);
    // Pose-injection entry point: skin one instance from an EXTERNALLY computed local pose
    // (e.g. the animator's blended ozz output) instead of selecting a built-in MotionState
    // clip. `localPose` must hold exactly NumSoaJoints() SoaTransforms for this skeleton.
    void SkinInstanceFromPose(ixrhi::IXRHICommandList& cmd,
                              const ixrhi::IXRHIFrameInfo& frame,
                              uint32_t skinSlot,
                              ozz::span<const ozz::math::SoaTransform> localPose);

    // Skinning in two halves, so that many instances' poses are worked out in parallel. On the
    // recording thread first: ReserveSkinSlot for each slot, and ReserveParallelPalettes for the
    // threads that will prepare them (ixjobs::JobSystem::CurrentWorker() indexes the scratch). Then
    // PreparePalette from any of those threads, each instance on one thread: it writes the frame's
    // bone palette of the instance's slots (Scene and Game view: the same pose). Then RecordSkin per
    // slot, on the recording thread, outside any render pass.
    bool ReserveSkinSlot(uint32_t skinSlot) { return EnsureSkinSlot(skinSlot); }
    void ReserveParallelPalettes(uint32_t threads);
    // An empty pose: the built-in clip for the state, at animTimeSeconds.
    bool PreparePalette(const ixrhi::IXRHIFrameInfo& frame,
                        const uint32_t* skinSlots,
                        uint32_t slotCount,
                        ozz::span<const ozz::math::SoaTransform> pose,
                        MotionState state,
                        float animTimeSeconds);
    void RecordSkin(ixrhi::IXRHICommandList& cmd, const ixrhi::IXRHIFrameInfo& frame, uint32_t skinSlot);
    // Many slots' dispatches; withoutBarrier: the caller issues one barrier for all of them after
    // (BufferMemoryBarrier ShaderWrite -> VertexRead) before any draw reads them.
    void RecordSkins(ixrhi::IXRHICommandList& cmd,
        const ixrhi::IXRHIFrameInfo& frame,
        const uint32_t* skinSlots,
        uint32_t slotCount,
        bool withoutBarrier);
    // Many skinned instances into one sun shadow cascade (RenderShadowCaster, with the state set once).
    struct ShadowCasterInstance
    {
        WorldVec3 position{};
        float yawRadians = 0.0f;
        uint32_t skinSlot = 0;
        std::array<float, 3> scale = {1.0f, 1.0f, 1.0f};
    };
    void RenderShadowCasters(ixrhi::IXRHICommandList& cmd,
        const ixrhi::IXRHIFrameInfo& frame,
        const WorldMat4& lightViewProj,
        const ixrhi::IXRHIRenderPass* shadowPass,
        const ShadowCasterInstance* instances,
        std::size_t count);
    // Skeleton accessors for the animation layer (clip retargeting, rest-pose fallback).
    // Return null/empty when no skeleton is loaded yet.
    const ozz::animation::Skeleton* Skeleton() const;
    ozz::span<const ozz::math::SoaTransform> RestPoseLocals() const;
    std::uint32_t NumJoints() const;
    // The model's largest bounding-box side in its own units (before the entity's scale).
    float LocalExtent() const
    {
        return std::max({m_bounds.max[0] - m_bounds.min[0], m_bounds.max[1] - m_bounds.min[1],
            m_bounds.max[2] - m_bounds.min[2]});
    }
    std::uint32_t NumSoaJoints() const;
    // Ordered joint names of the loaded skeleton (empty if none) — the retarget key for clips.
    std::vector<std::string> JointNames() const;
    void Render(ixrhi::IXRHICommandList& cmd,
                const ixrhi::IXRHIFrameInfo& frame,
                double timeSeconds);
    void RenderInWorld(ixrhi::IXRHICommandList& cmd,
        const ixrhi::IXRHIFrameInfo& frame,
        double timeSeconds,
        const WorldCamera& camera,
        WorldVec3 position,
        float yawRadians,
        uint32_t skinSlot = 0,
        std::array<float, 4> tint = {1.0f, 1.0f, 1.0f, 1.0f},
        std::uint32_t targetWidth = 0,
        std::uint32_t targetHeight = 0,
        std::array<float, 3> scale = {1.0f, 1.0f, 1.0f});
    void RenderInWorldReflection(ixrhi::IXRHICommandList& cmd,
        const ixrhi::IXRHIFrameInfo& frame,
        const WorldCamera& camera,
        std::uint32_t targetWidth,
        std::uint32_t targetHeight,
        const ixrhi::IXRHIRenderPass* renderPass,
        float waterLevelY,
        WorldVec3 position,
        float yawRadians,
        uint32_t skinSlot = 0,
        std::array<float, 4> tint = {1.0f, 1.0f, 1.0f, 1.0f},
        std::array<float, 3> scale = {1.0f, 1.0f, 1.0f});
    void SetLightingState(const LightingState& lighting) { m_lightingState = lighting; }
    // The sun shadow cascades the draws sample. Set before the first draw (the map is bound in every
    // draw) and each frame (cascades follow the camera).
    void SetSunShadow(const SunShadowReceive& shadow);
    // One skinned instance (already skinned into skinSlot this frame) into a sun shadow cascade,
    // depth only. Call inside the cascade's render pass (viewport set by its owner).
    void RenderShadowCaster(ixrhi::IXRHICommandList& cmd,
        const ixrhi::IXRHIFrameInfo& frame,
        const WorldMat4& lightViewProj,
        const ixrhi::IXRHIRenderPass* shadowPass,
        WorldVec3 position,
        float yawRadians,
        uint32_t skinSlot,
        std::array<float, 3> scale);
    void SetMotionState(MotionState state);
    float GroundOffsetY() const;
    std::uint32_t MaterialSlotCount() const { return std::max<std::uint32_t>(1u, static_cast<std::uint32_t>(m_draws.size())); }
    // How many instances of this model a frame can skin (slots past the first page are made on use).
    static constexpr uint32_t MaxSkinSlots() { return kSkinSlots * kMaxSkinPages; }
    void Destroy();

    struct Vertex
    {
        float position[3];
        float normal[3];
        float uv[2];
    };

    struct SourceVertex
    {
        float position[3];
        uint8_t boneWeights[4];
        uint8_t boneIndices[4];
        float normal[3];
        float uv[2];
    };
    static_assert(sizeof(SourceVertex) == 40, "Skinned source vertex layout must stay 40 bytes");

private:
    static constexpr uint32_t kFramesInFlight = 2;
    static constexpr uint32_t kTextureCount = 2;
    // Per page (see UniformPage, SkinPage), and the most pages a renderer makes.
    static constexpr uint32_t kUniformSlots = 32;
    static constexpr uint32_t kMaxUniformPages = 64;
    static constexpr uint32_t kSkinSlots = 32;
    static constexpr uint32_t kMaxSkinPages = 32;

    // Every draw of a frame writes a UniformBlock of its own: a draw recorded earlier must not read a
    // later one's. Pages of kUniformSlots per frame in flight, with a set per slot and texture; a frame
    // drawing more than the pages hold adds one, kept for the frames after.
    struct UniformPage
    {
        std::unique_ptr<ixrhi::IXRHIBindGroup> group;  // set = index * kTextureCount + texture
        std::shared_ptr<ixrhi::IXRHIBuffer> uniforms;  // each index's UniformBlock, kUniformStride apart
    };
    struct UniformSlot
    {
        UniformPage* page = nullptr;
        std::uint32_t index = 0;  // frame index * kUniformSlots + slot in the page
    };
    // A skin slot is one instance's bone palette and skinned vertices per frame in flight. Pages of
    // kSkinSlots, made when a frame skins more instances of the model than they hold.
    struct SkinPage
    {
        std::unique_ptr<ixrhi::IXRHIBindGroup> computeGroup;  // set = frame index * kSkinSlots + slot
        std::array<std::array<std::shared_ptr<ixrhi::IXRHIBuffer>, kSkinSlots>, kFramesInFlight> palettes{};
        std::array<std::array<std::shared_ptr<ixrhi::IXRHIBuffer>, kSkinSlots>, kFramesInFlight> outputs{};
    };

    struct RawMesh
    {
        uint32_t meshIndex = 0;
        uint32_t baseVertex = 0;
        uint32_t vertexCount = 0;
        std::vector<SourceVertex> sourceVertices;
    };

    struct RestVertexGpu
    {
        float position[4];
        uint32_t packedWeights = 0;
        uint32_t packedBones = 0;
        uint32_t pad0 = 0;
        uint32_t pad1 = 0;
        float normal[4];
        float uv[4];
    };
    static_assert(sizeof(RestVertexGpu) == 64, "Compute rest vertex SSBO stride must stay 64 bytes");

    struct SkinPushConstants
    {
        uint32_t vertexCount = 0;
        uint32_t boneCount = 0;
    };

    struct MeshBounds
    {
        float min[3]{};
        float max[3]{};
        float center[3]{};
        float fitScale = 1.0f;
    };

    struct MeshDraw
    {
        uint32_t firstIndex = 0;
        uint32_t indexCount = 0;
        uint32_t textureIndex = 0;
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

    struct OzzRuntime;
    // One thread's sampling context and transforms for PreparePalette (sized to this skeleton).
    struct PoseScratch;

    bool LoadGltfMesh(const std::string& modelPath);
    bool LoadFbxMesh(const std::string& modelPath);
    bool LoadOzzPose(const std::string& modelPath);
    bool CreateBuffers(ixrhi::IXRHIDevice& rhi);
    struct DecodedTextures;  // LoadCpu -> FinishGpu
    void DecodeTextures(const std::string& modelPath);
    bool UploadDecodedTextures(ixrhi::IXRHIDevice& rhi);
    bool CreateBindGroup(ixrhi::IXRHIDevice& rhi);
    UniformPage* AddUniformPage(ixrhi::IXRHIDevice& rhi);
    // Starts the uniform cursor over when the frame is a new one.
    void BeginFrameSlots(const ixrhi::IXRHIFrameInfo& frame);
    // The frame's next uniform slot; none past kMaxUniformPages (logged once).
    std::optional<UniformSlot> NextUniformSlot(uint32_t frameIndex);
    SkinPage* AddSkinPage(ixrhi::IXRHIDevice& rhi);
    // Makes the skin slot's page; false past MaxSkinSlots() (logged once).
    bool EnsureSkinSlot(uint32_t skinSlot);
    // The slot's buffers for the frame index, or null when the slot was never made.
    ixrhi::IXRHIBuffer* SkinnedOutput(uint32_t frameIndex, uint32_t skinSlot) const;
    ixrhi::IXRHIBuffer* BonePalette(uint32_t frameIndex, uint32_t skinSlot) const;
    bool CreatePipeline(ixrhi::IXRHIDevice& rhi);
    bool CreateReflectionPipeline(ixrhi::IXRHIDevice& rhi, const ixrhi::IXRHIRenderPass* renderPass);
    bool CreateShadowPipeline(ixrhi::IXRHIDevice& rhi, const ixrhi::IXRHIRenderPass* shadowPass);
    // Whether the draws' descriptors are complete (the sun shadow map is bound); logs once if not.
    bool SunShadowBound() const;
    bool CreateComputeResources(ixrhi::IXRHIDevice& rhi);
    bool CreateComputePipeline(ixrhi::IXRHIDevice& rhi);
    bool VerifyComputeSkin(ixrhi::IXRHIDevice& rhi);
    bool SkinPose(float animTimeSeconds, bool updateBounds, bool logSamples, MotionState state = MotionState::Idle);
    // Decomposed pieces of the old monolithic SkinPose, so the runtime path can build a GPU
    // palette without the (CPU-only, bounds/verify) vertex-skinning loop, and so an external
    // pose can be injected (SkinInstanceFromPose).
    bool SamplePoseFromState(float animTimeSeconds, MotionState state, ozz::span<ozz::math::SoaTransform> outLocals);
    bool SamplePose(float animTimeSeconds,
        MotionState state,
        ozz::animation::SamplingJob::Context& context,
        ozz::span<ozz::math::SoaTransform> outLocals) const;
    bool BuildPaletteFromLocals(ozz::span<const ozz::math::SoaTransform> locals);
    // locals -> model transforms -> palette (bone x inverse bind, row-vector), into the given buffers.
    bool BuildPalette(ozz::span<const ozz::math::SoaTransform> locals,
        ozz::span<ozz::math::Float4x4> models,
        std::vector<ixtreeme::math::Mat4>& palette) const;
    bool CpuSkinVertices(bool updateBounds, bool logSamples);
    bool UploadPaletteToBuffer(uint32_t frameIndex, uint32_t skinSlot);
    bool UploadBonePalette(MotionState state, float animTimeSeconds, uint32_t frameIndex, uint32_t skinSlot);
    bool UploadBonePalette(float animTimeSeconds, uint32_t frameIndex);
    void DispatchSkin(ixrhi::IXRHICommandList& cmd, uint32_t frameIndex, uint32_t skinSlot);
    void DispatchSkin(ixrhi::IXRHICommandList& cmd, uint32_t frameIndex);
    void EmitSkinBarrier(ixrhi::IXRHICommandList& cmd, uint32_t frameIndex, uint32_t skinSlot);
    void DestroyComputeResources();
    void DestroyAnimation();
    void DestroyPipeline();
    void DestroyReflectionPipeline();
    void UpdateUniform(const UniformSlot& slot, double timeSeconds, float aspect);
    void UpdateWorldUniform(const UniformSlot& slot,
        const WorldCamera& camera,
        WorldVec3 position,
        float yawRadians,
        double timeSeconds,
        std::array<float, 4> tint,
        std::array<float, 3> scale,
        bool reflectionPass = false,
        float waterLevelY = 0.0f);

    ixrhi::IXRHIDevice* m_rhi = nullptr;
    const ixrhi::IXRHIRenderPass* m_targetPass = nullptr; // borrowed (frame owner)
    client::asset::IAssetReader* m_assets = nullptr;
    std::shared_ptr<ixrhi::IXRHIBuffer> m_indexBuffer;
    std::unique_ptr<ixrhi::IXRHIBindGroupLayout> m_bindLayout;
    std::vector<std::unique_ptr<UniformPage>> m_uniformPages;
    std::unique_ptr<ixrhi::IXRHIBindGroupLayout> m_computeBindLayout;
    std::vector<std::unique_ptr<SkinPage>> m_skinPages;
    bool m_loggedUniformPagesFull = false;
    bool m_loggedSkinPagesFull = false;
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_pipeline;
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_reflectionPipeline;
    const ixrhi::IXRHIRenderPass* m_reflectionPass = nullptr; // borrowed (terrain owns)
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_shadowPipeline;  // sun shadow cascades, depth only
    const ixrhi::IXRHIRenderPass* m_shadowPass = nullptr;             // borrowed (terrain owns)
    bool m_shadowPipelineFailed = false;
    SunShadowReceive m_sunShadow;
    const ixrhi::IXRHITexture* m_boundSunShadowTexture = nullptr;     // what binding 2 holds in every set
    std::unique_ptr<ixrhi::IXRHIComputePipeline> m_computePipeline;
    std::vector<Vertex> m_vertices;
    std::vector<uint32_t> m_indices;
    std::vector<MeshDraw> m_draws;
    std::filesystem::path m_importedDiffuseTexturePath;
    std::vector<RawMesh> m_rawMeshes;
    std::vector<RestVertexGpu> m_restVerticesGpu;
    std::array<Texture, kTextureCount> m_textures{};
    std::shared_ptr<ixrhi::IXRHIBuffer> m_restVertexBuffer;
    uint32_t m_indexCount = 0;
    MeshBounds m_bounds{};
    std::unique_ptr<OzzRuntime> m_ozz;
    std::unique_ptr<DecodedTextures> m_decodedTextures;
    std::string m_loadedModelPath;
    std::vector<std::unique_ptr<PoseScratch>> m_poseScratch;  // indexed by ixjobs worker
    std::vector<ixtreeme::math::Mat4> m_inverseBindMatrices;
    std::vector<ixtreeme::math::Mat4> m_bonePaletteCpu;
    uint32_t m_boneCount = 0;
    MotionState m_motionState = MotionState::Idle;
    LightingState m_lightingState;
    // The frame the uniform cursor counts in (its frame number: a model drawn every other frame meets
    // the same frame index again without the frame between).
    std::uint64_t m_worldRenderFrameNumber = std::numeric_limits<std::uint64_t>::max();
    uint32_t m_worldUniformCursor = 0;
    double m_lastAnimationLogTime = -1000.0;
};
