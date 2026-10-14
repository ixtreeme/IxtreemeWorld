#include "GraphEnvironment.h"

#include "Debug.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace
{
constexpr std::uint32_t kConstantsBinding = 0, kDepthBinding = 1, kCubeBinding = 2, kPanoramaBinding = 3;

std::shared_ptr<ixrhi::IXRHITexture> CreateImage(ixrhi::IXRHIDevice& rhi, bool cube, std::uint8_t value, const char* name)
{
    const std::uint32_t layers = cube ? 6u : 1u;
    const std::vector<std::uint8_t> pixels(4u * layers, value);
    ixrhi::IXRHITextureDesc desc;
    desc.width = 1;
    desc.height = 1;
    desc.arrayLayers = layers;
    desc.cubeMap = cube;
    desc.format = ixrhi::IXRHIFormat::R8G8B8A8Unorm;
    desc.usage = ixrhi::IXRHITextureUsage::Sampled;
    desc.debugName = name;
    return rhi.CreateTexture(desc, pixels.data(), pixels.size());
}
} // namespace

bool GraphEnvironment::Create(ixrhi::IXRHIDevice& rhi)
{
    Destroy();
    m_rhi = &rhi;
    const std::vector<ixrhi::IXRHIBinding> bindings = {
        {kConstantsBinding, ixrhi::IXRHIBindingType::UniformBuffer, ixrhi::IXRHIShaderStage::Fragment},
        {kDepthBinding, ixrhi::IXRHIBindingType::SampledTexture, ixrhi::IXRHIShaderStage::Fragment},
        {kCubeBinding, ixrhi::IXRHIBindingType::SampledTexture, ixrhi::IXRHIShaderStage::Fragment},
        {kPanoramaBinding, ixrhi::IXRHIBindingType::SampledTexture, ixrhi::IXRHIShaderStage::Fragment},
    };
    m_layout = rhi.CreateBindGroupLayout(bindings);

    ixrhi::IXRHIBufferDesc constants;
    constants.sizeBytes = kConstantStride * kFramesInFlight * kSetsPerFrame;
    constants.usage = ixrhi::IXRHIBufferUsage::Uniform;
    constants.cpuAccess = ixrhi::IXRHICpuAccess::Write;
    constants.debugName = "GraphEnvironment:Constants";
    m_constants = rhi.CreateBuffer(constants, nullptr, 0);

    // The depth of a pixel is read where it is, not blended with its neighbours'.
    ixrhi::IXRHISamplerDesc depthSampler;
    depthSampler.minFilter = ixrhi::IXRHISamplerFilter::Nearest;
    depthSampler.magFilter = ixrhi::IXRHISamplerFilter::Nearest;
    depthSampler.debugName = "GraphEnvironment:DepthSampler";
    m_depthSampler = rhi.CreateSampler(depthSampler);
    ixrhi::IXRHISamplerDesc skySampler;
    skySampler.addressU = ixrhi::IXRHISamplerAddress::Repeat;
    skySampler.debugName = "GraphEnvironment:SkySampler";
    m_skySampler = rhi.CreateSampler(skySampler);
    m_fallbackDepth = CreateImage(rhi, false, 255, "GraphEnvironment:NoDepth");
    m_fallbackCube = CreateImage(rhi, true, 0, "GraphEnvironment:NoSkyCube");
    m_fallbackPanorama = CreateImage(rhi, false, 0, "GraphEnvironment:NoSkyPanorama");
    bool ready = m_layout && m_constants && m_depthSampler && m_skySampler && m_fallbackDepth && m_fallbackCube &&
                 m_fallbackPanorama;
    for (std::uint32_t frame = 0; ready && frame < kFramesInFlight; ++frame)
        ready = CreateGroup(frame);
    if (!ready)
    {
        Tracenf("[GRAPH-ENVIRONMENT] Create failed");
        Destroy();
        return false;
    }
    return true;
}

void GraphEnvironment::Destroy()
{
    for (auto& group : m_groups)
        group.reset();
    m_retired.clear();
    m_layout.reset();
    m_constants.reset();
    m_fallbackDepth.reset();
    m_fallbackCube.reset();
    m_fallbackPanorama.reset();
    m_depthSampler.reset();
    m_skySampler.reset();
    m_sky = {};
    m_hasSky = false;
    m_bound = {};
    for (auto& seen : m_imagesSeen)
        seen.clear();
    m_setFrame = {};
    m_cursor = {};
    m_current = {};
    m_rhi = nullptr;
}

bool GraphEnvironment::CreateGroup(std::uint32_t frameIndex)
{
    if (m_groups[frameIndex])
        m_retired.emplace_back(std::move(m_groups[frameIndex]), m_frameNumber);
    m_groups[frameIndex] = m_rhi->CreateBindGroup(*m_layout, kSetsPerFrame);
    m_bound[frameIndex] = {};
    m_imagesSeen[frameIndex].clear();
    if (!m_groups[frameIndex])
        return false;
    // Every set valid from the start: time 0, no depth, the default (black) sky images.
    const Constants defaults{};
    for (std::uint32_t set = 0; set < kSetsPerFrame; ++set)
        WriteSet(frameIndex, set, defaults, nullptr);
    return true;
}

void GraphEnvironment::SetSky(const SkyRenderer::Lookup& sky)
{
    m_sky = sky;
    m_hasSky = true;
}

void GraphEnvironment::WriteSet(std::uint32_t frameIndex, std::uint32_t set, const Constants& constants,
                                const std::shared_ptr<ixrhi::IXRHITexture>& depth)
{
    ixrhi::IXRHIBindGroup& group = *m_groups[frameIndex];
    BoundImages& bound = m_bound[frameIndex][set];
    const std::uint64_t offset = (static_cast<std::uint64_t>(frameIndex) * kSetsPerFrame + set) * kConstantStride;
    m_constants->Write(offset, &constants, sizeof(constants));
    if (!bound.constants)
        group.UpdateBuffer(set, kConstantsBinding, m_constants, offset, sizeof(Constants));
    bound.constants = true;

    const auto bind = [&](std::uint32_t binding, const std::shared_ptr<ixrhi::IXRHITexture>& image,
                          const std::shared_ptr<ixrhi::IXRHISampler>& sampler, const ixrhi::IXRHITexture*& current) {
        if (current == image.get())
            return;
        group.UpdateTexture(set, binding, image, sampler);
        m_imagesSeen[frameIndex].insert(image.get());
        current = image.get();
    };
    const bool skyImages = m_hasSky && m_sky.cube && m_sky.panorama && m_sky.cubeSampler && m_sky.panoramaSampler;
    bind(kDepthBinding, depth ? depth : m_fallbackDepth, m_depthSampler, bound.depth);
    bind(kCubeBinding, skyImages ? m_sky.cube : m_fallbackCube, skyImages ? m_sky.cubeSampler : m_skySampler, bound.cube);
    bind(kPanoramaBinding, skyImages ? m_sky.panorama : m_fallbackPanorama,
        skyImages ? m_sky.panoramaSampler : m_skySampler, bound.panorama);
    bound.hasDepth = depth != nullptr;
}

void GraphEnvironment::Begin(const ixrhi::IXRHIFrameInfo& frame,
                             double seconds,
                             const WorldCamera& camera,
                             std::uint32_t width,
                             std::uint32_t height,
                             std::shared_ptr<ixrhi::IXRHITexture> sceneDepth)
{
    m_seconds = seconds;
    m_camera = camera;
    m_width = width;
    m_height = height;
    BeginSet(frame, std::move(sceneDepth));
}

void GraphEnvironment::BeginSet(const ixrhi::IXRHIFrameInfo& frame, std::shared_ptr<ixrhi::IXRHITexture> sceneDepth)
{
    if (!IsReady() || !frame.frameActive)
        return;
    const std::uint32_t frameIndex = frame.frameIndex % kFramesInFlight;
    m_frameNumber = frame.frameNumber;
    std::erase_if(m_retired, [&](const auto& retired) { return m_frameNumber > retired.second + kFramesInFlight; });
    if (m_setFrame[frameIndex] != frame.frameNumber)
    {
        // The frame that last used these sets has finished: they can be rewritten, or the group
        // replaced once it has kept too many images alive. (Nothing of this frame has bound them yet:
        // Bind begins a frame first.)
        m_setFrame[frameIndex] = frame.frameNumber;
        m_cursor[frameIndex] = 0;
        if (m_imagesSeen[frameIndex].size() > kMaxImagesPerGroup && !CreateGroup(frameIndex))
        {
            Tracenf("[GRAPH-ENVIRONMENT] cannot replace the bind group");
            return;
        }
    }
    std::uint32_t set = m_cursor[frameIndex];
    if (set >= kSetsPerFrame)
    {
        // Out of sets: the last one is rewritten (the draws already recorded with it see these values).
        set = kSetsPerFrame - 1;
        if (!m_loggedFull)
            Tracenf("[GRAPH-ENVIRONMENT] more than %u views in a frame; the last set is shared", kSetsPerFrame);
        m_loggedFull = true;
    }
    else
        ++m_cursor[frameIndex];

    Constants constants{};
    constants.frame[0] = static_cast<float>(std::fmod(std::max(m_seconds, 0.0), kTimePeriodSeconds));
    constants.frame[1] = m_camera.nearPlane;
    constants.frame[2] = m_camera.farPlane;
    constants.frame[3] = sceneDepth ? 1.0f : 0.0f;
    constants.viewport[0] = static_cast<float>(m_width);
    constants.viewport[1] = static_cast<float>(m_height);
    constants.viewport[2] = m_width ? 1.0f / static_cast<float>(m_width) : 0.0f;
    constants.viewport[3] = m_height ? 1.0f / static_cast<float>(m_height) : 0.0f;
    if (m_hasSky)
        std::memcpy(constants.sky, m_sky.parameters.data(), sizeof(constants.sky));
    WriteSet(frameIndex, set, constants, sceneDepth);
    m_current[frameIndex] = set;
}

void GraphEnvironment::Bind(ixrhi::IXRHICommandList& cmd, const ixrhi::IXRHIFrameInfo& frame)
{
    const std::uint32_t frameIndex = frame.frameIndex % kFramesInFlight;
    if (m_setFrame[frameIndex] != frame.frameNumber)
        BeginSet(frame, nullptr);
    if (m_groups[frameIndex])
        cmd.BindGroup(kSet, *m_groups[frameIndex], m_current[frameIndex]);
}
bool GraphEnvironment::HasSceneDepth(std::uint32_t frameIndex) const
{
    frameIndex %= kFramesInFlight;
    return m_bound[frameIndex][m_current[frameIndex]].hasDepth;
}
