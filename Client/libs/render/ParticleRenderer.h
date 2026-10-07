#pragma once

// ParticleRenderer — instanced camera-facing quads for the CPU-simulated particles. One instanced
// draw of 6 vertices per emitter batch: the quad corner comes from SV_VertexID (no vertex buffer),
// the per-particle position/size/rotation/color from a StructuredBuffer indexed by SV_InstanceID
// (the same per-instance pattern StaticMesh.hlsl uses). The pixel shader outputs premultiplied
// alpha, so the two pipelines differ only in blend state: Alpha = One/OneMinusSrcAlpha,
// Additive = One/One. Depth tested (LessOrEqual) but not written; no culling.
//
// The pipelines are built against the pass set via SetTargetPass (the offscreen scene pass) and
// lazily rebuilt for any other pass the engine draws particles into (game view, swapchain direct),
// mirroring SkyRenderer's extra-pass handling. Textures are resolved per emitter through the
// engine-wired texture resolver and cached by asset id; an empty/unresolvable id uses a generated
// soft round white sprite.

#include "WorldCamera.h"

#include "IXRHIBinding.h"
#include "IXRHIBuffer.h"
#include "IXRHICommandList.h"
#include "IXRHIDevice.h"
#include "IXRHIPipeline.h"
#include "IXRHIRenderPass.h"
#include "IXRHIShader.h"
#include "IXRHITexture.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace client::asset
{
class IAssetReader;
}

class ParticleRenderer
{
public:
    // One particle as the vertex shader consumes it (64 bytes; mirrored by ParticleInstanceData in
    // shaders/Particles.hlsl, whose members are all float4 so the layout matches under every packing
    // rule). The static_assert guards the mirror.
    struct InstanceData
    {
        float position[3] = {0.0f, 0.0f, 0.0f};
        float size = 1.0f;
        float rotation = 0.0f;
        float padding[3] = {0.0f, 0.0f, 0.0f};
        float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        float uvRect[4] = {0.0f, 0.0f, 1.0f, 1.0f};  // atlas cell: xy = offset, zw = size
    };
    static_assert(sizeof(InstanceData) == 64, "ParticleInstanceData layout must stay 64 bytes");

    // One emitter's live particles for one frame (built by the engine from the simulator).
    struct Batch
    {
        bool additive = false;
        std::string textureAssetId;  // "" = the soft white default sprite
        // Soft particles: fade against the scene depth set via SetSceneDepth (no-op when none is
        // bound, e.g. the depth snapshot is unavailable).
        bool softParticles = true;
        float softDistance = 0.5f;
        // CPU path: the per-particle data built this frame. GPU path: the emitter's state buffer
        // (simulated by SimulateGpuEmitter) is drawn instead.
        bool gpu = false;
        std::uint32_t gpuEntityId = 0;
        std::vector<InstanceData> instances;
    };

    // The parameters the engine feeds the GPU simulation each frame (a mirror of the component
    // fields the compute needs; the renderer never sees the component).
    struct GpuEmitterParams
    {
        float emitterPosition[3] = {0.0f, 0.0f, 0.0f};
        float emitterDirection[3] = {0.0f, 1.0f, 0.0f};
        float gravity = -1.5f;
        float drag = 0.0f;
        float coneAngle = 20.0f;     // degrees
        float rotationSpeed = 0.0f;  // degrees/s
        float shapeRadius = 0.15f;
        float shapeArc = 360.0f;     // degrees
        float shape = 1.0f;          // ixparticle::ParticleShape ordinal
        float shapeExtents[3] = {0.5f, 0.1f, 0.5f};
        float lifetimeMin = 0.7f;
        float lifetimeMax = 1.2f;
        float speedMin = 0.6f;
        float speedMax = 1.4f;
        float sizeMin = 0.12f;
        float sizeMax = 0.25f;
        float sizeOverLife[4] = {1.0f, 1.0f, 1.0f, 0.2f};
        float colorOverLife[16] = {
            1.0f, 0.85f, 0.5f, 1.0f,
            1.0f, 0.60f, 0.3f, 0.8f,
            1.0f, 0.40f, 0.2f, 0.4f,
            1.0f, 0.25f, 0.1f, 0.0f};
        int atlasColumns = 1;
        int atlasRows = 1;
        int maxParticles = 256;
        float emissionRate = 25.0f;
        int burstCount = 0;        // spawned when emission starts (as the CPU path's Reset)
        bool startPlaying = true;  // the emitter's initial state (scripts can change it later)
        // The emitter's local X, Y, Z axes in world space (rows): the spawn shape is oriented by them.
        float emitterAxes[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    };

    ParticleRenderer() = default;
    ~ParticleRenderer();
    ParticleRenderer(const ParticleRenderer&) = delete;
    ParticleRenderer& operator=(const ParticleRenderer&) = delete;
    bool Create(ixrhi::IXRHIDevice& rhi, client::asset::IAssetReader& assets);
    bool RecreatePipeline(ixrhi::IXRHIDevice& rhi);
    void SetTargetPass(const ixrhi::IXRHIRenderPass* pass) { m_targetPass = pass; }
    // The scene depth snapshot soft particles fade against (nullptr = the neutral far-plane dummy,
    // i.e. no fade). Rebound across every draw slot when the texture changes (target recreation).
    void SetSceneDepth(std::shared_ptr<ixrhi::IXRHITexture> depth,
                       std::shared_ptr<ixrhi::IXRHISampler> sampler,
                       float nearPlane,
                       float farPlane);

    // GPU simulation: advances one emitter a frame (creates it lazily, writes its parameters,
    // dispatches the compute and emits the write->vertex-read barrier). Call OUTSIDE any render
    // pass, before the view draws; returns false when the GPU path is unavailable.
    bool SimulateGpuEmitter(ixrhi::IXRHICommandList& cmd,
                            const ixrhi::IXRHIFrameInfo& frame,
                            std::uint32_t entityId,
                            const GpuEmitterParams& params,
                            float dtSeconds);
    // Script controls (no-ops when the emitter has not been created yet).
    void GpuEmitterPlay(std::uint32_t entityId);
    void GpuEmitterStop(std::uint32_t entityId);
    void GpuEmitterRestart(std::uint32_t entityId);
    void GpuEmitterEmit(std::uint32_t entityId, std::uint32_t count);
    // Forgets every GPU emitter (Play session boundaries / project switches).
    void ResetGpuEmitters();
    // Resolves a texture asset id to a file path ("" = not found). Wired by the engine; the
    // renderer itself never touches the asset DB.
    void SetTextureResolver(std::function<std::string(const std::string&)> resolver)
    {
        m_textureResolver = std::move(resolver);
    }

    void RenderInWorld(ixrhi::IXRHICommandList& cmd,
                       const ixrhi::IXRHIFrameInfo& frame,
                       const WorldCamera& camera,
                       const Batch& batch,
                       std::uint32_t width,
                       std::uint32_t height);
    void Destroy();

private:
    static constexpr std::uint32_t kFramesInFlight = 2;
    // Descriptor sets / view uniforms per frame: one per emitter draw (both views). Past the last one
    // a draw is dropped: a set already bound in the frame's commands must not be rewritten.
    static constexpr std::uint32_t kDrawSlots = 128;
    static constexpr std::uint32_t kMaxInstances = 16384;

    struct ViewUniform
    {
        float viewProj[16];
        float cameraRight[4];
        float cameraUp[4];
        float particleParams[4];  // soft enabled, soft distance, near plane, far plane
    };

    struct TextureEntry
    {
        std::shared_ptr<ixrhi::IXRHITexture> texture;
        std::shared_ptr<ixrhi::IXRHISampler> sampler;
    };

    // The GPU simulation's per-emitter uniform (must match ParticlesSim.hlsl's SimParams exactly:
    // 13 float4s).
    struct GpuSimUniform
    {
        float emitterPos[4];     // xyz = position, w = dt
        float emitterDir[4];     // xyz = normalized direction
        float params0[4];        // gravity, drag, cone angle (rad), rotation speed (rad)
        float params1[4];        // shape radius, shape arc (rad), shape ordinal, clear (1: all others die)
        float shapeExtents[4];
        float spawn[4];          // spawn budget, spawn cursor, frame seed, max particles
        float sizeOverLife[4];
        float color0[4];
        float color1[4];
        float color2[4];
        float color3[4];
        float life[4];           // lifetime min/max, size min/max
        float speed[4];          // speed min/max, atlas columns, atlas rows
        float axisX[4];          // the emitter's local axes in world space (spawn shape orientation)
        float axisY[4];
        float axisZ[4];
    };
    static_assert(sizeof(GpuSimUniform) == 256, "SimParams layout must stay 16 float4s");
    static constexpr std::uint64_t kGpuStateBytes = 96;  // ParticlesSim.hlsl ParticleState

    struct GpuEmitter
    {
        std::shared_ptr<ixrhi::IXRHIBuffer> state;     // Storage|Vertex: the compute's ParticleState
        // GpuSimUniform, one per frame in flight (and a compute set each): the CPU writes this
        // frame's while the previous frame's dispatch may still be reading its own.
        std::array<std::shared_ptr<ixrhi::IXRHIBuffer>, kFramesInFlight> uniforms;
        std::unique_ptr<ixrhi::IXRHIBindGroup> computeBindGroup;
        std::uint32_t maxParticles = 0;
        float spawnAccumulator = 0.0f;
        std::uint32_t spawnCursor = 0;
        std::uint32_t frameSeed = 1;
        int pendingBurst = 0;
        bool playing = true;
        // Whether its initial play state is decided (by the component's playOnStart on its first
        // simulation, or by a script's Play/Stop before that); cleared when Play starts.
        bool started = false;
        bool startBurstPending = false;  // the component's burstCount, on the next simulation
        bool clearPending = false;       // the next dispatch kills every particle it does not spawn
        bool initialized = false;  // dispatched at least once: the state buffer has been drawn from
        std::uint64_t lastSeenFrame = 0;  // stale emitters are pruned (frames in flight grace)
    };

    bool CreateBuffers(ixrhi::IXRHIDevice& rhi);
    bool CreateBindGroup(ixrhi::IXRHIDevice& rhi);
    bool CreateDefaultTexture(ixrhi::IXRHIDevice& rhi);
    bool CreateDefaultDepthTexture(ixrhi::IXRHIDevice& rhi);
    bool CreateGpuSimResources(ixrhi::IXRHIDevice& rhi);
    GpuEmitter* EnsureGpuEmitter(ixrhi::IXRHIDevice& rhi, std::uint32_t entityId, int maxParticles);
    const TextureEntry* ResolveTexture(const std::string& assetId);

    ixrhi::IXRHIDevice* m_rhi = nullptr;
    client::asset::IAssetReader* m_assets = nullptr;
    const ixrhi::IXRHIRenderPass* m_targetPass = nullptr;
    std::function<std::string(const std::string&)> m_textureResolver;

    std::shared_ptr<ixrhi::IXRHIShader> m_vertexShader;
    std::shared_ptr<ixrhi::IXRHIShader> m_pixelShader;
    std::array<std::shared_ptr<ixrhi::IXRHIBuffer>, kFramesInFlight> m_instanceBuffers{};
    std::array<std::shared_ptr<ixrhi::IXRHIBuffer>, kFramesInFlight * kDrawSlots> m_uniformBuffers{};
    std::unique_ptr<ixrhi::IXRHIBindGroupLayout> m_bindLayout;
    std::unique_ptr<ixrhi::IXRHIBindGroup> m_bindGroup;
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_alphaPipeline;
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_additivePipeline;
    // GPU simulation path: the compute pipeline + its layout, the GPU draw shaders/pipelines and the
    // per-emitter state (keyed by entity id).
    std::shared_ptr<ixrhi::IXRHIShader> m_gpuVertexShader;
    std::shared_ptr<ixrhi::IXRHIShader> m_gpuPixelShader;
    std::shared_ptr<ixrhi::IXRHIShader> m_gpuSimShader;
    std::unique_ptr<ixrhi::IXRHIBindGroupLayout> m_gpuSimLayout;
    std::unique_ptr<ixrhi::IXRHIComputePipeline> m_gpuSimPipeline;
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_gpuAlphaPipeline;
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_gpuAdditivePipeline;
    std::unordered_map<std::uint32_t, GpuEmitter> m_gpuEmitters;
    std::uint64_t m_lastGpuPruneFrame = 0;
    TextureEntry m_defaultTexture;
    std::unordered_map<std::string, TextureEntry> m_textures;  // key = texture asset id
    std::vector<std::string> m_failedTextureIds;  // logged once; retried only after a restart
    // Soft particles: the scene depth snapshot bound to every draw slot (dummy = far plane).
    std::shared_ptr<ixrhi::IXRHITexture> m_sceneDepth;
    std::shared_ptr<ixrhi::IXRHISampler> m_sceneDepthSampler;
    std::shared_ptr<ixrhi::IXRHITexture> m_dummyDepth;
    // What each draw slot's set binds as the depth (binding 3), held for its address.
    std::array<std::shared_ptr<ixrhi::IXRHITexture>, kFramesInFlight * kDrawSlots> m_boundDepth{};
    float m_sceneNear = 0.1f;
    float m_sceneFar = 1000.0f;
    std::uint64_t m_lastFrameNumber = 0;
    std::uint32_t m_instanceCursor = 0;
    std::uint32_t m_drawSlotCursor = 0;
    bool m_loggedCapacity = false;
};
