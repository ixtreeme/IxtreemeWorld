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

#include "IXRHIBinding.h"
#include "IXRHIBuffer.h"
#include "IXRHICommandList.h"
#include "IXRHIDevice.h"
#include "IXRHIPipeline.h"
#include "IXRHIRenderPass.h"
#include "IXRHITexture.h"

#include <ozz/base/maths/soa_transform.h>
#include <ozz/base/span.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
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
    // Skeleton accessors for the animation layer (clip retargeting, rest-pose fallback).
    // Return null/empty when no skeleton is loaded yet.
    const ozz::animation::Skeleton* Skeleton() const;
    ozz::span<const ozz::math::SoaTransform> RestPoseLocals() const;
    std::uint32_t NumJoints() const;
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
        std::uint32_t targetHeight = 0);
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
        std::array<float, 4> tint = {1.0f, 1.0f, 1.0f, 1.0f});
    void SetLightingState(const LightingState& lighting) { m_lightingState = lighting; }
    void SetMotionState(MotionState state);
    float GroundOffsetY() const;
    std::uint32_t MaterialSlotCount() const { return std::max<std::uint32_t>(1u, static_cast<std::uint32_t>(m_draws.size())); }
    static constexpr uint32_t MaxSkinSlots() { return kSkinSlots; }
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
    static constexpr uint32_t kUniformSlots = 32;
    static constexpr uint32_t kSkinSlots = 32;

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

    bool LoadGltfMesh(const std::string& modelPath);
    bool LoadFbxMesh(const std::string& modelPath);
    bool LoadOzzPose(const std::string& modelPath);
    bool CreateBuffers(ixrhi::IXRHIDevice& rhi);
    bool CreateTextures(ixrhi::IXRHIDevice& rhi, const std::string& modelPath);
    bool CreateBindGroup(ixrhi::IXRHIDevice& rhi);
    bool CreatePipeline(ixrhi::IXRHIDevice& rhi);
    bool CreateReflectionPipeline(ixrhi::IXRHIDevice& rhi, const ixrhi::IXRHIRenderPass* renderPass);
    bool CreateComputeResources(ixrhi::IXRHIDevice& rhi);
    bool CreateComputeBindGroup(ixrhi::IXRHIDevice& rhi);
    bool CreateComputePipeline(ixrhi::IXRHIDevice& rhi);
    bool VerifyComputeSkin(ixrhi::IXRHIDevice& rhi);
    bool SkinPose(float animTimeSeconds, bool updateBounds, bool logSamples, MotionState state = MotionState::Idle);
    // Decomposed pieces of the old monolithic SkinPose, so the runtime path can build a GPU
    // palette without the (CPU-only, bounds/verify) vertex-skinning loop, and so an external
    // pose can be injected (SkinInstanceFromPose).
    bool SamplePoseFromState(float animTimeSeconds, MotionState state, ozz::span<ozz::math::SoaTransform> outLocals);
    bool BuildPaletteFromLocals(ozz::span<const ozz::math::SoaTransform> locals);
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
    void UpdateUniform(uint32_t frameIndex, uint32_t uniformSlot, double timeSeconds, float aspect);
    void UpdateWorldUniform(uint32_t frameIndex,
        uint32_t uniformSlot,
        const WorldCamera& camera,
        WorldVec3 position,
        float yawRadians,
        double timeSeconds,
        std::array<float, 4> tint,
        bool reflectionPass = false,
        float waterLevelY = 0.0f);

    ixrhi::IXRHIDevice* m_rhi = nullptr;
    const ixrhi::IXRHIRenderPass* m_targetPass = nullptr; // borrowed (frame owner)
    client::asset::IAssetReader* m_assets = nullptr;
    std::shared_ptr<ixrhi::IXRHIBuffer> m_indexBuffer;
    std::array<std::array<std::shared_ptr<ixrhi::IXRHIBuffer>, kUniformSlots>, kFramesInFlight> m_uniformBuffers{};
    std::unique_ptr<ixrhi::IXRHIBindGroupLayout> m_bindLayout;
    std::unique_ptr<ixrhi::IXRHIBindGroup> m_bindGroup;
    std::unique_ptr<ixrhi::IXRHIBindGroupLayout> m_computeBindLayout;
    std::unique_ptr<ixrhi::IXRHIBindGroup> m_computeBindGroup;
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_pipeline;
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_reflectionPipeline;
    const ixrhi::IXRHIRenderPass* m_reflectionPass = nullptr; // borrowed (terrain owns)
    std::unique_ptr<ixrhi::IXRHIComputePipeline> m_computePipeline;
    std::vector<Vertex> m_vertices;
    std::vector<uint32_t> m_indices;
    std::vector<MeshDraw> m_draws;
    std::filesystem::path m_importedDiffuseTexturePath;
    std::vector<RawMesh> m_rawMeshes;
    std::vector<RestVertexGpu> m_restVerticesGpu;
    std::array<Texture, kTextureCount> m_textures{};
    std::shared_ptr<ixrhi::IXRHIBuffer> m_restVertexBuffer;
    std::array<std::array<std::shared_ptr<ixrhi::IXRHIBuffer>, kSkinSlots>, kFramesInFlight> m_bonePaletteBuffers{};
    std::array<std::array<std::shared_ptr<ixrhi::IXRHIBuffer>, kSkinSlots>, kFramesInFlight> m_skinnedOutputBuffers{};
    uint32_t m_indexCount = 0;
    MeshBounds m_bounds{};
    std::unique_ptr<OzzRuntime> m_ozz;
    std::vector<ixtreeme::math::Mat4> m_inverseBindMatrices;
    std::vector<ixtreeme::math::Mat4> m_bonePaletteCpu;
    uint32_t m_boneCount = 0;
    MotionState m_motionState = MotionState::Idle;
    LightingState m_lightingState;
    uint32_t m_worldRenderFrameIndex = std::numeric_limits<uint32_t>::max();
    uint32_t m_worldUniformCursor = 0;
    double m_lastAnimationLogTime = -1000.0;
};
