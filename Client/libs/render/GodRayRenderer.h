#pragma once

// GodRayRenderer — screen-space light shafts from the Sun (SkySettings::godRays).
//
// Per view (0 = the Scene view and the built game, 1 = the editor Game view), after the scene is in
// its offscreen target and a depth snapshot was taken:
//   RenderRays  — outside any render pass: a half-size occlusion mask (the sun where the sky shows)
//                 and its radial blur towards the sun's screen position, in the view's own targets;
//   Composite   — inside the view's scene pass (loaded, not cleared): the blur added over the image.
// Nothing is drawn while the rays are off, the sun is under the horizon or far off the screen.
// Every pass takes its parameters as push constants, so the two views never share per-frame data.

#include "MapEditorTypes.h"
#include "WorldCamera.h"

#include "IXRHIBinding.h"
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

    bool Create(ixrhi::IXRHIDevice& rhi, client::asset::IAssetReader& assets);
    bool RecreatePipeline(ixrhi::IXRHIDevice& rhi);
    // Borrowed scene pass the composite draws into (offscreen scene pass); null = backend default.
    void SetTargetPass(const ixrhi::IXRHIRenderPass* pass) { m_targetPass = pass; }
    // Whether this view shows rays this frame (the settings, the sun's height and screen position).
    bool IsVisible(const SkySettings& sky, const LightingState& lighting, const WorldCamera& camera) const;
    // Mask + blur for `view`, outside any render pass. depth: the view's depth snapshot (shader-read).
    // Returns false when nothing is to be composited.
    bool RenderRays(ixrhi::IXRHICommandList& cmd,
                    const ixrhi::IXRHIFrameInfo& frame,
                    std::uint32_t view,
                    const SkySettings& sky,
                    const LightingState& lighting,
                    const WorldCamera& camera,
                    const std::shared_ptr<ixrhi::IXRHITexture>& depth,
                    std::uint32_t width,
                    std::uint32_t height);
    // Adds the view's rays into the current (scene) pass.
    void Composite(ixrhi::IXRHICommandList& cmd,
                   const ixrhi::IXRHIFrameInfo& frame,
                   std::uint32_t view,
                   std::uint32_t width,
                   std::uint32_t height);
    void Destroy();

private:
    static constexpr std::uint32_t kFramesInFlight = 2;

    struct Push
    {
        float invViewProjection[16];
        float sunDir[4];
        float sunColor[4];
        float rayParams[4];
        float composite[4];
    };

    struct ViewTargets
    {
        std::uint32_t width = 0;  // half the view size
        std::uint32_t height = 0;
        std::shared_ptr<ixrhi::IXRHITexture> mask;
        std::shared_ptr<ixrhi::IXRHITexture> blur;
        std::unique_ptr<ixrhi::IXRHIRenderTarget> maskTarget;
        std::unique_ptr<ixrhi::IXRHIRenderTarget> blurTarget;
        Push push{};
        bool ready = false;  // RenderRays produced something this frame
        const char* lastState = nullptr;  // last state logged (logged on change)
    };

    // Computes the push data for a camera; null when the rays show, otherwise why they do not.
    const char* BuildPush(const SkySettings& sky, const LightingState& lighting, const WorldCamera& camera, Push& out) const;
    bool EnsureViewTargets(ViewTargets& targets, std::uint32_t width, std::uint32_t height);
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> BuildPipeline(const std::shared_ptr<ixrhi::IXRHIShader>& ps,
                                                                const ixrhi::IXRHIRenderPass* pass,
                                                                bool additive,
                                                                const char* name);

    ixrhi::IXRHIDevice* m_rhi = nullptr;
    client::asset::IAssetReader* m_assets = nullptr;
    const ixrhi::IXRHIRenderPass* m_targetPass = nullptr;  // borrowed (frame owner)
    std::shared_ptr<ixrhi::IXRHIShader> m_vs;
    std::shared_ptr<ixrhi::IXRHIShader> m_maskPs;
    std::shared_ptr<ixrhi::IXRHIShader> m_blurPs;
    std::shared_ptr<ixrhi::IXRHIShader> m_compositePs;
    std::shared_ptr<ixrhi::IXRHISampler> m_sampler;
    std::unique_ptr<ixrhi::IXRHIBindGroupLayout> m_bindLayout;
    // One set per (view, frame in flight) for each pass: rewritten just before its use.
    std::unique_ptr<ixrhi::IXRHIBindGroup> m_maskBindings;
    std::unique_ptr<ixrhi::IXRHIBindGroup> m_blurBindings;
    std::unique_ptr<ixrhi::IXRHIBindGroup> m_compositeBindings;
    // A 1x1 target of the half-size targets' format: the mask and blur pipelines bake against its
    // pass, which outlives every view target (they are recreated on resize).
    std::shared_ptr<ixrhi::IXRHITexture> m_prototypeTexture;
    std::unique_ptr<ixrhi::IXRHIRenderTarget> m_prototypeTarget;
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_maskPipeline;
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_blurPipeline;
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_compositePipeline;
    std::array<ViewTargets, kViews> m_views{};
};
