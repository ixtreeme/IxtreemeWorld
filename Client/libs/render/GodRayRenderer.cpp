#include "GodRayRenderer.h"

#include "Debug.h"
#include "IXRHIShader.h"
#include "asset/IAssetReader.h"
#include "math/IXMath.h"
#include "math/WorldMath.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace
{
namespace xm = ixtreeme::math;

// The mask can be brighter than 1 (the sun's core): a float format keeps that through the blur.
constexpr ixrhi::IXRHIFormat kRayFormat = ixrhi::IXRHIFormat::R16G16B16A16Float;

std::shared_ptr<ixrhi::IXRHIShader> LoadShader(ixrhi::IXRHIDevice& rhi,
                                               client::asset::IAssetReader& assets,
                                               const std::string& path,
                                               ixrhi::IXRHIShaderStage stage,
                                               const char* entry)
{
    auto bytes = assets.ReadAll(path);
    if (!bytes || bytes->empty() || bytes->size() % sizeof(std::uint32_t) != 0)
    {
        Tracenf("[GOD-RAYS] failed to open shader: %s", path.c_str());
        return nullptr;
    }
    ixrhi::IXRHIShaderDesc desc;
    desc.stage = stage;
    desc.entryPoint = entry;
    const auto* words = reinterpret_cast<const std::uint32_t*>(bytes->data());
    desc.spirv.assign(words, words + bytes->size() / sizeof(std::uint32_t));
    desc.debugName = path;
    return rhi.CreateShader(desc);
}

std::shared_ptr<ixrhi::IXRHITexture> CreateRayTexture(ixrhi::IXRHIDevice& rhi,
                                                      std::uint32_t width,
                                                      std::uint32_t height,
                                                      const char* name)
{
    ixrhi::IXRHITextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = kRayFormat;
    desc.usage = ixrhi::IXRHITextureUsage::ColorAttachment | ixrhi::IXRHITextureUsage::Sampled;
    desc.debugName = name;
    return rhi.CreateTexture(desc, nullptr, 0);
}

std::unique_ptr<ixrhi::IXRHIRenderTarget> CreateRayTarget(ixrhi::IXRHIDevice& rhi,
                                                          const std::shared_ptr<ixrhi::IXRHITexture>& color,
                                                          const char* name)
{
    ixrhi::IXRHIRenderTargetDesc desc;
    desc.color = color;
    desc.colorLoad = ixrhi::IXRHILoadOp::Clear;
    desc.colorStore = ixrhi::IXRHIStoreOp::Store;
    desc.clearColor[0] = desc.clearColor[1] = desc.clearColor[2] = 0.0f;
    desc.clearColor[3] = 1.0f;
    desc.debugName = name;
    return rhi.CreateRenderTarget(desc);
}

int SamplesForQuality(std::int32_t quality)
{
    return quality <= 0 ? 32 : quality == 1 ? 64 : 96;
}
} // namespace

bool GodRayRenderer::Create(ixrhi::IXRHIDevice& rhi, client::asset::IAssetReader& assets)
{
    Destroy();
    m_rhi = &rhi;
    m_assets = &assets;

    m_vs = LoadShader(rhi, assets, "assets/shaders/god_rays_vs.spv", ixrhi::IXRHIShaderStage::Vertex, "VSMain");
    m_maskPs = LoadShader(rhi, assets, "assets/shaders/god_rays_mask_ps.spv", ixrhi::IXRHIShaderStage::Fragment, "MaskPS");
    m_blurPs = LoadShader(rhi, assets, "assets/shaders/god_rays_blur_ps.spv", ixrhi::IXRHIShaderStage::Fragment, "BlurPS");
    m_compositePs = LoadShader(rhi, assets, "assets/shaders/god_rays_composite_ps.spv",
        ixrhi::IXRHIShaderStage::Fragment, "CompositePS");

    ixrhi::IXRHISamplerDesc sampler;
    sampler.debugName = "GodRays:Sampler";
    m_sampler = rhi.CreateSampler(sampler);
    const std::vector<ixrhi::IXRHIBinding> bindings = {
        {0, ixrhi::IXRHIBindingType::SampledTexture, ixrhi::IXRHIShaderStage::Fragment},
    };
    m_bindLayout = rhi.CreateBindGroupLayout(bindings);
    if (m_bindLayout)
    {
        m_maskBindings = rhi.CreateBindGroup(*m_bindLayout, kViews * kFramesInFlight);
        m_blurBindings = rhi.CreateBindGroup(*m_bindLayout, kViews * kFramesInFlight);
        m_compositeBindings = rhi.CreateBindGroup(*m_bindLayout, kViews * kFramesInFlight);
    }
    m_prototypeTexture = CreateRayTexture(rhi, 1, 1, "GodRays:Prototype");
    if (m_prototypeTexture)
        m_prototypeTarget = CreateRayTarget(rhi, m_prototypeTexture, "GodRays:Prototype");

    const bool resources = m_vs && m_maskPs && m_blurPs && m_compositePs && m_sampler && m_bindLayout &&
        m_maskBindings && m_blurBindings && m_compositeBindings && m_prototypeTarget;
    if (resources)
    {
        m_maskPipeline = BuildPipeline(m_maskPs, m_prototypeTarget->GetPass(), false, "GodRays:Mask");
        m_blurPipeline = BuildPipeline(m_blurPs, m_prototypeTarget->GetPass(), false, "GodRays:Blur");
        m_compositePipeline = BuildPipeline(m_compositePs, m_targetPass, true, "GodRays:Composite");
    }
    const bool pipelines = m_maskPipeline && m_blurPipeline && m_compositePipeline;
    Tracenf("[GOD-RAYS] Create: resources=%d pipelines=%d", resources ? 1 : 0, pipelines ? 1 : 0);
    if (resources && pipelines)
        return true;
    Destroy();
    return false;
}

bool GodRayRenderer::RecreatePipeline(ixrhi::IXRHIDevice& rhi)
{
    if (!m_rhi)
        return true;
    m_rhi = &rhi;
    // Only the composite bakes against the borrowed scene pass; mask / blur use the prototype.
    m_compositePipeline = BuildPipeline(m_compositePs, m_targetPass, true, "GodRays:Composite");
    return true;
}

std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> GodRayRenderer::BuildPipeline(
    const std::shared_ptr<ixrhi::IXRHIShader>& ps,
    const ixrhi::IXRHIRenderPass* pass,
    bool additive,
    const char* name)
{
    if (!m_rhi || !m_vs || !ps || !m_bindLayout)
        return nullptr;
    ixrhi::IXRHIGraphicsPipelineDesc desc;
    desc.vertexShader = m_vs;
    desc.fragmentShader = ps;
    desc.bindGroupLayouts = {m_bindLayout.get()};
    desc.pushRanges = {{ixrhi::IXRHIShaderStage::Fragment, 0, sizeof(Push)}};
    desc.topology = ixrhi::IXRHIPrimitiveTopology::TriangleList;
    desc.cullMode = ixrhi::IXRHICullMode::None;
    desc.depthTestEnable = false;
    desc.depthWriteEnable = false;
    if (additive)
    {
        // Light adds to the image; the image's alpha stays.
        desc.blendAttachments = {{true,
            ixrhi::IXRHIBlendFactor::One,
            ixrhi::IXRHIBlendFactor::One,
            ixrhi::IXRHIBlendOp::Add,
            ixrhi::IXRHIBlendFactor::Zero,
            ixrhi::IXRHIBlendFactor::One,
            ixrhi::IXRHIBlendOp::Add}};
    }
    else
    {
        desc.blendAttachments = {ixrhi::IXRHIBlendAttachment{}};
    }
    desc.sampleCount = 1;
    desc.targetRenderPass = pass;
    desc.debugName = name;
    return m_rhi->CreateGraphicsPipeline(desc);
}

const char* GodRayRenderer::BuildPush(const SkySettings& sky,
                                      const LightingState& lighting,
                                      const WorldCamera& camera,
                                      Push& out) const
{
    const DirectionalLight& sun = lighting.directional;
    if (!sky.godRays || !sun.enabled || sky.godRayIntensity <= 0.0f)
        return "off";
    const WorldVec3 sunDir = WorldDirectionFromAzimuthElevation(
        xm::DegreesToRadians(sun.azimuthDegrees),
        xm::DegreesToRadians(std::clamp(sun.elevationDegrees, -90.0f, 90.0f)));
    // Fade in from the horizon: no rays from a sun that has set.
    const float height = std::clamp((sunDir.y + 0.02f) / 0.12f, 0.0f, 1.0f);
    if (height <= 0.0f)
        return "off: the sun is under the horizon";

    // The sun's place on the screen: a point far along its direction, through the view-projection
    // (row vectors: clip = p * M).
    const float* m = camera.viewProjection.m;
    const float far = std::max(1.0f, camera.farPlane * 0.5f);
    const float p[3] = {camera.eye.x + sunDir.x * far, camera.eye.y + sunDir.y * far, camera.eye.z + sunDir.z * far};
    float clip[4];
    for (int c = 0; c < 4; ++c)
        clip[c] = p[0] * m[c] + p[1] * m[4 + c] + p[2] * m[8 + c] + m[12 + c];
    if (clip[3] <= 1e-4f)
        return "off: the sun is behind the camera";
    const float sunU = clip[0] / clip[3] * 0.5f + 0.5f;
    const float sunV = clip[1] / clip[3] * 0.5f + 0.5f;
    // Full strength while the sun is on the screen, fading out over a third of a screen beyond it.
    const float outside = std::max({0.0f, -sunU, sunU - 1.0f, -sunV, sunV - 1.0f});
    const float onScreen = std::clamp(1.0f - outside / 0.35f, 0.0f, 1.0f);
    if (onScreen <= 0.0f)
        return "off: the sun is off the screen";

    xm::Mat4 inverse;
    if (!xm::Inverse(camera.viewProjection, inverse))
        return "off: the view-projection cannot be inverted";
    std::memcpy(out.invViewProjection, inverse.m, sizeof(out.invViewProjection));
    out.sunDir[0] = sunDir.x;
    out.sunDir[1] = sunDir.y;
    out.sunDir[2] = sunDir.z;
    out.sunDir[3] = height * onScreen;
    const float strength = std::clamp(sun.intensity, 0.0f, 1.0f);
    out.sunColor[0] = std::max(0.0f, sun.r) * strength;
    out.sunColor[1] = std::max(0.0f, sun.g) * strength;
    out.sunColor[2] = std::max(0.0f, sun.b) * strength;
    out.sunColor[3] = 0.0f;
    out.rayParams[0] = sunU;
    out.rayParams[1] = sunV;
    out.rayParams[2] = std::clamp(sky.godRayLength, 0.05f, 1.0f);
    out.rayParams[3] = std::clamp(sky.godRayFalloff, 0.8f, 1.0f);
    out.composite[0] = std::max(0.0f, sky.godRayIntensity);
    out.composite[1] = static_cast<float>(SamplesForQuality(sky.godRayQuality));
    out.composite[2] = 0.0f;
    out.composite[3] = 0.0f;
    return nullptr;
}

bool GodRayRenderer::IsVisible(const SkySettings& sky, const LightingState& lighting, const WorldCamera& camera) const
{
    Push push{};
    return m_rhi && BuildPush(sky, lighting, camera, push) == nullptr;
}

bool GodRayRenderer::EnsureViewTargets(ViewTargets& targets, std::uint32_t width, std::uint32_t height)
{
    const std::uint32_t halfWidth = std::max(1u, width / 2u);
    const std::uint32_t halfHeight = std::max(1u, height / 2u);
    if (targets.maskTarget && targets.blurTarget && targets.width == halfWidth && targets.height == halfHeight)
        return true;
    // The old targets may still be read by frames in flight.
    if (targets.maskTarget || targets.blurTarget)
        m_rhi->WaitIdle();
    const char* lastState = targets.lastState;
    targets = ViewTargets{};
    targets.lastState = lastState;
    targets.mask = CreateRayTexture(*m_rhi, halfWidth, halfHeight, "GodRays:Mask");
    targets.blur = CreateRayTexture(*m_rhi, halfWidth, halfHeight, "GodRays:Blur");
    if (targets.mask)
        targets.maskTarget = CreateRayTarget(*m_rhi, targets.mask, "GodRays:Mask");
    if (targets.blur)
        targets.blurTarget = CreateRayTarget(*m_rhi, targets.blur, "GodRays:Blur");
    if (!targets.maskTarget || !targets.blurTarget)
    {
        targets = ViewTargets{};
        return false;
    }
    targets.width = halfWidth;
    targets.height = halfHeight;
    return true;
}

bool GodRayRenderer::RenderRays(ixrhi::IXRHICommandList& cmd,
                                const ixrhi::IXRHIFrameInfo& frame,
                                std::uint32_t view,
                                const SkySettings& sky,
                                const LightingState& lighting,
                                const WorldCamera& camera,
                                const std::shared_ptr<ixrhi::IXRHITexture>& depth,
                                std::uint32_t width,
                                std::uint32_t height)
{
    if (view >= kViews)
        return false;
    ViewTargets& targets = m_views[view];
    targets.ready = false;
    const auto report = [&](const char* state) {
        if (targets.lastState != state)
        {
            Tracenf("[GOD-RAYS] view=%u %s", view, state);
            targets.lastState = state;
        }
    };
    if (!m_rhi || !frame.frameActive || !depth || width == 0 || height == 0 ||
        !m_maskPipeline || !m_blurPipeline)
    {
        report("skipped: no depth snapshot, size or pipeline");
        return false;
    }
    Push push{};
    // Off (disabled, sun set or off screen) is not logged: it flips whenever the camera turns.
    if (BuildPush(sky, lighting, camera, push) != nullptr)
        return false;
    if (!EnsureViewTargets(targets, width, height))
    {
        report("skipped: render targets could not be created");
        return false;
    }
    report("drawing");

    const std::uint32_t set = view * kFramesInFlight + frame.frameIndex % kFramesInFlight;
    m_maskBindings->UpdateTexture(set, 0, depth, m_sampler);
    m_blurBindings->UpdateTexture(set, 0, targets.mask, m_sampler);
    m_compositeBindings->UpdateTexture(set, 0, targets.blur, m_sampler);

    // The mask: the sun where the sky shows.
    targets.maskTarget->Begin(cmd);
    cmd.SetViewport(0.0f, 0.0f, static_cast<float>(targets.width), static_cast<float>(targets.height));
    cmd.SetScissor(0, 0, targets.width, targets.height);
    cmd.SetGraphicsPipeline(*m_maskPipeline);
    cmd.BindGroup(0, *m_maskBindings, set);
    cmd.PushConstants(&push, sizeof(push));
    cmd.Draw(3, 1, 0, 0);
    targets.maskTarget->End(cmd);

    // The shafts: the mask blurred towards the sun.
    targets.blurTarget->Begin(cmd);
    cmd.SetViewport(0.0f, 0.0f, static_cast<float>(targets.width), static_cast<float>(targets.height));
    cmd.SetScissor(0, 0, targets.width, targets.height);
    cmd.SetGraphicsPipeline(*m_blurPipeline);
    cmd.BindGroup(0, *m_blurBindings, set);
    cmd.PushConstants(&push, sizeof(push));
    cmd.Draw(3, 1, 0, 0);
    targets.blurTarget->End(cmd);

    targets.push = push;
    targets.ready = true;
    return true;
}

void GodRayRenderer::Composite(ixrhi::IXRHICommandList& cmd,
                               const ixrhi::IXRHIFrameInfo& frame,
                               std::uint32_t view,
                               std::uint32_t width,
                               std::uint32_t height)
{
    if (view >= kViews || !m_compositePipeline || !frame.frameActive || width == 0 || height == 0)
        return;
    ViewTargets& targets = m_views[view];
    if (!targets.ready)
        return;
    const std::uint32_t set = view * kFramesInFlight + frame.frameIndex % kFramesInFlight;
    cmd.SetViewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    cmd.SetScissor(0, 0, width, height);
    cmd.SetGraphicsPipeline(*m_compositePipeline);
    cmd.BindGroup(0, *m_compositeBindings, set);
    cmd.PushConstants(&targets.push, sizeof(targets.push));
    cmd.Draw(3, 1, 0, 0);
    targets.ready = false;
}

void GodRayRenderer::Destroy()
{
    m_maskPipeline.reset();
    m_blurPipeline.reset();
    m_compositePipeline.reset();
    for (ViewTargets& targets : m_views)
        targets = ViewTargets{};
    m_prototypeTarget.reset();
    m_prototypeTexture.reset();
    m_maskBindings.reset();
    m_blurBindings.reset();
    m_compositeBindings.reset();
    m_bindLayout.reset();
    m_sampler.reset();
    m_vs.reset();
    m_maskPs.reset();
    m_blurPs.reset();
    m_compositePs.reset();
    m_rhi = nullptr;
    m_assets = nullptr;
}
