// OffscreenSceneRenderer — IXRHI-native facade implementation. Same targets,
// same clear values, same snapshot sequencing, same composite (fullscreen
// triangle + Reinhard tone map) as before; all Vulkan objects live in the
// backend behind IXRHI types.

#include "OffscreenSceneRenderer.h"

#include "Debug.h"
#include "IXRHIShader.h"
#include "asset/IAssetReader.h"

#include <cstdlib>
#include <string>
#include <vector>

namespace
{

std::vector<std::uint32_t> ReadSpirv(client::asset::IAssetReader& assets, const std::string& path)
{
    auto bytes = assets.ReadAll(path);
    if (!bytes || bytes->empty() || bytes->size() % sizeof(std::uint32_t) != 0)
    {
        Tracenf("[OFFSCREEN] failed to open shader: %s", path.c_str());
        std::abort();
    }
    const auto* words = reinterpret_cast<const std::uint32_t*>(bytes->data());
    return std::vector<std::uint32_t>(words, words + bytes->size() / sizeof(std::uint32_t));
}

std::shared_ptr<ixrhi::IXRHITexture> CreateTargetTexture(ixrhi::IXRHIDevice& rhi,
                                                         std::uint32_t width,
                                                         std::uint32_t height,
                                                         ixrhi::IXRHIFormat format,
                                                         ixrhi::IXRHITextureUsage usage,
                                                         const std::string& debugName)
{
    ixrhi::IXRHITextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = format;
    desc.usage = usage;
    desc.debugName = debugName;
    return rhi.CreateTexture(desc, nullptr, 0);
}

} // namespace

bool OffscreenSceneRenderer::Create(ixrhi::IXRHIDevice& rhi,
                                    client::asset::IAssetReader& assets,
                                    std::uint32_t width,
                                    std::uint32_t height,
                                    ixrhi::IXRHIFormat colorFormat,
                                    ixrhi::IXRHIFormat depthFormat,
                                    const std::string& tag)
{
    Destroy();
    m_rhi = &rhi;
    m_assets = &assets;
    m_width = width;
    m_height = height;
    m_colorFormat = colorFormat;
    m_depthFormat = depthFormat;
    m_tag = tag.empty() ? "SceneView" : tag;

    if (m_width == 0 || m_height == 0 || m_colorFormat == ixrhi::IXRHIFormat::Undefined ||
        m_depthFormat == ixrhi::IXRHIFormat::Undefined)
        return false;

    m_ready = CreateTargets(rhi) && CreateComposite(rhi);
    if (m_ready)
    {
        Tracenf("[OFFSCREEN] Targets created: %ux%u, format=swapchain-compatible", m_width, m_height);
        Tracen("[OFFSCREEN] Composite-pass active: swap-chain blit + central Reinhard tone-mapping");
    }
    else
    {
        Tracen("[OFFSCREEN] Failed to initialize, falling back to direct swap-chain rendering");
        Destroy();
    }
    return m_ready;
}

bool OffscreenSceneRenderer::Recreate(ixrhi::IXRHIDevice& rhi,
                                      std::uint32_t width,
                                      std::uint32_t height,
                                      ixrhi::IXRHIFormat colorFormat,
                                      ixrhi::IXRHIFormat depthFormat)
{
    if (!m_assets)
        return false;
    rhi.WaitIdle();
    const std::string tag = m_tag;
    return Create(rhi, *m_assets, width, height, colorFormat, depthFormat, tag);
}

void OffscreenSceneRenderer::BeginMainPass(ixrhi::IXRHICommandList& cmd,
                                           const ixrhi::IXRHIFrameInfo& frame,
                                           bool clear)
{
    if (!m_ready || !frame.frameActive || m_passActive)
        return;

    m_activeTarget = clear ? m_clearTarget.get() : m_loadTarget.get();
    if (!m_activeTarget)
        return;
    m_activeTarget->Begin(cmd);
    m_passActive = true;
}

void OffscreenSceneRenderer::EndMainPass(ixrhi::IXRHICommandList& cmd)
{
    if (!m_ready || !m_passActive || !m_activeTarget)
        return;
    m_activeTarget->End(cmd);
    m_activeTarget = nullptr;
    m_passActive = false;
    // Producing-use states (the pass wrote color + depth; see header contract).
    m_colorState = ixrhi::IXRHIImageLayout::ColorAttachment;
    m_depthState = ixrhi::IXRHIImageLayout::DepthStencilAttachment;
}

void OffscreenSceneRenderer::SnapshotScene(ixrhi::IXRHICommandList& cmd,
                                            const ixrhi::IXRHIFrameInfo& frame)
{
    if (!m_ready || !frame.frameActive || m_passActive || !m_colorSnapshot || !m_depthSnapshot)
        return;

    using L = ixrhi::IXRHIImageLayout;
    cmd.TransitionTexture(*m_color, m_colorState, L::TransferSrc);
    cmd.TransitionTexture(*m_colorSnapshot, m_colorSnapshotState, L::TransferDst);
    cmd.CopyTexture(*m_color, *m_colorSnapshot);
    cmd.TransitionTexture(*m_color, L::TransferSrc, L::ShaderReadOnly);
    cmd.TransitionTexture(*m_colorSnapshot, L::TransferDst, L::ShaderReadOnly);
    m_colorState = L::ShaderReadOnly;
    m_colorSnapshotState = L::ShaderReadOnly;

    cmd.TransitionTexture(*m_depth, m_depthState, L::TransferSrc);
    cmd.TransitionTexture(*m_depthSnapshot, m_depthSnapshotState, L::TransferDst);
    cmd.CopyTexture(*m_depth, *m_depthSnapshot);
    cmd.TransitionTexture(*m_depth, L::TransferSrc, L::DepthStencilAttachment);
    cmd.TransitionTexture(*m_depthSnapshot, L::TransferDst, L::ShaderReadOnly);
    m_depthState = L::DepthStencilAttachment;
    m_depthSnapshotState = L::ShaderReadOnly;

    m_snapshotsReady = true;
}

void OffscreenSceneRenderer::RenderComposite(ixrhi::IXRHICommandList& cmd,
                                             const ixrhi::IXRHIFrameInfo& frame)
{
    if (!m_ready || !frame.frameActive || !m_compositePipeline || !m_bindGroup)
        return;

    cmd.SetGraphicsPipeline(*m_compositePipeline);
    cmd.BindGroup(0, *m_bindGroup, 0);
    cmd.Draw(3, 1, 0, 0);
}

void OffscreenSceneRenderer::Destroy()
{
    if (!m_rhi)
        return;

    m_compositePipeline.reset();
    m_bindGroup.reset();
    m_bindLayout.reset();
    m_compositeVs.reset();
    m_compositePs.reset();
    m_clearTarget.reset();
    m_loadTarget.reset();
    m_activeTarget = nullptr;
    m_sampler.reset();
    m_color.reset();
    m_depth.reset();
    m_colorSnapshot.reset();
    m_depthSnapshot.reset();

    m_colorState = ixrhi::IXRHIImageLayout::Undefined;
    m_depthState = ixrhi::IXRHIImageLayout::Undefined;
    m_colorSnapshotState = ixrhi::IXRHIImageLayout::Undefined;
    m_depthSnapshotState = ixrhi::IXRHIImageLayout::Undefined;

    m_rhi = nullptr;
    m_assets = nullptr;
    m_ready = false;
    m_passActive = false;
    m_snapshotsReady = false;
}

const ixrhi::IXRHIRenderPass* OffscreenSceneRenderer::GetTargetPass() const
{
    return m_clearTarget ? m_clearTarget->GetPass() : nullptr;
}

bool OffscreenSceneRenderer::CreateTargets(ixrhi::IXRHIDevice& rhi)
{
    using U = ixrhi::IXRHITextureUsage;
    m_color = CreateTargetTexture(rhi,
        m_width,
        m_height,
        m_colorFormat,
        U::ColorAttachment | U::Sampled | U::TransferSrc,
        m_tag + ".Color");
    m_depth = CreateTargetTexture(rhi,
        m_width,
        m_height,
        m_depthFormat,
        U::DepthStencilAttachment | U::Sampled | U::TransferSrc,
        m_tag + ".Depth");
    m_colorSnapshot = CreateTargetTexture(rhi,
        m_width,
        m_height,
        m_colorFormat,
        U::TransferDst | U::Sampled,
        m_tag + ".ColorSnapshot");
    m_depthSnapshot = CreateTargetTexture(rhi,
        m_width,
        m_height,
        m_depthFormat,
        U::TransferDst | U::Sampled,
        m_tag + ".DepthSnapshot");
    if (!m_color || !m_depth || !m_colorSnapshot || !m_depthSnapshot)
        return false;

    ixrhi::IXRHISamplerDesc samplerDesc;
    samplerDesc.minFilter = ixrhi::IXRHISamplerFilter::Linear;
    samplerDesc.magFilter = ixrhi::IXRHISamplerFilter::Linear;
    samplerDesc.mipmapFilter = ixrhi::IXRHISamplerFilter::Linear;
    samplerDesc.addressU = ixrhi::IXRHISamplerAddress::ClampToEdge;
    samplerDesc.addressV = ixrhi::IXRHISamplerAddress::ClampToEdge;
    samplerDesc.addressW = ixrhi::IXRHISamplerAddress::ClampToEdge;
    samplerDesc.maxLod = 1.0f;
    samplerDesc.debugName = m_tag + ".Sampler";
    m_sampler = rhi.CreateSampler(samplerDesc);
    if (!m_sampler)
        return false;

    ixrhi::IXRHIRenderTargetDesc clearDesc;
    clearDesc.color = m_color;
    clearDesc.depth = m_depth;
    clearDesc.colorLoad = ixrhi::IXRHILoadOp::Clear;
    clearDesc.colorStore = ixrhi::IXRHIStoreOp::Store;
    clearDesc.depthLoad = ixrhi::IXRHILoadOp::Clear;
    clearDesc.depthStore = ixrhi::IXRHIStoreOp::Store;
    clearDesc.clearColor[0] = 0.04f;
    clearDesc.clearColor[1] = 0.05f;
    clearDesc.clearColor[2] = 0.09f;
    clearDesc.clearColor[3] = 1.0f;
    clearDesc.clearDepth = 1.0f;
    clearDesc.debugName = m_tag + ".ClearTarget";
    m_clearTarget = rhi.CreateRenderTarget(clearDesc);

    ixrhi::IXRHIRenderTargetDesc loadDesc = clearDesc;
    loadDesc.colorLoad = ixrhi::IXRHILoadOp::Load;
    loadDesc.depthLoad = ixrhi::IXRHILoadOp::Load;
    loadDesc.debugName = m_tag + ".LoadTarget";
    m_loadTarget = rhi.CreateRenderTarget(loadDesc);

    return m_clearTarget != nullptr && m_loadTarget != nullptr;
}

bool OffscreenSceneRenderer::CreateComposite(ixrhi::IXRHIDevice& rhi)
{
    if (!m_assets)
        return false;

    ixrhi::IXRHIShaderDesc vsDesc;
    vsDesc.stage = ixrhi::IXRHIShaderStage::Vertex;
    vsDesc.entryPoint = "VSMain";
    vsDesc.spirv = ReadSpirv(*m_assets, "assets/shaders/composite_vs.spv");
    vsDesc.debugName = "Composite.VS";
    ixrhi::IXRHIShaderDesc psDesc;
    psDesc.stage = ixrhi::IXRHIShaderStage::Fragment;
    psDesc.entryPoint = "PSMain";
    psDesc.spirv = ReadSpirv(*m_assets, "assets/shaders/composite_ps.spv");
    psDesc.debugName = "Composite.PS";
    m_compositeVs = rhi.CreateShader(vsDesc);
    m_compositePs = rhi.CreateShader(psDesc);
    if (!m_compositeVs || !m_compositePs)
        return false;

    const std::vector<ixrhi::IXRHIBinding> bindings = {
        {0, ixrhi::IXRHIBindingType::SampledTexture, ixrhi::IXRHIShaderStage::Fragment},
    };
    m_bindLayout = rhi.CreateBindGroupLayout(bindings);
    if (!m_bindLayout)
        return false;
    m_bindGroup = rhi.CreateBindGroup(*m_bindLayout, 1);
    if (!m_bindGroup)
        return false;
    m_bindGroup->UpdateTexture(0, 0, m_color, m_sampler);

    ixrhi::IXRHIGraphicsPipelineDesc desc;
    desc.vertexShader = m_compositeVs;
    desc.fragmentShader = m_compositePs;
    desc.bindGroupLayouts = {m_bindLayout.get()};
    // No vertex buffers: procedural fullscreen triangle via SV_VertexID.
    desc.topology = ixrhi::IXRHIPrimitiveTopology::TriangleList;
    desc.cullMode = ixrhi::IXRHICullMode::None;
    desc.frontFace = ixrhi::IXRHIFrontFace::Clockwise;
    desc.depthTestEnable = false;
    desc.depthWriteEnable = false;
    desc.blendAttachments = {{false,
        ixrhi::IXRHIBlendFactor::One,
        ixrhi::IXRHIBlendFactor::Zero,
        ixrhi::IXRHIBlendOp::Add,
        ixrhi::IXRHIBlendFactor::One,
        ixrhi::IXRHIBlendFactor::Zero,
        ixrhi::IXRHIBlendOp::Add}};
    desc.sampleCount = 1;
    desc.targetRenderPass = nullptr; // swapchain pass (backend default, as before)
    desc.debugName = m_tag + ".Composite";
    m_compositePipeline = rhi.CreateGraphicsPipeline(desc);
    return m_compositePipeline != nullptr;
}
