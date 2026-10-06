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
        std::vector<InstanceData> instances;
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
    static constexpr std::uint32_t kDrawSlots = 32;
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

    bool CreateBuffers(ixrhi::IXRHIDevice& rhi);
    bool CreateBindGroup(ixrhi::IXRHIDevice& rhi);
    bool CreateDefaultTexture(ixrhi::IXRHIDevice& rhi);
    bool CreateDefaultDepthTexture(ixrhi::IXRHIDevice& rhi);
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
    TextureEntry m_defaultTexture;
    std::unordered_map<std::string, TextureEntry> m_textures;  // key = texture asset id
    std::vector<std::string> m_failedTextureIds;  // logged once; retried only after a restart
    // Soft particles: the scene depth snapshot bound to every draw slot (dummy = far plane).
    std::shared_ptr<ixrhi::IXRHITexture> m_sceneDepth;
    std::shared_ptr<ixrhi::IXRHISampler> m_sceneDepthSampler;
    std::shared_ptr<ixrhi::IXRHITexture> m_dummyDepth;
    const ixrhi::IXRHITexture* m_boundDepth = nullptr;
    float m_sceneNear = 0.1f;
    float m_sceneFar = 1000.0f;
    std::uint64_t m_lastFrameNumber = 0;
    std::uint32_t m_instanceCursor = 0;
    std::uint32_t m_drawSlotCursor = 0;
    bool m_loggedCapacity = false;
};
