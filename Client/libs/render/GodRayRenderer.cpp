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
constexpr std::uint32_t kCascadeBufferBytes = sizeof(float) * 16u * GodRayRenderer::kCascades;

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

int StepsForQuality(std::int32_t quality)
{
    return quality <= 0 ? 16 : quality == 1 ? 32 : 64;
}

WorldVec3 SunDirection(const DirectionalLight& sun)
{
    return WorldDirectionFromAzimuthElevation(
        xm::DegreesToRadians(sun.azimuthDegrees),
        xm::DegreesToRadians(std::clamp(sun.elevationDegrees, -90.0f, 90.0f)));
}

// 0 below the horizon, fading in just above it: no rays from a sun that has set.
float SunHeightFade(const WorldVec3& sunDir)
{
    return std::clamp((sunDir.y + 0.02f) / 0.12f, 0.0f, 1.0f);
}
} // namespace

bool GodRayRenderer::Create(ixrhi::IXRHIDevice& rhi, client::asset::IAssetReader& assets)
{
    Destroy();
    m_rhi = &rhi;
    m_assets = &assets;

    using S = ixrhi::IXRHIShaderStage;
    m_vs = LoadShader(rhi, assets, "assets/shaders/god_rays_vs.spv", S::Vertex, "VSMain");
    m_maskPs = LoadShader(rhi, assets, "assets/shaders/god_rays_mask_ps.spv", S::Fragment, "MaskPS");
    m_blurPs = LoadShader(rhi, assets, "assets/shaders/god_rays_blur_ps.spv", S::Fragment, "BlurPS");
    m_volumeBlurPs = LoadShader(rhi, assets, "assets/shaders/god_rays_volblur_ps.spv", S::Fragment, "VolBlurPS");
    m_marchPs = LoadShader(rhi, assets, "assets/shaders/volumetric_light_ps.spv", S::Fragment, "MarchPS");

    ixrhi::IXRHISamplerDesc sampler;
    sampler.debugName = "GodRays:Sampler";
    m_sampler = rhi.CreateSampler(sampler);
    ixrhi::IXRHISamplerDesc shadowSampler;
    shadowSampler.addressU = ixrhi::IXRHISamplerAddress::ClampToBorder;
    shadowSampler.addressV = ixrhi::IXRHISamplerAddress::ClampToBorder;
    shadowSampler.addressW = ixrhi::IXRHISamplerAddress::ClampToBorder;
    shadowSampler.compareEnable = true;
    shadowSampler.compareOp = ixrhi::IXRHICompareOp::LessOrEqual;
    shadowSampler.maxLod = 0.0f;
    shadowSampler.debugName = "GodRays:ShadowSampler";
    m_shadowSampler = rhi.CreateSampler(shadowSampler);

    using B = ixrhi::IXRHIBindingType;
    m_bindLayout = rhi.CreateBindGroupLayout({{0, B::SampledTexture, S::Fragment}});
    m_marchLayout = rhi.CreateBindGroupLayout({
        {0, B::SampledTexture, S::Fragment},
        {1, B::SampledTexture, S::Fragment},
        {2, B::UniformBuffer, S::Fragment},
    });
    const std::uint32_t sets = kViews * kFramesInFlight;
    if (m_bindLayout)
    {
        m_maskBindings = rhi.CreateBindGroup(*m_bindLayout, sets);
        m_blurBindings = rhi.CreateBindGroup(*m_bindLayout, sets);
        m_volumeBlurBindings = rhi.CreateBindGroup(*m_bindLayout, sets);
    }
    if (m_marchLayout)
        m_marchBindings = rhi.CreateBindGroup(*m_marchLayout, sets);
    bool buffers = true;
    for (auto& buffer : m_cascadeBuffers)
    {
        ixrhi::IXRHIBufferDesc desc;
        desc.sizeBytes = kCascadeBufferBytes;
        desc.usage = ixrhi::IXRHIBufferUsage::Uniform;
        desc.cpuAccess = ixrhi::IXRHICpuAccess::Write;
        desc.debugName = "GodRays:Cascades";
        buffer = rhi.CreateBuffer(desc, nullptr, 0);
        buffers = buffers && buffer != nullptr;
    }
    m_prototypeTexture = CreateRayTexture(rhi, 1, 1, "GodRays:Prototype");
    if (m_prototypeTexture)
        m_prototypeTarget = CreateRayTarget(rhi, m_prototypeTexture, "GodRays:Prototype");

    const bool resources = m_vs && m_maskPs && m_blurPs && m_volumeBlurPs && m_marchPs &&
        m_sampler && m_shadowSampler && m_bindLayout && m_marchLayout && m_maskBindings && m_blurBindings &&
        m_volumeBlurBindings && m_marchBindings && buffers &&
        m_prototypeTarget;
    if (resources)
    {
        const ixrhi::IXRHIRenderPass* rayPass = m_prototypeTarget->GetPass();
        m_maskPipeline = BuildPipeline(m_maskPs, *m_bindLayout, sizeof(Push), rayPass, "GodRays:Mask");
        m_blurPipeline = BuildPipeline(m_blurPs, *m_bindLayout, sizeof(Push), rayPass, "GodRays:Blur");
        m_volumeBlurPipeline = BuildPipeline(m_volumeBlurPs, *m_bindLayout, sizeof(Push), rayPass, "GodRays:VolumeBlur");
        m_marchPipeline = BuildPipeline(m_marchPs, *m_marchLayout, sizeof(VolumetricPush), rayPass, "GodRays:Volumetric");
    }
    const bool pipelines = m_maskPipeline && m_blurPipeline && m_volumeBlurPipeline && m_marchPipeline;
    Tracenf("[GOD-RAYS] Create: resources=%d pipelines=%d", resources ? 1 : 0, pipelines ? 1 : 0);
    if (resources && pipelines)
        return true;
    Destroy();
    return false;
}

std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> GodRayRenderer::BuildPipeline(
    const std::shared_ptr<ixrhi::IXRHIShader>& ps,
    const ixrhi::IXRHIBindGroupLayout& layout,
    std::uint32_t pushBytes,
    const ixrhi::IXRHIRenderPass* pass,
    const char* name)
{
    if (!m_rhi || !m_vs || !ps)
        return nullptr;
    ixrhi::IXRHIGraphicsPipelineDesc desc;
    desc.vertexShader = m_vs;
    desc.fragmentShader = ps;
    desc.bindGroupLayouts = {&layout};
    desc.pushRanges = {{ixrhi::IXRHIShaderStage::Fragment, 0, pushBytes}};
    desc.topology = ixrhi::IXRHIPrimitiveTopology::TriangleList;
    desc.cullMode = ixrhi::IXRHICullMode::None;
    desc.depthTestEnable = false;
    desc.depthWriteEnable = false;
    desc.blendAttachments = {ixrhi::IXRHIBlendAttachment{}};
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
    if (!sky.ScreenSpaceGodRays() || !sun.enabled || sky.godRayIntensity <= 0.0f)
        return "off";
    const WorldVec3 sunDir = SunDirection(sun);
    const float height = SunHeightFade(sunDir);
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

const char* GodRayRenderer::BuildVolumetricPush(const SkySettings& sky,
                                                const LightingState& lighting,
                                                const WorldCamera& camera,
                                                const SunShadow& shadow,
                                                VolumetricPush& out) const
{
    const DirectionalLight& sun = lighting.directional;
    if (!sky.VolumetricGodRays() || !sun.enabled || sky.volumetricIntensity <= 0.0f || sky.volumetricDensity <= 0.0f)
        return "off";
    const WorldVec3 sunDir = SunDirection(sun);
    const float height = SunHeightFade(sunDir);
    if (height <= 0.0f)
        return "off: the sun is under the horizon";
    if (!shadow.texture || !shadow.cascadeViewProj)
        return "skipped: no sun shadow map (it needs a terrain and Environment > Sun > Casts shadows)";

    xm::Mat4 inverse;
    if (!xm::Inverse(camera.viewProjection, inverse))
        return "off: the view-projection cannot be inverted";
    std::memcpy(out.invViewProjection, inverse.m, sizeof(out.invViewProjection));
    out.cameraPos[0] = camera.eye.x;
    out.cameraPos[1] = camera.eye.y;
    out.cameraPos[2] = camera.eye.z;
    out.cameraPos[3] = std::clamp(sky.volumetricDistance, 5.0f, 200.0f);
    out.sunDir[0] = sunDir.x;
    out.sunDir[1] = sunDir.y;
    out.sunDir[2] = sunDir.z;
    out.sunDir[3] = std::clamp(sky.volumetricDensity, 0.0f, 0.2f);
    const float strength = std::clamp(sun.intensity, 0.0f, 1.0f) * std::max(0.0f, sky.volumetricIntensity) * height;
    out.sunColor[0] = std::max(0.0f, sun.r) * strength;
    out.sunColor[1] = std::max(0.0f, sun.g) * strength;
    out.sunColor[2] = std::max(0.0f, sun.b) * strength;
    out.sunColor[3] = std::clamp(sky.volumetricAnisotropy, 0.0f, 0.95f);
    out.params[0] = static_cast<float>(StepsForQuality(sky.volumetricQuality));
    out.params[1] = shadow.depthBias;
    out.params[2] = 0.0f;
    out.params[3] = 0.0f;
    return nullptr;
}

bool GodRayRenderer::IsVisible(const SkySettings& sky,
                               const LightingState& lighting,
                               const WorldCamera& camera,
                               const SunShadow& shadow) const
{
    if (!m_rhi)
        return false;
    Push push{};
    VolumetricPush volumetric{};
    return BuildPush(sky, lighting, camera, push) == nullptr ||
        BuildVolumetricPush(sky, lighting, camera, shadow, volumetric) == nullptr;
}

bool GodRayRenderer::EnsureViewTargets(ViewTargets& targets, std::uint32_t width, std::uint32_t height)
{
    const std::uint32_t rayWidth = std::max(1u, width / 4u);
    const std::uint32_t rayHeight = std::max(1u, height / 4u);
    if (targets.maskTarget && targets.blurTarget && targets.volumeTarget && targets.volumeBlurTarget &&
        targets.width == rayWidth && targets.height == rayHeight)
        return true;
    // The old targets may still be read by frames in flight.
    if (targets.maskTarget || targets.blurTarget || targets.volumeTarget || targets.volumeBlurTarget)
        m_rhi->WaitIdle();
    const char* lastScreenSpaceState = targets.lastScreenSpaceState;
    const char* lastVolumetricState = targets.lastVolumetricState;
    targets = ViewTargets{};
    targets.lastScreenSpaceState = lastScreenSpaceState;
    targets.lastVolumetricState = lastVolumetricState;
    targets.mask = CreateRayTexture(*m_rhi, rayWidth, rayHeight, "GodRays:Mask");
    targets.blur = CreateRayTexture(*m_rhi, rayWidth, rayHeight, "GodRays:Blur");
    targets.volume = CreateRayTexture(*m_rhi, rayWidth, rayHeight, "GodRays:Volume");
    targets.volumeBlur = CreateRayTexture(*m_rhi, rayWidth, rayHeight, "GodRays:VolumeBlur");
    if (targets.mask)
        targets.maskTarget = CreateRayTarget(*m_rhi, targets.mask, "GodRays:Mask");
    if (targets.blur)
        targets.blurTarget = CreateRayTarget(*m_rhi, targets.blur, "GodRays:Blur");
    if (targets.volume)
        targets.volumeTarget = CreateRayTarget(*m_rhi, targets.volume, "GodRays:Volume");
    if (targets.volumeBlur)
        targets.volumeBlurTarget = CreateRayTarget(*m_rhi, targets.volumeBlur, "GodRays:VolumeBlur");
    if (!targets.maskTarget || !targets.blurTarget || !targets.volumeTarget || !targets.volumeBlurTarget)
    {
        targets = ViewTargets{};
        targets.lastScreenSpaceState = lastScreenSpaceState;
        targets.lastVolumetricState = lastVolumetricState;
        return false;
    }
    targets.width = rayWidth;
    targets.height = rayHeight;
    return true;
}

void GodRayRenderer::DrawFullscreen(ixrhi::IXRHICommandList& cmd,
                                    const ixrhi::IXRHIRenderTarget& target,
                                    std::uint32_t width,
                                    std::uint32_t height,
                                    const ixrhi::IXRHIGraphicsPipeline& pipeline,
                                    const ixrhi::IXRHIBindGroup& bindings,
                                    std::uint32_t set,
                                    const void* push,
                                    std::uint32_t pushBytes)
{
    target.Begin(cmd);
    cmd.SetViewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    cmd.SetScissor(0, 0, width, height);
    cmd.SetGraphicsPipeline(pipeline);
    cmd.BindGroup(0, bindings, set);
    cmd.PushConstants(push, pushBytes);
    cmd.Draw(3, 1, 0, 0);
    target.End(cmd);
}

bool GodRayRenderer::RenderRays(ixrhi::IXRHICommandList& cmd,
                                const ixrhi::IXRHIFrameInfo& frame,
                                std::uint32_t view,
                                const SkySettings& sky,
                                const LightingState& lighting,
                                const WorldCamera& camera,
                                const std::shared_ptr<ixrhi::IXRHITexture>& depth,
                                const SunShadow& shadow,
                                std::uint32_t width,
                                std::uint32_t height)
{
    if (view >= kViews)
        return false;
    ViewTargets& targets = m_views[view];
    targets.screenSpaceReady = false;
    targets.volumetricReady = false;
    // States are logged when they change. "Off" states (disabled, sun set or off screen) are neither
    // logged nor remembered: they flip whenever the camera turns.
    const auto report = [&](const char*& last, const char* technique, const char* state) {
        if (std::strncmp(state, "off", 3) == 0 || last == state)
            return;
        Tracenf("[GOD-RAYS] view=%u %s %s", view, technique, state);
        last = state;
    };
    if (!m_rhi || !frame.frameActive || !depth || width == 0 || height == 0 || !m_maskPipeline || !m_marchPipeline)
    {
        report(targets.lastScreenSpaceState, "screen-space", "skipped: no depth snapshot, size or pipeline");
        return false;
    }

    Push push{};
    const char* screenSpaceState = BuildPush(sky, lighting, camera, push);
    VolumetricPush volumetric{};
    const char* volumetricState = BuildVolumetricPush(sky, lighting, camera, shadow, volumetric);
    report(targets.lastVolumetricState, "volumetric", volumetricState ? volumetricState : "drawing");
    report(targets.lastScreenSpaceState, "screen-space", screenSpaceState ? screenSpaceState : "drawing");
    if (screenSpaceState && volumetricState)
        return false;
    if (!EnsureViewTargets(targets, width, height))
    {
        report(targets.lastScreenSpaceState, "screen-space", "skipped: render targets could not be created");
        return false;
    }

    const std::uint32_t frameIndex = frame.frameIndex % kFramesInFlight;
    const std::uint32_t set = view * kFramesInFlight + frameIndex;
    if (!screenSpaceState)
    {
        m_maskBindings->UpdateTexture(set, 0, depth, m_sampler);
        m_blurBindings->UpdateTexture(set, 0, targets.mask, m_sampler);
        // The mask: the sun where the sky shows. The shafts: the mask blurred towards the sun.
        DrawFullscreen(cmd, *targets.maskTarget, targets.width, targets.height, *m_maskPipeline,
            *m_maskBindings, set, &push, sizeof(push));
        DrawFullscreen(cmd, *targets.blurTarget, targets.width, targets.height, *m_blurPipeline,
            *m_blurBindings, set, &push, sizeof(push));
        targets.push = push;
        targets.screenSpaceReady = true;
    }
    if (!volumetricState)
    {
        // The cascade matrices are this frame's for both views: writing them twice writes the same.
        m_cascadeBuffers[frameIndex]->Write(0, shadow.cascadeViewProj, kCascadeBufferBytes);
        m_marchBindings->UpdateTexture(set, 0, depth, m_sampler);
        m_marchBindings->UpdateTexture(set, 1, shadow.texture, m_shadowSampler);
        m_marchBindings->UpdateBuffer(set, 2, m_cascadeBuffers[frameIndex], 0, kCascadeBufferBytes);
        m_volumeBlurBindings->UpdateTexture(set, 0, targets.volume, m_sampler);
        DrawFullscreen(cmd, *targets.volumeTarget, targets.width, targets.height, *m_marchPipeline,
            *m_marchBindings, set, &volumetric, sizeof(volumetric));
        Push blur{};
        blur.rayParams[0] = 1.0f / static_cast<float>(targets.width);
        blur.rayParams[1] = 1.0f / static_cast<float>(targets.height);
        DrawFullscreen(cmd, *targets.volumeBlurTarget, targets.width, targets.height, *m_volumeBlurPipeline,
            *m_volumeBlurBindings, set, &blur, sizeof(blur));
        targets.volumetricReady = true;
    }
    return true;
}

bool GodRayRenderer::TakeAddedLight(std::uint32_t view, AddedLight& out)
{
    out = AddedLight{};
    if (view >= kViews)
        return false;
    ViewTargets& targets = m_views[view];
    if (!targets.screenSpaceReady && !targets.volumetricReady)
        return false;
    // The shafts take their intensity here; the volumetric light has it in the march already.
    if (targets.screenSpaceReady)
    {
        out.shafts = targets.blur;
        out.shaftsWeight = targets.push.composite[0];
    }
    if (targets.volumetricReady)
    {
        out.volume = targets.volumeBlur;
        out.volumeWeight = 1.0f;
    }
    targets.screenSpaceReady = false;
    targets.volumetricReady = false;
    return true;
}

void GodRayRenderer::Destroy()
{
    m_maskPipeline.reset();
    m_blurPipeline.reset();
    m_marchPipeline.reset();
    m_volumeBlurPipeline.reset();
    for (ViewTargets& targets : m_views)
        targets = ViewTargets{};
    m_prototypeTarget.reset();
    m_prototypeTexture.reset();
    for (auto& buffer : m_cascadeBuffers)
        buffer.reset();
    m_maskBindings.reset();
    m_blurBindings.reset();
    m_marchBindings.reset();
    m_volumeBlurBindings.reset();
    m_bindLayout.reset();
    m_marchLayout.reset();
    m_sampler.reset();
    m_shadowSampler.reset();
    m_vs.reset();
    m_maskPs.reset();
    m_blurPs.reset();
    m_volumeBlurPs.reset();
    m_marchPs.reset();
    m_rhi = nullptr;
    m_assets = nullptr;
}
