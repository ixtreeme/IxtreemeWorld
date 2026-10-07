#pragma once

// GodRayRenderer — light shafts from the Sun (SkySettings::godRays), two techniques:
//   screen space — a quarter-size occlusion mask (the sun where the sky shows past the geometry)
//                  blurred radially towards the sun's screen position: shafts around the sun when
//                  it is on the screen;
//   volumetric   — the sunlight the air scatters towards the camera along every view ray, shadowed
//                  by the sun shadow cascades: shafts seen from the side too, wherever the sun is;
//   or both, added together.
//
// Per view (0 = the Scene view and the built game, 1 = the editor Game view), after the scene is in
// its offscreen target and a depth snapshot was taken:
//   RenderRays  — outside any render pass, into the view's own quarter-size targets (both effects are soft);
//   TakeAddedLight — the two results for the view's scene, which adds them in its tone-map draw
//                    (OffscreenSceneRenderer::SetAddedLight): no pass of their own over the image.
// Every pass takes its per-view data as push constants, so the two views never share per-frame data;
// the cascade matrices (the same for both views) are one uniform buffer per frame in flight.

#include "MapEditorTypes.h"
#include "WorldCamera.h"

#include "IXRHIBinding.h"
#include "IXRHIBuffer.h"
#include "IXRHICommandList.h"
#include "IXRHIDevice.h"
#include "IXRHIPipeline.h"
#include "IXRHIRenderPass.h"
#include "IXRHIRenderTarget.h"
#include "IXRHITexture.h"

#include <array>
#include <cstdint>
#include <memory>

namespace client::asset {
class IAssetReader;
}

class GodRayRenderer
{
public:
    static constexpr std::uint32_t kViews = 2;
    static constexpr std::uint32_t kCascades = 4;

    // The sun shadow cascades drawn this frame (TerrainRenderer): texture null when there are none.
    struct SunShadow
    {
        std::shared_ptr<ixrhi::IXRHITexture> texture;  // D32 array, one layer per cascade, shader-readable
        const WorldMat4* cascadeViewProj = nullptr;    // kCascades light view-projections, finest first
        float depthBias = 0.0015f;
    };

    bool Create(ixrhi::IXRHIDevice& rhi, client::asset::IAssetReader& assets);
    // Whether this view shows any rays this frame (so a depth snapshot is needed).
    bool IsVisible(const SkySettings& sky, const LightingState& lighting, const WorldCamera& camera,
                   const SunShadow& shadow) const;
    // Both techniques for `view`, outside any render pass. depth: the view's depth snapshot
    // (shader-read). Returns false when neither technique drew anything.
    bool RenderRays(ixrhi::IXRHICommandList& cmd,
                    const ixrhi::IXRHIFrameInfo& frame,
                    std::uint32_t view,
                    const SkySettings& sky,
                    const LightingState& lighting,
                    const WorldCamera& camera,
                    const std::shared_ptr<ixrhi::IXRHITexture>& depth,
                    const SunShadow& shadow,
                    std::uint32_t width,
                    std::uint32_t height);
    // The view's results of this frame's RenderRays for its scene's tone-map draw, each with its
    // weight (0 for a technique not drawn). False when neither was drawn.
    struct AddedLight
    {
        std::shared_ptr<ixrhi::IXRHITexture> shafts;  // screen space
        float shaftsWeight = 0.0f;
        std::shared_ptr<ixrhi::IXRHITexture> volume;  // volumetric
        float volumeWeight = 0.0f;
    };
    bool TakeAddedLight(std::uint32_t view, AddedLight& out);
    void Destroy();

private:
    static constexpr std::uint32_t kFramesInFlight = 2;

    // Screen-space passes (GodRays.hlsl); the volumetric blur uses it too.
    struct Push
    {
        float invViewProjection[16];
        float sunDir[4];
        float sunColor[4];
        float rayParams[4];
        float composite[4];
    };
    // The volumetric march (VolumetricLight.hlsl).
    struct VolumetricPush
    {
        float invViewProjection[16];
        float cameraPos[4];
        float sunDir[4];
        float sunColor[4];
        float params[4];
    };

    struct ViewTargets
    {
        std::uint32_t width = 0;  // a quarter of the view size
        std::uint32_t height = 0;
        std::shared_ptr<ixrhi::IXRHITexture> mask;
        std::shared_ptr<ixrhi::IXRHITexture> blur;
        std::shared_ptr<ixrhi::IXRHITexture> volume;
        std::shared_ptr<ixrhi::IXRHITexture> volumeBlur;
        std::unique_ptr<ixrhi::IXRHIRenderTarget> maskTarget;
        std::unique_ptr<ixrhi::IXRHIRenderTarget> blurTarget;
        std::unique_ptr<ixrhi::IXRHIRenderTarget> volumeTarget;
        std::unique_ptr<ixrhi::IXRHIRenderTarget> volumeBlurTarget;
        Push push{};
        bool screenSpaceReady = false;  // produced this frame, to be added to the scene
        bool volumetricReady = false;
        const char* lastScreenSpaceState = nullptr;  // last states logged (logged on change)
        const char* lastVolumetricState = nullptr;
    };

    // Push data for a camera; null when the technique shows, otherwise why it does not.
    const char* BuildPush(const SkySettings& sky, const LightingState& lighting, const WorldCamera& camera, Push& out) const;
    const char* BuildVolumetricPush(const SkySettings& sky, const LightingState& lighting, const WorldCamera& camera,
                                    const SunShadow& shadow, VolumetricPush& out) const;
    bool EnsureViewTargets(ViewTargets& targets, std::uint32_t width, std::uint32_t height);
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> BuildPipeline(const std::shared_ptr<ixrhi::IXRHIShader>& ps,
                                                                const ixrhi::IXRHIBindGroupLayout& layout,
                                                                std::uint32_t pushBytes,
                                                                const ixrhi::IXRHIRenderPass* pass,
                                                                const char* name);
    void DrawFullscreen(ixrhi::IXRHICommandList& cmd,
                        const ixrhi::IXRHIRenderTarget& target,
                        std::uint32_t width,
                        std::uint32_t height,
                        const ixrhi::IXRHIGraphicsPipeline& pipeline,
                        const ixrhi::IXRHIBindGroup& bindings,
                        std::uint32_t set,
                        const void* push,
                        std::uint32_t pushBytes);

    ixrhi::IXRHIDevice* m_rhi = nullptr;
    client::asset::IAssetReader* m_assets = nullptr;
    std::shared_ptr<ixrhi::IXRHIShader> m_vs;
    std::shared_ptr<ixrhi::IXRHIShader> m_maskPs;
    std::shared_ptr<ixrhi::IXRHIShader> m_blurPs;
    std::shared_ptr<ixrhi::IXRHIShader> m_volumeBlurPs;
    std::shared_ptr<ixrhi::IXRHIShader> m_marchPs;
    std::shared_ptr<ixrhi::IXRHISampler> m_sampler;
    std::shared_ptr<ixrhi::IXRHISampler> m_shadowSampler;  // depth compare, lit outside the map
    std::unique_ptr<ixrhi::IXRHIBindGroupLayout> m_bindLayout;   // one texture
    std::unique_ptr<ixrhi::IXRHIBindGroupLayout> m_marchLayout;  // depth, shadow cascades, cascade matrices
    // One set per (view, frame in flight) for each pass: rewritten just before its use.
    std::unique_ptr<ixrhi::IXRHIBindGroup> m_maskBindings;
    std::unique_ptr<ixrhi::IXRHIBindGroup> m_blurBindings;
    std::unique_ptr<ixrhi::IXRHIBindGroup> m_marchBindings;
    std::unique_ptr<ixrhi::IXRHIBindGroup> m_volumeBlurBindings;
    std::array<std::shared_ptr<ixrhi::IXRHIBuffer>, kFramesInFlight> m_cascadeBuffers{};
    // A 1x1 target of the quarter-size targets' format: their pipelines bake against its pass,
    // which outlives every view target (they are recreated on resize).
    std::shared_ptr<ixrhi::IXRHITexture> m_prototypeTexture;
    std::unique_ptr<ixrhi::IXRHIRenderTarget> m_prototypeTarget;
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_maskPipeline;
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_blurPipeline;
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_marchPipeline;
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_volumeBlurPipeline;
    std::array<ViewTargets, kViews> m_views{};
};
