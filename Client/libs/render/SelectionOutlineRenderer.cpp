// SelectionOutlineRenderer — IXRHI-native implementation. Same line batching,
// same per-frame uploads, same pipeline state as before; only the API crossed
// the boundary (vk* calls now live in Engine/Graphics/Vulkan/).

#include "SelectionOutlineRenderer.h"

#include "Debug.h"
#include "IXRHIShader.h"
#include "asset/IAssetReader.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
constexpr uint32_t kMaxLines = 4096;
constexpr uint32_t kMaxVertices = kMaxLines * 2u;

std::vector<std::uint32_t> ReadSpirv(client::asset::IAssetReader& assets, const std::string& path)
{
    auto bytes = assets.ReadAll(path);
    if (!bytes || bytes->empty() || bytes->size() % sizeof(std::uint32_t) != 0)
    {
        Tracenf("[SELECTION-OUTLINE] failed to open shader: %s", path.c_str());
        std::abort();
    }
    const auto* words = reinterpret_cast<const std::uint32_t*>(bytes->data());
    return std::vector<std::uint32_t>(words, words + bytes->size() / sizeof(std::uint32_t));
}

std::shared_ptr<ixrhi::IXRHIShader> LoadShader(ixrhi::IXRHIDevice& rhi,
                                               client::asset::IAssetReader& assets,
                                               const std::string& path,
                                               ixrhi::IXRHIShaderStage stage,
                                               const char* entry)
{
    ixrhi::IXRHIShaderDesc desc;
    desc.stage = stage;
    desc.entryPoint = entry;
    desc.spirv = ReadSpirv(assets, path);
    desc.debugName = path;
    return rhi.CreateShader(desc);
}
} // namespace

bool SelectionOutlineRenderer::Create(ixrhi::IXRHIDevice& rhi, client::asset::IAssetReader& assets)
{
    Destroy();
    m_rhi = &rhi;
    m_assets = &assets;

    const bool buffers = CreateBuffers(rhi);
    const bool bindings = buffers ? CreateBindGroup(rhi) : false;
    const bool pipeline = bindings ? CreatePipeline(rhi) : false;
    Tracenf("[SELECTION-OUTLINE] Create: buffers=%d descriptors=%d pipeline=%d",
        buffers ? 1 : 0,
        bindings ? 1 : 0,
        pipeline ? 1 : 0);

    if (buffers && bindings && pipeline)
        return true;

    Destroy();
    return false;
}

bool SelectionOutlineRenderer::RecreatePipeline(ixrhi::IXRHIDevice& rhi)
{
    if (!m_rhi)
        return true;
    m_rhi = &rhi;
    DestroyPipeline();
    // Backend yields null while the target pass is torn down: deferred, retried
    // on the next RecreatePipeline. Always true (parity: real failures abort).
    CreatePipeline(rhi);
    return true;
}

void SelectionOutlineRenderer::Render(ixrhi::IXRHICommandList& cmd,
                                      const ixrhi::IXRHIFrameInfo& frame,
                                      const WorldCamera& camera,
                                      const std::vector<Line>& lines,
                                      std::uint32_t targetWidth,
                                      std::uint32_t targetHeight)
{
    if (!m_pipeline || !frame.frameActive || lines.empty())
        return;

    const std::uint32_t width = targetWidth > 0 ? targetWidth : frame.targetWidth;
    const std::uint32_t height = targetHeight > 0 ? targetHeight : frame.targetHeight;
    if (width == 0 || height == 0)
        return;

    const uint32_t frameIndex = frame.frameIndex % kFramesInFlight;
    std::vector<Vertex> vertices;
    vertices.reserve(std::min<std::size_t>(lines.size(), kMaxLines) * 2u);
    for (const Line& line : lines)
    {
        if (vertices.size() + 2u > kMaxVertices)
            break;
        vertices.push_back(Vertex{{line.a.x, line.a.y, line.a.z},
            {line.color[0], line.color[1], line.color[2], line.color[3]}});
        vertices.push_back(Vertex{{line.b.x, line.b.y, line.b.z},
            {line.color[0], line.color[1], line.color[2], line.color[3]}});
    }
    if (vertices.empty())
        return;

    UpdateUniform(frameIndex, camera);
    m_vertexBuffers[frameIndex]->Write(
        0, vertices.data(), sizeof(Vertex) * vertices.size());

    cmd.SetViewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    cmd.SetScissor(0, 0, width, height);
    cmd.SetGraphicsPipeline(*m_pipeline);
    cmd.SetVertexBuffer(0, *m_vertexBuffers[frameIndex], 0);
    cmd.BindGroup(0, *m_bindGroup, frameIndex);
    cmd.Draw(static_cast<uint32_t>(vertices.size()), 1, 0, 0);
}

void SelectionOutlineRenderer::Destroy()
{
    if (!m_rhi)
        return;

    DestroyPipeline();
    m_bindGroup.reset();
    m_bindLayout.reset();
    for (auto& buffer : m_vertexBuffers)
        buffer.reset();
    for (auto& buffer : m_uniformBuffers)
        buffer.reset();

    m_rhi = nullptr;
    m_assets = nullptr;
}

bool SelectionOutlineRenderer::CreateBuffers(ixrhi::IXRHIDevice& rhi)
{
    for (auto& buffer : m_vertexBuffers)
    {
        ixrhi::IXRHIBufferDesc desc;
        desc.sizeBytes = sizeof(Vertex) * kMaxVertices;
        desc.usage = ixrhi::IXRHIBufferUsage::Vertex;
        desc.cpuAccess = ixrhi::IXRHICpuAccess::Write;
        desc.debugName = "SelectionOutline VB";
        buffer = rhi.CreateBuffer(desc, nullptr, 0);
        if (!buffer)
            return false;
    }
    for (auto& buffer : m_uniformBuffers)
    {
        ixrhi::IXRHIBufferDesc desc;
        desc.sizeBytes = sizeof(UniformBlock);
        desc.usage = ixrhi::IXRHIBufferUsage::Uniform;
        desc.cpuAccess = ixrhi::IXRHICpuAccess::Write;
        desc.debugName = "SelectionOutline UBO";
        buffer = rhi.CreateBuffer(desc, nullptr, 0);
        if (!buffer)
            return false;
    }
    return true;
}

bool SelectionOutlineRenderer::CreateBindGroup(ixrhi::IXRHIDevice& rhi)
{
    const std::vector<ixrhi::IXRHIBinding> bindings = {
        {0, ixrhi::IXRHIBindingType::UniformBuffer, ixrhi::IXRHIShaderStage::Vertex},
    };
    m_bindLayout = rhi.CreateBindGroupLayout(bindings);
    if (!m_bindLayout)
        return false;
    m_bindGroup = rhi.CreateBindGroup(*m_bindLayout, kFramesInFlight);
    if (!m_bindGroup)
        return false;

    for (uint32_t i = 0; i < kFramesInFlight; ++i)
        m_bindGroup->UpdateBuffer(i, 0, m_uniformBuffers[i], 0, sizeof(UniformBlock));
    return true;
}

bool SelectionOutlineRenderer::CreatePipeline(ixrhi::IXRHIDevice& rhi)
{
    if (!m_assets)
        return false;

    auto vs = LoadShader(rhi,
        *m_assets,
        "assets/shaders/selection_outline_vs.spv",
        ixrhi::IXRHIShaderStage::Vertex,
        "VSMain");
    auto ps = LoadShader(rhi,
        *m_assets,
        "assets/shaders/selection_outline_ps.spv",
        ixrhi::IXRHIShaderStage::Fragment,
        "PSMain");
    if (!vs || !ps)
        return false;

    ixrhi::IXRHIGraphicsPipelineDesc desc;
    desc.vertexShader = vs;
    desc.fragmentShader = ps;
    desc.bindGroupLayouts = {m_bindLayout.get()};
    desc.vertexBindings = {{0, sizeof(Vertex)}};
    desc.vertexAttributes = {
        {0, 0, ixrhi::IXRHIFormat::R32G32B32Float, offsetof(Vertex, position)},
        {1, 0, ixrhi::IXRHIFormat::R32G32B32A32Float, offsetof(Vertex, color)},
    };
    desc.topology = ixrhi::IXRHIPrimitiveTopology::LineList;
    desc.cullMode = ixrhi::IXRHICullMode::None;
    desc.frontFace = ixrhi::IXRHIFrontFace::CounterClockwise;
    desc.depthTestEnable = false;
    desc.depthWriteEnable = false;
    desc.depthCompareOp = ixrhi::IXRHICompareOp::LessOrEqual;
    desc.blendAttachments = {{true,
        ixrhi::IXRHIBlendFactor::SrcAlpha,
        ixrhi::IXRHIBlendFactor::OneMinusSrcAlpha,
        ixrhi::IXRHIBlendOp::Add,
        ixrhi::IXRHIBlendFactor::One,
        ixrhi::IXRHIBlendFactor::OneMinusSrcAlpha,
        ixrhi::IXRHIBlendOp::Add}};
    desc.sampleCount = 1;
    desc.targetRenderPass = m_targetPass;
    desc.debugName = "SelectionOutline";

    auto pipeline = rhi.CreateGraphicsPipeline(desc);
    if (!pipeline)
        return false; // target pass torn down: caller treats as deferred retry
    m_pipeline = std::move(pipeline);
    return true;
}

void SelectionOutlineRenderer::DestroyPipeline()
{
    m_pipeline.reset();
}

void SelectionOutlineRenderer::UpdateUniform(uint32_t frameIndex, const WorldCamera& camera)
{
    const UniformBlock uniform{camera.viewProjection};
    m_uniformBuffers[frameIndex]->Write(0, &uniform, sizeof(uniform));
}
