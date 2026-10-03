#pragma once

// SkyRenderer — the scene's sky (SkySettings): a flat colour, a procedural gradient with a sun disc
// that follows the Sun light, a six-image cube map, or an equirectangular panorama (LDR or .hdr).
//
// Draw contract: one full-screen triangle at the far plane with depth test LessOrEqual and no depth
// writes, so it can be drawn first (it becomes the background) or after the opaque geometry (the
// water reflection pass) in any pass that has a depth attachment. The per-view camera goes in a push
// constant, so drawing it for several views in one frame (Scene, Game, reflection) never clobbers;
// the sky parameters are one uniform buffer per frame in flight, written once per frame by Update.

#include "MapEditorTypes.h"
#include "WorldCamera.h"

#include "IXRHIBinding.h"
#include "IXRHIBuffer.h"
#include "IXRHICommandList.h"
#include "IXRHIDevice.h"
#include "IXRHIPipeline.h"
#include "IXRHIRenderPass.h"
#include "IXRHITexture.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace client::asset {
class IAssetReader;
}

class SkyRenderer
{
public:
    bool Create(ixrhi::IXRHIDevice& rhi, client::asset::IAssetReader& assets);
    bool RecreatePipeline(ixrhi::IXRHIDevice& rhi);
    // Borrowed target pass (offscreen scene pass); null = backend default (the swapchain pass).
    void SetTargetPass(const ixrhi::IXRHIRenderPass* pass) { m_targetPass = pass; }
    // Once per frame, before any Render: takes the sky, the sun it follows and the project root the
    // image paths are relative to. (Re)loads the images when their paths change.
    void Update(const ixrhi::IXRHIFrameInfo& frame,
                const SkySettings& sky,
                const LightingState& lighting,
                const std::filesystem::path& projectRoot);
    // Draws the sky into the current pass. renderPass: the pass being recorded when it is not the
    // target pass (the water reflection pass); a pipeline is baked for it on first use.
    void Render(ixrhi::IXRHICommandList& cmd,
                const ixrhi::IXRHIFrameInfo& frame,
                const WorldCamera& camera,
                std::uint32_t targetWidth,
                std::uint32_t targetHeight,
                const ixrhi::IXRHIRenderPass* renderPass = nullptr);
    // The sky's average colour (linear, exposure and tint included) — for "ambient from sky".
    std::array<float, 3> AverageColor() const { return m_averageColor; }
    // Empty while the sky draws as set; otherwise why it cannot (a missing or unreadable image).
    const std::string& Status() const { return m_status; }
    bool IsReady() const { return m_pipeline != nullptr; }
    void Destroy();

private:
    static constexpr std::uint32_t kFramesInFlight = 2;

    struct UniformBlock
    {
        float zenith[4];
        float horizon[4];
        float ground[4];
        float sunDir[4];
        float sunColor[4];
        float tint[4];
        float color[4];
    };

    bool CreateResources(ixrhi::IXRHIDevice& rhi);
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> BuildPipeline(ixrhi::IXRHIDevice& rhi,
        const ixrhi::IXRHIRenderPass* pass);
    void BindTextures();
    void LoadImages(const SkySettings& sky, const std::filesystem::path& projectRoot);

    ixrhi::IXRHIDevice* m_rhi = nullptr;
    client::asset::IAssetReader* m_assets = nullptr;
    const ixrhi::IXRHIRenderPass* m_targetPass = nullptr;  // borrowed (frame owner)
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_pipeline;
    // A second pipeline for the one other pass the sky is drawn in (the water reflection).
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_extraPipeline;
    const ixrhi::IXRHIRenderPass* m_extraPass = nullptr;
    std::shared_ptr<ixrhi::IXRHIShader> m_vertexShader;
    std::shared_ptr<ixrhi::IXRHIShader> m_fragmentShader;
    std::unique_ptr<ixrhi::IXRHIBindGroupLayout> m_bindLayout;
    std::unique_ptr<ixrhi::IXRHIBindGroup> m_bindGroup;
    std::array<std::shared_ptr<ixrhi::IXRHIBuffer>, kFramesInFlight> m_uniformBuffers{};
    std::shared_ptr<ixrhi::IXRHISampler> m_cubeSampler;
    std::shared_ptr<ixrhi::IXRHISampler> m_panoramaSampler;
    std::shared_ptr<ixrhi::IXRHITexture> m_fallbackCube;
    std::shared_ptr<ixrhi::IXRHITexture> m_fallbackPanorama;
    std::shared_ptr<ixrhi::IXRHITexture> m_cube;      // null while no cube map is loaded
    std::shared_ptr<ixrhi::IXRHITexture> m_panorama;  // null while no panorama is loaded

    // What the loaded images came from: reloaded when any of these change.
    std::array<std::string, SkySettings::kCubeFaces> m_loadedCubePaths{};
    std::string m_loadedPanoramaPath;
    std::filesystem::path m_loadedRoot;
    bool m_cubeRequested = false;
    bool m_panoramaRequested = false;
    std::array<float, 3> m_cubeAverage{};      // linear, before exposure/tint
    std::array<float, 3> m_panoramaAverage{};  // linear, before exposure/tint

    std::array<float, 3> m_averageColor{};
    std::string m_cubeStatus;
    std::string m_panoramaStatus;
    std::string m_status;
};
