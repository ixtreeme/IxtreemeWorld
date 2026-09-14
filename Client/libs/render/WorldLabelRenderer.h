#pragma once

// WorldLabelRenderer — Phase-2 IXRHI-native migration (proof path #2, exercises
// the texture + sampler path: font-atlas upload through IXRHI, sampled in the
// fragment stage. Zero Vk* dependency; glyph raster and vertex building
// unchanged. Renders into the swapchain pass (no render-pass override).

#include "WorldCamera.h"

#include "IXRHIBinding.h"
#include "IXRHIBuffer.h"
#include "IXRHICommandList.h"
#include "IXRHIDevice.h"
#include "IXRHIPipeline.h"
#include "IXRHITexture.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace client::asset {
class IAssetReader;
}

class WorldLabelRenderer
{
public:
    struct Label
    {
        WorldVec3 position{};
        std::string text;
        std::array<float, 4> color = {0.92f, 0.96f, 1.0f, 1.0f};
        bool selected = false;
    };

    bool Create(ixrhi::IXRHIDevice& rhi, client::asset::IAssetReader& assets);
    bool RecreatePipeline(ixrhi::IXRHIDevice& rhi);
    void Render(ixrhi::IXRHICommandList& cmd,
                const ixrhi::IXRHIFrameInfo& frame,
                const WorldCamera& camera,
                const std::vector<Label>& worldLabels);
    void Destroy();

private:
    static constexpr uint32_t kFramesInFlight = 2;

    struct Vertex
    {
        float position[3];
        float uv[2];
        float color[4];
    };

    struct UniformBlock
    {
        WorldMat4 mvp;
    };

    struct Glyph
    {
        float u0 = 0.0f;
        float v0 = 0.0f;
        float u1 = 0.0f;
        float v1 = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        float advance = 0.0f;
    };

    bool CreateBuffers(ixrhi::IXRHIDevice& rhi);
    bool CreateFontAtlas(ixrhi::IXRHIDevice& rhi);
    bool CreateBindGroup(ixrhi::IXRHIDevice& rhi);
    bool CreatePipeline(ixrhi::IXRHIDevice& rhi);
    void DestroyPipeline();
    void UpdateUniform(uint32_t frameIndex, const WorldCamera& camera);
    void BuildVertices(const WorldCamera& camera, const std::vector<Label>& worldLabels, std::vector<Vertex>& vertices) const;
    void AppendLine(std::vector<Vertex>& vertices, WorldVec3 origin, WorldVec3 right, WorldVec3 up,
        const std::string& text, float pixelScale, const float color[4], float fade = 1.0f) const;
    void AppendQuad(std::vector<Vertex>& vertices, WorldVec3 origin, WorldVec3 right, WorldVec3 up,
        float width, float height, const float color[4], float fade = 1.0f) const;

    ixrhi::IXRHIDevice* m_rhi = nullptr;
    client::asset::IAssetReader* m_assets = nullptr;
    std::array<std::shared_ptr<ixrhi::IXRHIBuffer>, kFramesInFlight> m_vertexBuffers{};
    std::array<std::shared_ptr<ixrhi::IXRHIBuffer>, kFramesInFlight> m_uniformBuffers{};
    std::unique_ptr<ixrhi::IXRHIBindGroupLayout> m_bindLayout;
    std::unique_ptr<ixrhi::IXRHIBindGroup> m_bindGroup;
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_pipeline;
    std::shared_ptr<ixrhi::IXRHITexture> m_fontAtlas;
    std::shared_ptr<ixrhi::IXRHISampler> m_fontSampler;
    std::array<Glyph, 128> m_glyphs{};
};
