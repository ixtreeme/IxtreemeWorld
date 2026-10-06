// OffscreenSceneRenderer — IXRHI-native facade implementation. A floating-point
// scene target with its snapshots, and the tone-map pass (fullscreen triangle,
// Composite.hlsl) into the display image or the swapchain; all Vulkan objects
// live in the backend behind IXRHI types.

#include "OffscreenSceneRenderer.h"

#include "Debug.h"
#include "SceneClearColor.h"
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
                                    ixrhi::IXRHIFormat displayFormat,
                                    ixrhi::IXRHIFormat depthFormat,
                                    const std::string& tag)
{
    Destroy();
    m_rhi = &rhi;
    m_assets = &assets;
    m_width = width;
    m_height = height;
    m_colorFormat = kSceneColorFormat;
    m_displayFormat = displayFormat;
    m_depthFormat = depthFormat;
    m_tag = tag.empty() ? "SceneView" : tag;

    if (m_width == 0 || m_height == 0 || m_displayFormat == ixrhi::IXRHIFormat::Undefined ||
        m_depthFormat == ixrhi::IXRHIFormat::Undefined)
        return false;

    m_ready = CreateTargets(rhi) && CreateComposite(rhi);
    if (m_ready)
        Tracenf("[OFFSCREEN] %s targets created: %ux%u, scene RGBA16F, tone-mapped display", m_tag.c_str(), m_width, m_height);
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
                                      ixrhi::IXRHIFormat displayFormat,
                                      ixrhi::IXRHIFormat depthFormat)
{
    if (!m_assets)
        return false;
    rhi.WaitIdle();
    const std::string tag = m_tag;
    return Create(rhi, *m_assets, width, height, displayFormat, depthFormat, tag);
}

void OffscreenSceneRenderer::BeginMainPass(ixrhi::IXRHICommandList& cmd,
                                           const ixrhi::IXRHIFrameInfo& frame,
                                           bool clear,
                                           bool storeDepth)
{
    if (!m_ready || !frame.frameActive || m_passActive)
        return;

    if (clear)
        m_activeTarget = storeDepth ? m_clearTarget.get() : m_clearTargetNoDepthStore.get();
    else
        m_activeTarget = storeDepth ? m_loadTarget.get() : m_loadTargetNoDepthStore.get();
    if (!m_activeTarget)
        return;
    m_activeTargetStoresDepth = storeDepth;
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
    // DontCare depth is undefined after the pass: snapshot copies must not read it.
    m_depthStoreValid = m_activeTargetStoresDepth;
    // The layouts the pass leaves them in (its final layouts): color shader-readable for the editor
    // panel and the composite, depth still an attachment.
    m_colorState = ixrhi::IXRHIImageLayout::ShaderReadOnly;
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

    // A DontCare depth pass leaves the depth image undefined: skip the copy and let the consumers
    // keep the previous frame's snapshot instead of sampling garbage.
    if (!m_depthStoreValid)
    {
        m_snapshotsReady = true;
        return;
    }

    cmd.TransitionTexture(*m_depth, m_depthState, L::TransferSrc);
    cmd.TransitionTexture(*m_depthSnapshot, m_depthSnapshotState, L::TransferDst);
    cmd.CopyTexture(*m_depth, *m_depthSnapshot);
    cmd.TransitionTexture(*m_depth, L::TransferSrc, L::DepthStencilAttachment);
    cmd.TransitionTexture(*m_depthSnapshot, L::TransferDst, L::ShaderReadOnly);
    m_depthState = L::DepthStencilAttachment;
    m_depthSnapshotState = L::ShaderReadOnly;

    m_snapshotsReady = true;
}

void OffscreenSceneRenderer::SnapshotDepth(ixrhi::IXRHICommandList& cmd,
                                           const ixrhi::IXRHIFrameInfo& frame)
{
    if (!m_ready || !frame.frameActive || m_passActive || !m_depthSnapshot)
        return;
    if (!m_depthStoreValid)
        return;  // the pass discarded the depth (DontCare); nothing valid to copy

    using L = ixrhi::IXRHIImageLayout;
    cmd.TransitionTexture(*m_depth, m_depthState, L::TransferSrc);
    cmd.TransitionTexture(*m_depthSnapshot, m_depthSnapshotState, L::TransferDst);
    cmd.CopyTexture(*m_depth, *m_depthSnapshot);
    cmd.TransitionTexture(*m_depth, L::TransferSrc, L::DepthStencilAttachment);
    cmd.TransitionTexture(*m_depthSnapshot, L::TransferDst, L::ShaderReadOnly);
    m_depthState = L::DepthStencilAttachment;
    m_depthSnapshotState = L::ShaderReadOnly;

    m_snapshotsReady = true;
}

void OffscreenSceneRenderer::BeginDisplayPass(ixrhi::IXRHICommandList& cmd,
                                              const ixrhi::IXRHIFrameInfo& frame,
                                              const ToneMapSettings& toneMap)
{
    if (!m_ready || !frame.frameActive || m_passActive || m_displayPassActive || !m_displayTarget ||
        !m_displayPipeline)
        return;
    // The scene's color is shader-readable here: EndMainPass leaves it so (the pass's final layout).
    m_displayTarget->Begin(cmd);
    m_displayPassActive = true;
    DrawToneMap(cmd, *m_displayPipeline, toneMap);
}

void OffscreenSceneRenderer::EndDisplayPass(ixrhi::IXRHICommandList& cmd)
{
    if (!m_displayPassActive)
        return;
    m_displayTarget->End(cmd);
    m_displayPassActive = false;
    m_displayReadable = true;
}

void OffscreenSceneRenderer::EnsureDisplayReadable(ixrhi::IXRHICommandList& cmd)
{
    if (!m_ready || m_displayReadable || m_passActive || m_displayPassActive || !m_displayTarget)
        return;
    m_displayTarget->Begin(cmd);  // clears it to black
    m_displayTarget->End(cmd);
    m_displayReadable = true;
}

void OffscreenSceneRenderer::RenderComposite(ixrhi::IXRHICommandList& cmd,
                                             const ixrhi::IXRHIFrameInfo& frame,
                                             const ToneMapSettings& toneMap)
{
    if (!m_ready || !frame.frameActive || !m_compositePipeline || !m_bindGroup)
        return;
    DrawToneMap(cmd, *m_compositePipeline, toneMap);
}

void OffscreenSceneRenderer::DrawToneMap(ixrhi::IXRHICommandList& cmd,
                                         const ixrhi::IXRHIGraphicsPipeline& pipeline,
                                         const ToneMapSettings& toneMap)
{
    // Composite.hlsl ToneMapParams.
    struct
    {
        float exposure;
        std::int32_t mode;
        float padding[2];
    } params{toneMap.exposure, toneMap.mode, {0.0f, 0.0f}};
    cmd.SetGraphicsPipeline(pipeline);
    cmd.BindGroup(0, *m_bindGroup, 0);
    cmd.PushConstants(&params, sizeof(params));
    cmd.Draw(3, 1, 0, 0);
}

void OffscreenSceneRenderer::Destroy()
{
    if (!m_rhi)
        return;

    m_compositePipeline.reset();
    m_displayPipeline.reset();
    m_bindGroup.reset();
    m_bindLayout.reset();
    m_compositeVs.reset();
    m_compositePs.reset();
    m_clearTarget.reset();
    m_loadTarget.reset();
    m_clearTargetNoDepthStore.reset();
    m_loadTargetNoDepthStore.reset();
    m_displayTarget.reset();
    m_activeTarget = nullptr;
    m_activeTargetStoresDepth = true;
    m_depthStoreValid = false;
    m_sampler.reset();
    m_color.reset();
    m_depth.reset();
    m_colorSnapshot.reset();
    m_depthSnapshot.reset();
    m_display.reset();

    m_colorState = ixrhi::IXRHIImageLayout::Undefined;
    m_depthState = ixrhi::IXRHIImageLayout::Undefined;
    m_colorSnapshotState = ixrhi::IXRHIImageLayout::Undefined;
    m_depthSnapshotState = ixrhi::IXRHIImageLayout::Undefined;

    m_rhi = nullptr;
    m_assets = nullptr;
    m_ready = false;
    m_passActive = false;
    m_displayPassActive = false;
    m_displayReadable = false;
    m_snapshotsReady = false;
}

const ixrhi::IXRHIRenderPass* OffscreenSceneRenderer::GetTargetPass() const
{
    return m_clearTarget ? m_clearTarget->GetPass() : nullptr;
}

const ixrhi::IXRHIRenderPass* OffscreenSceneRenderer::GetDisplayPass() const
{
    return m_displayTarget ? m_displayTarget->GetPass() : nullptr;
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
    m_display = CreateTargetTexture(rhi,
        m_width,
        m_height,
        m_displayFormat,
        U::ColorAttachment | U::Sampled,
        m_tag + ".Display");
    if (!m_color || !m_depth || !m_colorSnapshot || !m_depthSnapshot || !m_display)
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
    clearDesc.clearColor[0] = kSceneClearColor[0];
    clearDesc.clearColor[1] = kSceneClearColor[1];
    clearDesc.clearColor[2] = kSceneClearColor[2];
    clearDesc.clearColor[3] = kSceneClearColor[3];
    clearDesc.clearDepth = 1.0f;
    clearDesc.debugName = m_tag + ".ClearTarget";
    m_clearTarget = rhi.CreateRenderTarget(clearDesc);

    ixrhi::IXRHIRenderTargetDesc loadDesc = clearDesc;
    loadDesc.colorLoad = ixrhi::IXRHILoadOp::Load;
    loadDesc.depthLoad = ixrhi::IXRHILoadOp::Load;
    loadDesc.debugName = m_tag + ".LoadTarget";
    m_loadTarget = rhi.CreateRenderTarget(loadDesc);

    // DontCare-depth variants: the depth is written for depth testing but its contents are dropped
    // at pass end, saving a full-resolution depth store on frames nothing reads the snapshot.
    ixrhi::IXRHIRenderTargetDesc noStoreClearDesc = clearDesc;
    noStoreClearDesc.depthStore = ixrhi::IXRHIStoreOp::DontCare;
    noStoreClearDesc.debugName = m_tag + ".ClearTargetNoDepthStore";
    m_clearTargetNoDepthStore = rhi.CreateRenderTarget(noStoreClearDesc);

    ixrhi::IXRHIRenderTargetDesc noStoreLoadDesc = loadDesc;
    noStoreLoadDesc.depthStore = ixrhi::IXRHIStoreOp::DontCare;
    noStoreLoadDesc.debugName = m_tag + ".LoadTargetNoDepthStore";
    m_loadTargetNoDepthStore = rhi.CreateRenderTarget(noStoreLoadDesc);

    // The tone-mapped image (no depth: its overlays draw over everything). Cleared to black: only
    // EnsureDisplayReadable keeps that, the tone map covers every pixel.
    ixrhi::IXRHIRenderTargetDesc displayDesc;
    displayDesc.color = m_display;
    displayDesc.colorLoad = ixrhi::IXRHILoadOp::Clear;
    displayDesc.colorStore = ixrhi::IXRHIStoreOp::Store;
    displayDesc.clearColor[0] = 0.0f;
    displayDesc.clearColor[1] = 0.0f;
    displayDesc.clearColor[2] = 0.0f;
    displayDesc.clearColor[3] = 1.0f;
    displayDesc.debugName = m_tag + ".DisplayTarget";
    m_displayTarget = rhi.CreateRenderTarget(displayDesc);

    return m_clearTarget != nullptr && m_loadTarget != nullptr &&
        m_clearTargetNoDepthStore != nullptr && m_loadTargetNoDepthStore != nullptr &&
        m_displayTarget != nullptr;
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
    desc.pushRanges = {{ixrhi::IXRHIShaderStage::Fragment, 0, 16}};  // ToneMapParams
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
    desc.targetRenderPass = m_displayTarget->GetPass();
    desc.debugName = m_tag + ".ToneMap";
    m_displayPipeline = rhi.CreateGraphicsPipeline(desc);
    return m_compositePipeline != nullptr && m_displayPipeline != nullptr;
}
