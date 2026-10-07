#pragma once

// OffscreenSceneRenderer — Phase-3B IXRHI-native facade.
//
// ZERO Vk* dependency. Owns IXRHI color/depth/snapshot textures, sampler,
// clear + load render targets (backend-owned passes/framebuffers), and the
// composite pipeline. Consumers receive IXRHI resources:
//
// - renderers bake pipelines against GetTargetPass() (borrowed token,
//   replaces the app-side Borrow of the native pass),
// - the editor displays GetDisplayTexture() through EditorGraphicsBridge,
// - terrain refraction samples the snapshot textures + sampler.
//
// The scene renders in floating point (kSceneColorFormat: light of any
// brightness). Tone mapping brings it to the screen: into the display image
// (BeginDisplayPass, whose pass then takes overlays that must not be tone
// mapped, such as editor outlines and the game's UI) or into the swapchain
// (RenderComposite, the game).
//
// Layout tracking rule (binding): states name the layout the image is really
// in, since a barrier's old layout must match it (the color image is
// ShaderReadOnly after EndMainPass: the pass's final layout; its external
// dependency already orders the color writes before shader reads).

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
#include <string>

namespace client::asset {
class IAssetReader;
}

// How the floating-point scene is brought to the screen (the scene's Tone mapping settings).
struct ToneMapSettings
{
    std::int32_t mode = 1;  // 0 none (clipped at white), 1 neutral, 2 filmic
    float exposure = 1.0f;  // light multiplier before the curve
};

class OffscreenSceneRenderer
{
public:
    static constexpr ixrhi::IXRHIFormat kSceneColorFormat = ixrhi::IXRHIFormat::R16G16B16A16Float;

    // displayFormat: the tone-mapped image's format (the swapchain's, so the UI shows it as it is).
    bool Create(ixrhi::IXRHIDevice& rhi,
                client::asset::IAssetReader& assets,
                std::uint32_t width,
                std::uint32_t height,
                ixrhi::IXRHIFormat displayFormat,
                ixrhi::IXRHIFormat depthFormat,
                const std::string& tag = "SceneView");
    bool Recreate(ixrhi::IXRHIDevice& rhi,
                  std::uint32_t width,
                  std::uint32_t height,
                  ixrhi::IXRHIFormat displayFormat,
                  ixrhi::IXRHIFormat depthFormat);
    // storeDepth=false uses a depth attachment whose contents are discarded at pass end (DontCare):
    // consumers that need the depth snapshot (water refraction, god rays, soft particles) must ask
    // for a storing pass, otherwise SnapshotScene/SnapshotDepth degrade to a no-op.
    void BeginMainPass(ixrhi::IXRHICommandList& cmd,
                       const ixrhi::IXRHIFrameInfo& frame,
                       bool clear = true,
                       bool storeDepth = true);
    void EndMainPass(ixrhi::IXRHICommandList& cmd);
    void SnapshotScene(ixrhi::IXRHICommandList& cmd, const ixrhi::IXRHIFrameInfo& frame);
    // Depth-only variant for consumers that need just the scene depth (soft particles): skips the
    // full-resolution color copy the water refraction needs.
    void SnapshotDepth(ixrhi::IXRHICommandList& cmd, const ixrhi::IXRHIFrameInfo& frame);
    // The scene depth itself, made shader-readable, for reads after the scene pass (the god rays): no
    // copy. Null when the pass discarded it (or is still open). It becomes an attachment again when the
    // next pass begins; whatever reads it while a pass draws into it needs a snapshot instead.
    std::shared_ptr<ixrhi::IXRHITexture> ReadableDepth(ixrhi::IXRHICommandList& cmd,
                                                       const ixrhi::IXRHIFrameInfo& frame);
    // Tone-maps the finished scene into the display image and leaves its pass open for overlays.
    void BeginDisplayPass(ixrhi::IXRHICommandList& cmd,
                          const ixrhi::IXRHIFrameInfo& frame,
                          const ToneMapSettings& toneMap);
    void EndDisplayPass(ixrhi::IXRHICommandList& cmd);
    // Clears the display image once after creation, so the UI never samples one never drawn.
    void EnsureDisplayReadable(ixrhi::IXRHICommandList& cmd);
    // Tone-maps the scene into the swapchain pass (open by the caller).
    void RenderComposite(ixrhi::IXRHICommandList& cmd,
                         const ixrhi::IXRHIFrameInfo& frame,
                         const ToneMapSettings& toneMap);
    // Light added to the scene by this frame's tone-map draw, before exposure and the curve: up to two
    // images over the whole view (any size: sampled bilinear), each with a weight. The god rays come in
    // this way instead of in an additive pass of their own over the scene image. Forgotten when the
    // next frame's scene pass begins (cleared).
    void SetAddedLight(const std::shared_ptr<ixrhi::IXRHITexture>& first,
                       float firstWeight,
                       const std::shared_ptr<ixrhi::IXRHITexture>& second,
                       float secondWeight);
    void Destroy();

    bool IsReady() const { return m_ready; }
    const ixrhi::IXRHIRenderPass* GetTargetPass() const;
    const ixrhi::IXRHIRenderPass* GetDisplayPass() const;
    const std::shared_ptr<ixrhi::IXRHITexture>& GetColorTexture() const { return m_color; }
    const std::shared_ptr<ixrhi::IXRHITexture>& GetDisplayTexture() const { return m_display; }
    const std::shared_ptr<ixrhi::IXRHITexture>& GetColorSnapshotTexture() const
    {
        return m_colorSnapshot;
    }
    const std::shared_ptr<ixrhi::IXRHITexture>& GetDepthSnapshotTexture() const
    {
        return m_depthSnapshot;
    }
    const std::shared_ptr<ixrhi::IXRHISampler>& GetSampler() const { return m_sampler; }
    std::uint32_t Width() const { return m_width; }
    std::uint32_t Height() const { return m_height; }
    ixrhi::IXRHIFormat ColorFormat() const { return m_colorFormat; }
    ixrhi::IXRHIFormat DisplayFormat() const { return m_displayFormat; }
    ixrhi::IXRHIFormat DepthFormat() const { return m_depthFormat; }

private:
    bool CreateTargets(ixrhi::IXRHIDevice& rhi);
    bool CreateComposite(ixrhi::IXRHIDevice& rhi);
    void DrawToneMap(ixrhi::IXRHICommandList& cmd,
                     const ixrhi::IXRHIFrameInfo& frame,
                     const ixrhi::IXRHIGraphicsPipeline& pipeline,
                     const ToneMapSettings& toneMap);

    // The tone-map sets, one per frame in flight (their added-light images change between frames).
    static constexpr std::uint32_t kToneMapSets = 2;

    ixrhi::IXRHIDevice* m_rhi = nullptr;
    client::asset::IAssetReader* m_assets = nullptr;
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    ixrhi::IXRHIFormat m_colorFormat = ixrhi::IXRHIFormat::Undefined;
    ixrhi::IXRHIFormat m_displayFormat = ixrhi::IXRHIFormat::Undefined;
    ixrhi::IXRHIFormat m_depthFormat = ixrhi::IXRHIFormat::Undefined;
    std::string m_tag = "SceneView";

    std::shared_ptr<ixrhi::IXRHITexture> m_color;
    std::shared_ptr<ixrhi::IXRHITexture> m_depth;
    std::shared_ptr<ixrhi::IXRHITexture> m_colorSnapshot;
    std::shared_ptr<ixrhi::IXRHITexture> m_depthSnapshot;
    std::shared_ptr<ixrhi::IXRHITexture> m_display;
    std::shared_ptr<ixrhi::IXRHISampler> m_sampler;

    std::unique_ptr<ixrhi::IXRHIRenderTarget> m_clearTarget;
    std::unique_ptr<ixrhi::IXRHIRenderTarget> m_loadTarget;
    std::unique_ptr<ixrhi::IXRHIRenderTarget> m_clearTargetNoDepthStore;
    std::unique_ptr<ixrhi::IXRHIRenderTarget> m_loadTargetNoDepthStore;
    std::unique_ptr<ixrhi::IXRHIRenderTarget> m_displayTarget;
    const ixrhi::IXRHIRenderTarget* m_activeTarget = nullptr; // borrowed, begun pass
    bool m_activeTargetStoresDepth = true;
    bool m_depthStoreValid = false;  // the depth image holds this frame's scene depth

    std::shared_ptr<ixrhi::IXRHIShader> m_compositeVs;
    std::shared_ptr<ixrhi::IXRHIShader> m_compositePs;
    std::unique_ptr<ixrhi::IXRHIBindGroupLayout> m_bindLayout;
    std::unique_ptr<ixrhi::IXRHIBindGroup> m_bindGroup;
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_compositePipeline;  // into the swapchain pass
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_displayPipeline;    // into the display image
    // SetAddedLight's images and weights for this frame's tone-map draw, and what each set binds
    // (bindings 1 and 2; the scene color stands in where no image is added, weighed 0).
    std::array<std::shared_ptr<ixrhi::IXRHITexture>, 2> m_addedLight{};
    std::array<float, 2> m_addedLightWeight{};
    std::array<std::array<std::shared_ptr<ixrhi::IXRHITexture>, 2>, kToneMapSets> m_boundAddedLight{};

    // Tracked producing-use states (see header contract).
    ixrhi::IXRHIImageLayout m_colorState = ixrhi::IXRHIImageLayout::Undefined;
    ixrhi::IXRHIImageLayout m_depthState = ixrhi::IXRHIImageLayout::Undefined;
    ixrhi::IXRHIImageLayout m_colorSnapshotState = ixrhi::IXRHIImageLayout::Undefined;
    ixrhi::IXRHIImageLayout m_depthSnapshotState = ixrhi::IXRHIImageLayout::Undefined;

    bool m_ready = false;
    bool m_passActive = false;
    bool m_displayPassActive = false;
    bool m_displayReadable = false;  // drawn (or cleared) at least once since creation
    bool m_snapshotsReady = false;
};
