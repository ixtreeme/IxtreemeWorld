#pragma once

// SelectionOutlineRenderer — Phase-2 IXRHI-native migration (proof path #1).
//
// ZERO Vk* dependency: all buffers/bindings/pipelines/commands go through
// ixrhi::IXRHIDevice / ixrhi::IXRHICommandList. Visual output and draw order are
// unchanged (line list, per-frame uniform + vertex uploads, alpha blend).
//
// Frame contract: the caller supplies the recording command list (borrowed frame
// list via ixvulkan::WrapFrameCommandList) and an IXRHIFrameInfo snapshot. The
// pipeline bakes against m_targetPass (offscreen scene pass, borrowed) or the
// backend default when null — see EngineApplication's offscreenPass borrower.

#include "WorldCamera.h"

#include "IXRHIBinding.h"
#include "IXRHIBuffer.h"
#include "IXRHICommandList.h"
#include "IXRHIDevice.h"
#include "IXRHIPipeline.h"
#include "IXRHIRenderPass.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace client::asset {
class IAssetReader;
}

class SelectionOutlineRenderer
{
public:
    struct Line
    {
        WorldVec3 a{};
        WorldVec3 b{};
        std::array<float, 4> color = {1.0f, 0.61f, 0.07f, 1.0f};
    };

    bool Create(ixrhi::IXRHIDevice& rhi, client::asset::IAssetReader& assets);
    bool RecreatePipeline(ixrhi::IXRHIDevice& rhi);
    // Borrowed target pass (offscreen scene pass); null = backend default.
    // Replaces the pre-Phase-3A backend-global pass override for this renderer.
    void SetTargetPass(const ixrhi::IXRHIRenderPass* pass) { m_targetPass = pass; }
    void Render(ixrhi::IXRHICommandList& cmd,
                const ixrhi::IXRHIFrameInfo& frame,
                const WorldCamera& camera,
                const std::vector<Line>& lines,
                std::uint32_t targetWidth = 0,
                std::uint32_t targetHeight = 0);
    void Destroy();

private:
    static constexpr uint32_t kFramesInFlight = 2;

    struct Vertex
    {
        float position[3];
        float color[4];
    };

    struct UniformBlock
    {
        WorldMat4 mvp;
    };

    bool CreateBuffers(ixrhi::IXRHIDevice& rhi);
    bool CreateBindGroup(ixrhi::IXRHIDevice& rhi);
    bool CreatePipeline(ixrhi::IXRHIDevice& rhi);
    void DestroyPipeline();
    void UpdateUniform(uint32_t frameIndex, const WorldCamera& camera);

    ixrhi::IXRHIDevice* m_rhi = nullptr;
    const ixrhi::IXRHIRenderPass* m_targetPass = nullptr; // borrowed (frame owner)
    client::asset::IAssetReader* m_assets = nullptr;
    std::array<std::shared_ptr<ixrhi::IXRHIBuffer>, kFramesInFlight> m_vertexBuffers{};
    std::array<std::shared_ptr<ixrhi::IXRHIBuffer>, kFramesInFlight> m_uniformBuffers{};
    std::unique_ptr<ixrhi::IXRHIBindGroupLayout> m_bindLayout;
    std::unique_ptr<ixrhi::IXRHIBindGroup> m_bindGroup;
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_pipeline;
};
