#pragma once

// OffscreenSceneRenderer — Phase-3B IXRHI-native facade.
//
// ZERO Vk* dependency. Owns IXRHI color/depth/snapshot textures, sampler,
// clear + load render targets (backend-owned passes/framebuffers), and the
// composite pipeline. Consumers receive IXRHI resources:
//
// - renderers bake pipelines against GetTargetPass() (borrowed token,
//   replaces the app-side Borrow of the native pass),
// - the editor displays GetColorTexture() through EditorGraphicsBridge,
// - terrain refraction samples the snapshot textures + sampler.
//
// Layout tracking rule (binding): states name the PRODUCING use (e.g. the
// color image is ColorAttachment after EndMainPass, not ShaderReadOnly), so
// backend barriers derive correct stages/access from the labels alone.

#include "IXRHIBinding.h"
#include "IXRHIBuffer.h"
#include "IXRHICommandList.h"
#include "IXRHIDevice.h"
#include "IXRHIPipeline.h"
#include "IXRHIRenderPass.h"
#include "IXRHIRenderTarget.h"
#include "IXRHITexture.h"

#include <cstdint>
#include <memory>
#include <string>

namespace client::asset {
class IAssetReader;
}

class OffscreenSceneRenderer
{
public:
    bool Create(ixrhi::IXRHIDevice& rhi,
                client::asset::IAssetReader& assets,
                std::uint32_t width,
                std::uint32_t height,
                ixrhi::IXRHIFormat colorFormat,
                ixrhi::IXRHIFormat depthFormat,
                const std::string& tag = "SceneView");
    bool Recreate(ixrhi::IXRHIDevice& rhi,
                  std::uint32_t width,
                  std::uint32_t height,
                  ixrhi::IXRHIFormat colorFormat,
                  ixrhi::IXRHIFormat depthFormat);
    void BeginMainPass(ixrhi::IXRHICommandList& cmd,
                       const ixrhi::IXRHIFrameInfo& frame,
                       bool clear = true);
    void EndMainPass(ixrhi::IXRHICommandList& cmd);
    void SnapshotScene(ixrhi::IXRHICommandList& cmd, const ixrhi::IXRHIFrameInfo& frame);
    void RenderComposite(ixrhi::IXRHICommandList& cmd, const ixrhi::IXRHIFrameInfo& frame);
    void Destroy();

    bool IsReady() const { return m_ready; }
    const ixrhi::IXRHIRenderPass* GetTargetPass() const;
    const std::shared_ptr<ixrhi::IXRHITexture>& GetColorTexture() const { return m_color; }
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
    ixrhi::IXRHIFormat DepthFormat() const { return m_depthFormat; }

private:
    bool CreateTargets(ixrhi::IXRHIDevice& rhi);
    bool CreateComposite(ixrhi::IXRHIDevice& rhi);

    ixrhi::IXRHIDevice* m_rhi = nullptr;
    client::asset::IAssetReader* m_assets = nullptr;
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    ixrhi::IXRHIFormat m_colorFormat = ixrhi::IXRHIFormat::Undefined;
    ixrhi::IXRHIFormat m_depthFormat = ixrhi::IXRHIFormat::Undefined;
    std::string m_tag = "SceneView";

    std::shared_ptr<ixrhi::IXRHITexture> m_color;
    std::shared_ptr<ixrhi::IXRHITexture> m_depth;
    std::shared_ptr<ixrhi::IXRHITexture> m_colorSnapshot;
    std::shared_ptr<ixrhi::IXRHITexture> m_depthSnapshot;
    std::shared_ptr<ixrhi::IXRHISampler> m_sampler;

    std::unique_ptr<ixrhi::IXRHIRenderTarget> m_clearTarget;
    std::unique_ptr<ixrhi::IXRHIRenderTarget> m_loadTarget;
    const ixrhi::IXRHIRenderTarget* m_activeTarget = nullptr; // borrowed, begun pass

    std::shared_ptr<ixrhi::IXRHIShader> m_compositeVs;
    std::shared_ptr<ixrhi::IXRHIShader> m_compositePs;
    std::unique_ptr<ixrhi::IXRHIBindGroupLayout> m_bindLayout;
    std::unique_ptr<ixrhi::IXRHIBindGroup> m_bindGroup;
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_compositePipeline;

    // Tracked producing-use states (see header contract).
    ixrhi::IXRHIImageLayout m_colorState = ixrhi::IXRHIImageLayout::Undefined;
    ixrhi::IXRHIImageLayout m_depthState = ixrhi::IXRHIImageLayout::Undefined;
    ixrhi::IXRHIImageLayout m_colorSnapshotState = ixrhi::IXRHIImageLayout::Undefined;
    ixrhi::IXRHIImageLayout m_depthSnapshotState = ixrhi::IXRHIImageLayout::Undefined;

    bool m_ready = false;
    bool m_passActive = false;
    bool m_snapshotsReady = false;
};
