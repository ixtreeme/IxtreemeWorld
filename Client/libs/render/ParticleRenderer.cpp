#include "asset/ExrImage.h"
#include "ParticleRenderer.h"

#include "Debug.h"
#include "asset/IAssetReader.h"

#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

namespace xm = ixtreeme::math;

namespace
{

std::vector<std::uint32_t> ReadSpirv(client::asset::IAssetReader& assets, const std::string& path)
{
    auto bytes = assets.ReadAll(path);
    if (!bytes || bytes->empty() || bytes->size() % sizeof(std::uint32_t) != 0)
    {
        TraceError("[PARTICLE] failed to open shader: %s", path.c_str());
        return {};
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
    if (desc.spirv.empty())
        return nullptr;
    return rhi.CreateShader(desc);
}

bool DecodeTextureFile(const std::string& path, int& width, int& height, std::vector<std::uint8_t>& pixels,
                       ixrhi::IXRHIFormat& format)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    std::vector<std::uint8_t> encoded((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (encoded.empty())
        return false;

    if (client::asset::IsExr(encoded) || client::asset::IsExrPath(path))
    {
        std::string error;
        auto exr = client::asset::DecodeExr(encoded, error);
        if (!exr) { TraceError("[PARTICLE-EXR] %s", error.c_str()); return false; }
        width = exr->width;
        height = exr->height;
        pixels = client::asset::ExrHalfPixels(*exr);
        format = ixrhi::IXRHIFormat::R16G16B16A16Float;
        return true;
    }
    format = ixrhi::IXRHIFormat::R8G8B8A8Unorm;

    int channels = 0;
    stbi_uc* decoded = stbi_load_from_memory(
        encoded.data(), static_cast<int>(encoded.size()), &width, &height, &channels, 4);
    if (!decoded || width <= 0 || height <= 0)
    {
        if (decoded)
            stbi_image_free(decoded);
        return false;
    }
    pixels.assign(decoded,
        decoded + static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u);
    stbi_image_free(decoded);
    return true;
}

} // namespace

ParticleRenderer::~ParticleRenderer()
{
    Destroy();
}

bool ParticleRenderer::Create(ixrhi::IXRHIDevice& rhi, client::asset::IAssetReader& assets)
{
    Destroy();
    m_rhi = &rhi;
    m_assets = &assets;

    m_vertexShader = LoadShader(rhi, assets, "assets/shaders/particle_vs.spv",
        ixrhi::IXRHIShaderStage::Vertex, "VSMain");
    m_pixelShader = LoadShader(rhi, assets, "assets/shaders/particle_ps.spv",
        ixrhi::IXRHIShaderStage::Fragment, "PSMain");
    if (!m_vertexShader || !m_pixelShader)
    {
        TraceError("[PARTICLE] shaders missing - particle emitters will not render");
        Destroy();
        return false;
    }

    // The GPU simulation path is optional: without its shaders the CPU path still works.
    m_gpuVertexShader = LoadShader(rhi, assets, "assets/shaders/particle_gpu_vs.spv",
        ixrhi::IXRHIShaderStage::Vertex, "VSMain");
    m_gpuPixelShader = LoadShader(rhi, assets, "assets/shaders/particle_gpu_ps.spv",
        ixrhi::IXRHIShaderStage::Fragment, "PSMain");
    m_gpuSimShader = LoadShader(rhi, assets, "assets/shaders/particles_sim_cs.spv",
        ixrhi::IXRHIShaderStage::Compute, "CSMain");
    if (!m_gpuVertexShader || !m_gpuPixelShader || !m_gpuSimShader)
        TraceError("[PARTICLE] GPU simulation shaders missing - GPU emitters are unavailable");

    if (!CreateBuffers(rhi) || !CreateDefaultTexture(rhi) || !CreateDefaultDepthTexture(rhi) ||
        !CreateBindGroup(rhi) || !CreateGpuSimResources(rhi))
    {
        TraceError("[PARTICLE] renderer resource creation failed");
        Destroy();
        return false;
    }
    return RecreatePipeline(rhi);
}

bool ParticleRenderer::CreateGpuSimResources(ixrhi::IXRHIDevice& rhi)
{
    if (!m_gpuSimShader)
        return true;  // optional path
    const std::vector<ixrhi::IXRHIBinding> bindings = {
        {0, ixrhi::IXRHIBindingType::UniformBuffer, ixrhi::IXRHIShaderStage::Compute},
        {1, ixrhi::IXRHIBindingType::StorageBuffer, ixrhi::IXRHIShaderStage::Compute},
    };
    m_gpuSimLayout = rhi.CreateBindGroupLayout(bindings);
    return m_gpuSimLayout != nullptr;
}

bool ParticleRenderer::CreateBuffers(ixrhi::IXRHIDevice& rhi)
{
    for (std::uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        ixrhi::IXRHIBufferDesc desc;
        desc.sizeBytes = sizeof(InstanceData) * kInitialInstances;
        desc.usage = ixrhi::IXRHIBufferUsage::Storage;
        desc.cpuAccess = ixrhi::IXRHICpuAccess::Write;
        desc.debugName = "Particle:Instances";
        m_instanceBuffers[frame] = rhi.CreateBuffer(desc, nullptr, 0);
        if (!m_instanceBuffers[frame])
            return false;
        m_instanceCapacity[frame] = kInitialInstances;
    }
    for (std::uint32_t slot = 0; slot < kFramesInFlight * kDrawSlots; ++slot)
    {
        ixrhi::IXRHIBufferDesc desc;
        desc.sizeBytes = sizeof(ViewUniform);
        desc.usage = ixrhi::IXRHIBufferUsage::Uniform;
        desc.cpuAccess = ixrhi::IXRHICpuAccess::Write;
        desc.debugName = "Particle:ViewUBO";
        m_uniformBuffers[slot] = rhi.CreateBuffer(desc, nullptr, 0);
        if (!m_uniformBuffers[slot])
            return false;
    }
    return true;
}

bool ParticleRenderer::CreateDefaultTexture(ixrhi::IXRHIDevice& rhi)
{
    // A soft round white sprite: the fallback for emitters without a texture. RGBA8, alpha falls
    // off smoothly to the edge (premultiplied in the shader).
    constexpr std::uint32_t kSize = 64;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kSize) * kSize * 4u);
    for (std::uint32_t y = 0; y < kSize; ++y)
    {
        for (std::uint32_t x = 0; x < kSize; ++x)
        {
            const float dx = (static_cast<float>(x) + 0.5f) / static_cast<float>(kSize) * 2.0f - 1.0f;
            const float dy = (static_cast<float>(y) + 0.5f) / static_cast<float>(kSize) * 2.0f - 1.0f;
            const float radius = std::sqrt(dx * dx + dy * dy);
            float alpha = std::clamp(1.0f - radius, 0.0f, 1.0f);
            alpha = alpha * alpha * (3.0f - 2.0f * alpha);  // smoothstep falloff
            const std::size_t index = (static_cast<std::size_t>(y) * kSize + x) * 4u;
            pixels[index + 0] = 255;
            pixels[index + 1] = 255;
            pixels[index + 2] = 255;
            pixels[index + 3] = static_cast<std::uint8_t>(alpha * 255.0f + 0.5f);
        }
    }

    ixrhi::IXRHISamplerDesc samplerDesc;
    samplerDesc.minFilter = ixrhi::IXRHISamplerFilter::Linear;
    samplerDesc.magFilter = ixrhi::IXRHISamplerFilter::Linear;
    samplerDesc.mipmapFilter = ixrhi::IXRHISamplerFilter::Nearest;
    samplerDesc.addressU = ixrhi::IXRHISamplerAddress::ClampToEdge;
    samplerDesc.addressV = ixrhi::IXRHISamplerAddress::ClampToEdge;
    samplerDesc.addressW = ixrhi::IXRHISamplerAddress::ClampToEdge;
    samplerDesc.maxLod = 0.0f;
    samplerDesc.debugName = "Particle:Sampler";
    m_defaultTexture.sampler = rhi.CreateSampler(samplerDesc);
    if (!m_defaultTexture.sampler)
        return false;

    ixrhi::IXRHITextureDesc desc;
    desc.width = kSize;
    desc.height = kSize;
    desc.format = ixrhi::IXRHIFormat::R8G8B8A8Unorm;
    desc.usage = ixrhi::IXRHITextureUsage::Sampled | ixrhi::IXRHITextureUsage::TransferDst;
    desc.debugName = "Particle:DefaultSprite";
    m_defaultTexture.texture = rhi.CreateTexture(desc, pixels.data(), pixels.size());
    return m_defaultTexture.texture != nullptr;
}

bool ParticleRenderer::CreateDefaultDepthTexture(ixrhi::IXRHIDevice& rhi)
{
    // The neutral depth for soft particles when no snapshot is available: 1.0 = the far plane, so
    // the linearized scene depth is huge and the fade saturates to 1 (no fade).
    const float farDepth = 1.0f;
    ixrhi::IXRHITextureDesc desc;
    desc.width = 1;
    desc.height = 1;
    desc.format = ixrhi::IXRHIFormat::R32Float;
    desc.usage = ixrhi::IXRHITextureUsage::Sampled | ixrhi::IXRHITextureUsage::TransferDst;
    desc.debugName = "Particle:DefaultDepth";
    m_dummyDepth = rhi.CreateTexture(desc, &farDepth, sizeof(farDepth));
    return m_dummyDepth != nullptr;
}

bool ParticleRenderer::CreateBindGroup(ixrhi::IXRHIDevice& rhi)
{
    const std::vector<ixrhi::IXRHIBinding> bindings = {
        // The view uniform carries the soft-particle parameters the pixel shader reads too.
        {0, ixrhi::IXRHIBindingType::UniformBuffer,
            ixrhi::IXRHIShaderStage::Vertex | ixrhi::IXRHIShaderStage::Fragment},
        {1, ixrhi::IXRHIBindingType::SampledTexture, ixrhi::IXRHIShaderStage::Fragment},
        {2, ixrhi::IXRHIBindingType::StorageBuffer, ixrhi::IXRHIShaderStage::Vertex},
        {3, ixrhi::IXRHIBindingType::SampledTexture, ixrhi::IXRHIShaderStage::Fragment},  // scene depth
    };
    m_bindLayout = rhi.CreateBindGroupLayout(bindings);
    if (!m_bindLayout)
        return false;
    m_bindGroup = rhi.CreateBindGroup(*m_bindLayout, kFramesInFlight * kDrawSlots);
    if (!m_bindGroup)
        return false;

    for (std::uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        for (std::uint32_t slot = 0; slot < kDrawSlots; ++slot)
        {
            const std::uint32_t setIndex = frame * kDrawSlots + slot;
            m_bindGroup->UpdateBuffer(setIndex, 0, m_uniformBuffers[setIndex], 0, sizeof(ViewUniform));
            if (m_defaultTexture.texture && m_defaultTexture.sampler)
                m_bindGroup->UpdateTexture(setIndex, 1, m_defaultTexture.texture, m_defaultTexture.sampler);
            m_bindGroup->UpdateBuffer(setIndex, 2, m_instanceBuffers[frame], 0,
                sizeof(InstanceData) * m_instanceCapacity[frame]);
            if (m_dummyDepth && m_defaultTexture.sampler)
                m_bindGroup->UpdateTexture(setIndex, 3, m_dummyDepth, m_defaultTexture.sampler);
            m_boundDepth[setIndex] = m_dummyDepth;
        }
    }
    return true;
}

void ParticleRenderer::SetSceneDepth(std::shared_ptr<ixrhi::IXRHITexture> depth,
                                     std::shared_ptr<ixrhi::IXRHISampler> sampler,
                                     float nearPlane,
                                     float farPlane)
{
    // Bound per draw (DrawBatch), into the set of the frame being recorded: rewriting every slot here
    // touched the sets the frames in flight still used.
    m_sceneNear = nearPlane > 0.0f ? nearPlane : 0.1f;
    m_sceneFar = farPlane > m_sceneNear ? farPlane : m_sceneNear + 1.0f;
    if (!depth || !sampler)
    {
        m_sceneDepth.reset();
        m_sceneDepthSampler.reset();
        return;
    }
    m_sceneDepth = std::move(depth);
    m_sceneDepthSampler = std::move(sampler);
}

bool ParticleRenderer::RecreatePipeline(ixrhi::IXRHIDevice& rhi)
{
    if (!m_vertexShader || !m_pixelShader || !m_bindLayout)
        return false;
    m_alphaPipeline.reset();
    m_additivePipeline.reset();

    auto build = [&](bool additive) {
        ixrhi::IXRHIGraphicsPipelineDesc desc;
        desc.vertexShader = m_vertexShader;
        desc.fragmentShader = m_pixelShader;
        desc.bindGroupLayouts = {m_bindLayout.get()};
        desc.vertexBindings = {};
        desc.vertexAttributes = {};
        desc.topology = ixrhi::IXRHIPrimitiveTopology::TriangleList;
        desc.cullMode = ixrhi::IXRHICullMode::None;
        desc.depthTestEnable = true;
        desc.depthWriteEnable = false;
        desc.depthCompareOp = ixrhi::IXRHICompareOp::LessOrEqual;
        // The pixel shader outputs premultiplied alpha: alpha = One/OneMinusSrcAlpha,
        // additive = One/One.
        desc.blendAttachments = {{true,
            ixrhi::IXRHIBlendFactor::One,
            additive ? ixrhi::IXRHIBlendFactor::One : ixrhi::IXRHIBlendFactor::OneMinusSrcAlpha,
            ixrhi::IXRHIBlendOp::Add,
            ixrhi::IXRHIBlendFactor::One,
            additive ? ixrhi::IXRHIBlendFactor::One : ixrhi::IXRHIBlendFactor::OneMinusSrcAlpha,
            ixrhi::IXRHIBlendOp::Add}};
        desc.sampleCount = 1;
        desc.targetRenderPass = m_targetPass;
        desc.debugName = additive ? "Particles:Additive" : "Particles:Alpha";
        return rhi.CreateGraphicsPipeline(desc);
    };

    m_alphaPipeline = build(false);
    m_additivePipeline = build(true);
    if (!m_alphaPipeline || !m_additivePipeline)
    {
        TraceError("[PARTICLE] pipeline creation failed");
        return false;
    }

    // GPU simulation path (optional): the GPU draw pipelines share the CPU bind group layout, and
    // the compute pipeline simulates the emitter state buffers.
    m_gpuAlphaPipeline.reset();
    m_gpuAdditivePipeline.reset();
    m_gpuSimPipeline.reset();
    if (m_gpuVertexShader && m_gpuPixelShader)
    {
        auto buildGpu = [&](bool additive) {
            ixrhi::IXRHIGraphicsPipelineDesc desc;
            desc.vertexShader = m_gpuVertexShader;
            desc.fragmentShader = m_gpuPixelShader;
            desc.bindGroupLayouts = {m_bindLayout.get()};
            desc.topology = ixrhi::IXRHIPrimitiveTopology::TriangleList;
            desc.cullMode = ixrhi::IXRHICullMode::None;
            desc.depthTestEnable = true;
            desc.depthWriteEnable = false;
            desc.depthCompareOp = ixrhi::IXRHICompareOp::LessOrEqual;
            desc.blendAttachments = {{true,
                ixrhi::IXRHIBlendFactor::One,
                additive ? ixrhi::IXRHIBlendFactor::One : ixrhi::IXRHIBlendFactor::OneMinusSrcAlpha,
                ixrhi::IXRHIBlendOp::Add,
                ixrhi::IXRHIBlendFactor::One,
                additive ? ixrhi::IXRHIBlendFactor::One : ixrhi::IXRHIBlendFactor::OneMinusSrcAlpha,
                ixrhi::IXRHIBlendOp::Add}};
            desc.sampleCount = 1;
            desc.targetRenderPass = m_targetPass;
            desc.debugName = additive ? "ParticlesGpu:Additive" : "ParticlesGpu:Alpha";
            return rhi.CreateGraphicsPipeline(desc);
        };
        m_gpuAlphaPipeline = buildGpu(false);
        m_gpuAdditivePipeline = buildGpu(true);
    }
    if (m_gpuSimShader && m_gpuSimLayout)
    {
        ixrhi::IXRHIComputePipelineDesc desc;
        desc.computeShader = m_gpuSimShader;
        desc.bindGroupLayouts = {m_gpuSimLayout.get()};
        desc.debugName = "Particles:Simulation";
        m_gpuSimPipeline = rhi.CreateComputePipeline(desc);
    }
    return true;
}

const ParticleRenderer::TextureEntry* ParticleRenderer::ResolveTexture(const std::string& assetId)
{
    if (assetId.empty())
        return &m_defaultTexture;
    const auto it = m_textures.find(assetId);
    if (it != m_textures.end())
        return &it->second;
    if (std::find(m_failedTextureIds.begin(), m_failedTextureIds.end(), assetId) != m_failedTextureIds.end())
        return &m_defaultTexture;

    const std::string path = m_textureResolver ? m_textureResolver(assetId) : std::string();
    if (path.empty())
    {
        TraceError("[PARTICLE] texture asset '%s' has no file (using the default sprite)", assetId.c_str());
        m_failedTextureIds.push_back(assetId);
        return &m_defaultTexture;
    }

    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;
    ixrhi::IXRHIFormat format = ixrhi::IXRHIFormat::R8G8B8A8Unorm;
    if (!DecodeTextureFile(path, width, height, pixels, format))
    {
        TraceError("[PARTICLE] texture '%s' could not be decoded (using the default sprite)", path.c_str());
        m_failedTextureIds.push_back(assetId);
        return &m_defaultTexture;
    }

    ixrhi::IXRHITextureDesc desc;
    desc.width = static_cast<std::uint32_t>(width);
    desc.height = static_cast<std::uint32_t>(height);
    desc.format = format;
    desc.usage = ixrhi::IXRHITextureUsage::Sampled | ixrhi::IXRHITextureUsage::TransferDst;
    desc.debugName = "Particle:" + assetId;
    TextureEntry entry;
    entry.texture = m_rhi->CreateTexture(desc, pixels.data(), pixels.size());
    entry.sampler = m_defaultTexture.sampler;
    if (!entry.texture)
    {
        TraceError("[PARTICLE] texture '%s' upload failed (using the default sprite)", path.c_str());
        m_failedTextureIds.push_back(assetId);
        return &m_defaultTexture;
    }
    const auto inserted = m_textures.emplace(assetId, std::move(entry));
    Tracenf("[PARTICLE] texture loaded '%s' (%dx%d)", path.c_str(), width, height);
    return &inserted.first->second;
}

void ParticleRenderer::RenderInWorld(ixrhi::IXRHICommandList& cmd,
                                     const ixrhi::IXRHIFrameInfo& frame,
                                     const WorldCamera& camera,
                                     const Batch& batch,
                                     std::uint32_t width,
                                     std::uint32_t height)
{
    if (!m_rhi || !frame.frameActive)
        return;
    if (width == 0 || height == 0)
        return;

    // GPU emitters draw their whole state buffer with a fixed instance count (dead particles are
    // size 0); CPU emitters draw the instances appended to the shared buffer this frame.
    const GpuEmitter* gpuEmitter = nullptr;
    const ixrhi::IXRHIGraphicsPipeline* pipeline = nullptr;
    if (batch.gpu)
    {
        const auto it = m_gpuEmitters.find(batch.gpuEntityId);
        if (it == m_gpuEmitters.end() || !it->second.state || !it->second.initialized)
            return;
        gpuEmitter = &it->second;
        pipeline = batch.additive ? m_gpuAdditivePipeline.get() : m_gpuAlphaPipeline.get();
    }
    else
    {
        if (batch.instanceCount == 0)
            return;
        pipeline = batch.additive ? m_additivePipeline.get() : m_alphaPipeline.get();
    }
    if (!pipeline || !m_bindGroup)
        return;

    const std::uint32_t frameIndex = frame.frameIndex % kFramesInFlight;
    if (frame.frameNumber != m_lastFrameNumber)
    {
        m_lastFrameNumber = frame.frameNumber;
        m_drawSlotCursor = 0;
    }
    if (m_drawSlotCursor >= kDrawSlots)
    {
        static bool loggedSlotsFull = false;
        if (!loggedSlotsFull)
        {
            Tracenf("[PARTICLE] more than %u emitter draws in a frame: the rest are skipped", kDrawSlots);
            loggedSlotsFull = true;
        }
        return;
    }

    std::uint32_t count = 0;
    std::uint32_t base = 0;
    if (gpuEmitter)
    {
        count = gpuEmitter->maxParticles;
    }
    else
    {
        // Its range of this frame's upload (past what the buffer took: nothing, or the part it did).
        if (frame.frameNumber != m_uploadedFrameNumber || batch.instanceBase >= m_uploadedCount)
            return;
        base = batch.instanceBase;
        count = std::min(batch.instanceCount, m_uploadedCount - base);
        if (count == 0)
            return;
    }

    // The billboard's camera basis (same derivation as WorldLabelRenderer).
    ViewUniform uniform{};
    std::memcpy(uniform.viewProj, camera.viewProjection.m, sizeof(uniform.viewProj));
    const WorldVec3 forward = xm::Normalize(camera.target - camera.eye);
    WorldVec3 right = xm::Normalize(xm::Cross({0.0f, 1.0f, 0.0f}, forward));
    if (xm::Dot(right, right) <= 0.000001f)
        right = {1.0f, 0.0f, 0.0f};
    const WorldVec3 up = xm::Normalize(xm::Cross(forward, right));
    uniform.cameraRight[0] = right.x;
    uniform.cameraRight[1] = right.y;
    uniform.cameraRight[2] = right.z;
    uniform.cameraUp[0] = up.x;
    uniform.cameraUp[1] = up.y;
    uniform.cameraUp[2] = up.z;
    uniform.particleParams[0] = batch.softParticles ? 1.0f : 0.0f;
    uniform.particleParams[1] = batch.softDistance;
    uniform.particleParams[2] = m_sceneNear;
    uniform.particleParams[3] = m_sceneFar;

    const std::uint32_t slotInFrame = m_drawSlotCursor++;  // < kDrawSlots, checked above
    const std::uint32_t bindSlot = frameIndex * kDrawSlots + slotInFrame;
    m_uniformBuffers[bindSlot]->Write(0, &uniform, sizeof(uniform));
    const TextureEntry* texture = ResolveTexture(batch.textureAssetId);
    if (texture && texture->texture && texture->sampler)
        m_bindGroup->UpdateTexture(bindSlot, 1, texture->texture, texture->sampler);

    // Rebind the instance storage for this slot: the shared per-frame buffer (CPU path) or the
    // emitter's GPU state buffer. Slots are shared between CPU and GPU draws, so this is per draw.
    if (gpuEmitter)
        m_bindGroup->UpdateBuffer(bindSlot, 2, gpuEmitter->state, 0,
            kGpuStateBytes * gpuEmitter->maxParticles);
    else
        m_bindGroup->UpdateBuffer(bindSlot, 2, m_instanceBuffers[frameIndex], 0,
            sizeof(InstanceData) * m_instanceCapacity[frameIndex]);
    // The depth the soft fade reads (SetSceneDepth; the dummy one until there is one).
    const bool haveSceneDepth = m_sceneDepth && m_sceneDepthSampler;
    const std::shared_ptr<ixrhi::IXRHITexture>& depthImage = haveSceneDepth ? m_sceneDepth : m_dummyDepth;
    if (depthImage && m_boundDepth[bindSlot] != depthImage)
    {
        m_bindGroup->UpdateTexture(bindSlot, 3, depthImage,
            haveSceneDepth ? m_sceneDepthSampler : m_defaultTexture.sampler);
        m_boundDepth[bindSlot] = depthImage;  // held: a freed image's address could come back
    }

    cmd.SetViewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    cmd.SetScissor(0, 0, width, height);
    cmd.SetGraphicsPipeline(*pipeline);
    cmd.BindGroup(0, *m_bindGroup, bindSlot);
    cmd.Draw(6, count, 0, base);
}

ParticleRenderer::InstanceData* ParticleRenderer::BeginCpuInstances(const ixrhi::IXRHIFrameInfo& frame,
                                                                    std::uint32_t count,
                                                                    std::uint32_t& granted)
{
    m_uploadedFrameNumber = frame.frameNumber;
    m_uploadedCount = 0;
    granted = 0;
    if (!m_rhi || !frame.frameActive || count == 0)
        return nullptr;
    const std::uint32_t frameIndex = frame.frameIndex % kFramesInFlight;
    if (count > kMaxInstances)
    {
        if (!m_loggedCapacity)
        {
            TraceError("[PARTICLE] more than %u CPU particles in a frame: %u not drawn", kMaxInstances,
                count - kMaxInstances);
            m_loggedCapacity = true;
        }
        count = kMaxInstances;
    }
    if (count > m_instanceCapacity[frameIndex] || !m_instanceBuffers[frameIndex])
    {
        // The frame in flight that last used this buffer is done with it, and the draw sets point at
        // the frame's buffer per draw (the sets that pointed at the old one keep it alive).
        std::uint32_t capacity = std::max(m_instanceCapacity[frameIndex], kInitialInstances);
        while (capacity < count)
            capacity = std::min(capacity * 2u, kMaxInstances);
        ixrhi::IXRHIBufferDesc desc;
        desc.sizeBytes = sizeof(InstanceData) * capacity;
        desc.usage = ixrhi::IXRHIBufferUsage::Storage;
        desc.cpuAccess = ixrhi::IXRHICpuAccess::Write;
        desc.debugName = "Particle:Instances";
        if (std::shared_ptr<ixrhi::IXRHIBuffer> grown = m_rhi->CreateBuffer(desc, nullptr, 0))
        {
            m_instanceBuffers[frameIndex] = std::move(grown);
            m_instanceCapacity[frameIndex] = capacity;
            Tracenf("[PARTICLE] frame %u instance buffer: %u particles", frameIndex, capacity);
        }
        if (!m_instanceBuffers[frameIndex])
            return nullptr;
        count = std::min(count, m_instanceCapacity[frameIndex]);
    }
    auto* instances = static_cast<InstanceData*>(m_instanceBuffers[frameIndex]->HostAddress());
    if (!instances)
        return nullptr;
    m_uploadedCount = count;
    granted = count;
    return instances;
}

ParticleRenderer::GpuEmitter* ParticleRenderer::EnsureGpuEmitter(ixrhi::IXRHIDevice& rhi,
                                                                 std::uint32_t entityId,
                                                                 int maxParticles)
{
    maxParticles = std::clamp(maxParticles, 1, 65536);
    GpuEmitter& emitter = m_gpuEmitters[entityId];
    if (emitter.state && emitter.uniforms[0] && emitter.computeBindGroup &&
        emitter.maxParticles == static_cast<std::uint32_t>(maxParticles))
        return &emitter;

    // A new size replaces buffers and sets the frames in flight may still use (their dispatches and
    // draws): wait for them first. It only happens when Max Particles changes.
    if (emitter.state || emitter.computeBindGroup)
        rhi.WaitIdle();
    auto release = [&emitter]() {
        emitter.state.reset();
        for (std::shared_ptr<ixrhi::IXRHIBuffer>& uniform : emitter.uniforms)
            uniform.reset();
        emitter.computeBindGroup.reset();
    };
    release();

    // (Re)create the emitter's state + uniform buffers and its compute bind group. The state buffer
    // starts zeroed (lifetime 0 = dead), so an uninitialized slot can never render garbage.
    std::vector<std::uint8_t> zeroState(static_cast<std::size_t>(kGpuStateBytes) *
        static_cast<std::size_t>(maxParticles), 0);
    ixrhi::IXRHIBufferDesc stateDesc;
    stateDesc.sizeBytes = zeroState.size();
    stateDesc.usage = ixrhi::IXRHIBufferUsage::Storage | ixrhi::IXRHIBufferUsage::Vertex;
    stateDesc.debugName = "Particle:GPUState";
    emitter.state = rhi.CreateBuffer(stateDesc, zeroState.data(), zeroState.size());
    ixrhi::IXRHIBufferDesc uniformDesc;
    uniformDesc.sizeBytes = sizeof(GpuSimUniform);
    uniformDesc.usage = ixrhi::IXRHIBufferUsage::Uniform;
    uniformDesc.cpuAccess = ixrhi::IXRHICpuAccess::Write;
    uniformDesc.debugName = "Particle:GPUSimParams";
    bool uniformsOk = true;
    for (std::shared_ptr<ixrhi::IXRHIBuffer>& uniform : emitter.uniforms)
    {
        uniform = rhi.CreateBuffer(uniformDesc, nullptr, 0);
        uniformsOk = uniformsOk && uniform != nullptr;
    }
    if (!emitter.state || !uniformsOk || !m_gpuSimLayout)
    {
        release();
        return nullptr;
    }
    emitter.computeBindGroup = rhi.CreateBindGroup(*m_gpuSimLayout, kFramesInFlight);
    if (!emitter.computeBindGroup)
    {
        release();
        return nullptr;
    }
    for (std::uint32_t frameSlot = 0; frameSlot < kFramesInFlight; ++frameSlot)
    {
        emitter.computeBindGroup->UpdateBuffer(frameSlot, 0, emitter.uniforms[frameSlot], 0, sizeof(GpuSimUniform));
        emitter.computeBindGroup->UpdateBuffer(frameSlot, 1, emitter.state, 0, stateDesc.sizeBytes);
    }

    emitter.maxParticles = static_cast<std::uint32_t>(maxParticles);
    emitter.spawnAccumulator = 0.0f;
    emitter.spawnCursor = 0;
    emitter.frameSeed = 1;
    emitter.pendingBurst = 0;
    emitter.initialized = false;  // a fresh buffer: nothing has drawn from it yet
    // (The play state - started, playing - survives a resize: a stopped emitter stays stopped.)
    Tracenf("[PARTICLE][gpu] emitter created entity=%u maxParticles=%u", entityId, emitter.maxParticles);
    return &emitter;
}

bool ParticleRenderer::SimulateGpuEmitter(ixrhi::IXRHICommandList& cmd,
                                          const ixrhi::IXRHIFrameInfo& frame,
                                          std::uint32_t entityId,
                                          const GpuEmitterParams& params,
                                          float dtSeconds)
{
    if (!m_rhi || !frame.frameActive || !m_gpuSimPipeline || !m_gpuSimLayout)
        return false;
    GpuEmitter* emitterPtr = EnsureGpuEmitter(*m_rhi, entityId, params.maxParticles);
    if (!emitterPtr)
        return false;
    GpuEmitter& emitter = *emitterPtr;
    emitter.lastSeenFrame = frame.frameNumber;
    // Prune emitters whose entity is gone (once per frame; a generous grace so buffers still read
    // by a frame in flight are never released).
    if (frame.frameNumber != m_lastGpuPruneFrame)
    {
        m_lastGpuPruneFrame = frame.frameNumber;
        for (auto it = m_gpuEmitters.begin(); it != m_gpuEmitters.end();)
        {
            if (!it->second.entityPaused && it->second.lastSeenFrame + 120 < frame.frameNumber)
                it = m_gpuEmitters.erase(it);
            else
                ++it;
        }
    }
    // Its start (created, or Play entered): the initial play state and, when it plays, the start
    // burst, as the CPU path's Reset. A script's Play/Stop before this simulation already decided.
    if (!emitter.started)
    {
        emitter.started = true;
        emitter.playing = params.startPlaying;
        emitter.startBurstPending = params.startPlaying;
    }
    if (emitter.startBurstPending)
    {
        emitter.pendingBurst += std::max(params.burstCount, 0);
        emitter.startBurstPending = false;
    }

    // Spawn budget: the rate accumulator plus any queued bursts (scripts); a stopped emitter only
    // fires queued bursts. The compute respawns the ring range [cursor, cursor + budget).
    if (emitter.playing && dtSeconds > 0.0f)
        emitter.spawnAccumulator += params.emissionRate * dtSeconds;
    std::uint32_t budget = 0;
    if (emitter.spawnAccumulator >= 1.0f)
    {
        budget = static_cast<std::uint32_t>(emitter.spawnAccumulator);
        emitter.spawnAccumulator -= static_cast<float>(budget);
    }
    if (emitter.pendingBurst > 0)
    {
        budget += static_cast<std::uint32_t>(emitter.pendingBurst);
        emitter.pendingBurst = 0;
    }
    budget = std::min(budget, emitter.maxParticles);

    GpuSimUniform uniform{};
    uniform.emitterPos[0] = params.emitterPosition[0];
    uniform.emitterPos[1] = params.emitterPosition[1];
    uniform.emitterPos[2] = params.emitterPosition[2];
    uniform.emitterPos[3] = std::clamp(dtSeconds, 0.0f, 0.1f);
    uniform.emitterDir[0] = params.emitterDirection[0];
    uniform.emitterDir[1] = params.emitterDirection[1];
    uniform.emitterDir[2] = params.emitterDirection[2];
    uniform.params0[0] = params.gravity;
    uniform.params0[1] = params.drag;
    uniform.params0[2] = params.coneAngle * 0.01745329252f;
    uniform.params0[3] = params.rotationSpeed * 0.01745329252f;
    uniform.params1[0] = params.shapeRadius;
    uniform.params1[1] = params.shapeArc * 0.01745329252f;
    uniform.params1[2] = params.shape;
    uniform.params1[3] = emitter.clearPending ? 1.0f : 0.0f;
    for (int axis = 0; axis < 3; ++axis)
    {
        uniform.axisX[axis] = params.emitterAxes[axis];
        uniform.axisY[axis] = params.emitterAxes[3 + axis];
        uniform.axisZ[axis] = params.emitterAxes[6 + axis];
    }
    uniform.shapeExtents[0] = params.shapeExtents[0];
    uniform.shapeExtents[1] = params.shapeExtents[1];
    uniform.shapeExtents[2] = params.shapeExtents[2];
    uniform.spawn[0] = static_cast<float>(budget);
    uniform.spawn[1] = static_cast<float>(emitter.spawnCursor);
    uniform.spawn[2] = static_cast<float>(emitter.frameSeed);
    uniform.spawn[3] = static_cast<float>(emitter.maxParticles);
    std::memcpy(uniform.sizeOverLife, params.sizeOverLife, sizeof(uniform.sizeOverLife));
    std::memcpy(uniform.color0, params.colorOverLife + 0, sizeof(uniform.color0));
    std::memcpy(uniform.color1, params.colorOverLife + 4, sizeof(uniform.color1));
    std::memcpy(uniform.color2, params.colorOverLife + 8, sizeof(uniform.color2));
    std::memcpy(uniform.color3, params.colorOverLife + 12, sizeof(uniform.color3));
    uniform.life[0] = params.lifetimeMin;
    uniform.life[1] = params.lifetimeMax;
    uniform.life[2] = params.sizeMin;
    uniform.life[3] = params.sizeMax;
    uniform.speed[0] = params.speedMin;
    uniform.speed[1] = params.speedMax;
    uniform.speed[2] = static_cast<float>(std::max(params.atlasColumns, 1));
    uniform.speed[3] = static_cast<float>(std::max(params.atlasRows, 1));
    // This frame's uniform and compute set: the previous frame's dispatch may still read its own.
    const std::uint32_t frameSlot = frame.frameIndex % kFramesInFlight;
    emitter.uniforms[frameSlot]->Write(0, &uniform, sizeof(uniform));

    // Outside any pass (the engine calls this in the render pre-pass). The same buffer is drawn
    // later this frame, hence the write -> vertex-read barrier (the skinning path's pattern).
    if (emitter.initialized)
        cmd.TransitionBuffer(*emitter.state, ixrhi::IXRHIBufferState::VertexRead,
            ixrhi::IXRHIBufferState::ShaderWrite);
    cmd.SetComputePipeline(*m_gpuSimPipeline);
    cmd.BindGroup(0, *emitter.computeBindGroup, frameSlot);
    const std::uint32_t groups = (emitter.maxParticles + 63u) / 64u;
    cmd.Dispatch(groups, 1, 1);
    cmd.TransitionBuffer(*emitter.state, ixrhi::IXRHIBufferState::ShaderWrite,
        ixrhi::IXRHIBufferState::VertexRead);
    emitter.initialized = true;
    emitter.clearPending = false;

    emitter.spawnCursor = (emitter.spawnCursor + budget) % emitter.maxParticles;
    ++emitter.frameSeed;
    return true;
}

void ParticleRenderer::GpuEmitterPlay(std::uint32_t entityId)
{
    const auto it = m_gpuEmitters.find(entityId);
    if (it == m_gpuEmitters.end())
        return;
    // As the CPU path: an emitter not started yet starts (with its burst); a stopped one resumes.
    if (!it->second.started)
    {
        it->second.started = true;
        it->second.startBurstPending = true;
    }
    it->second.playing = true;
}

void ParticleRenderer::GpuEmitterSetEntityPaused(std::uint32_t entityId, bool paused)
{
    const auto it = m_gpuEmitters.find(entityId);
    if (it != m_gpuEmitters.end())
    {
        // Another emitter may prune this frame before this resumed entity is
        // simulated. Refresh its grace period before releasing pause protection.
        if (it->second.entityPaused && !paused)
            it->second.lastSeenFrame = m_lastGpuPruneFrame;
        it->second.entityPaused = paused;
    }
}

void ParticleRenderer::GpuEmitterStop(std::uint32_t entityId)
{
    const auto it = m_gpuEmitters.find(entityId);
    if (it == m_gpuEmitters.end())
        return;
    it->second.started = true;  // decided: playOnStart no longer applies
    it->second.playing = false;
}

void ParticleRenderer::GpuEmitterRestart(std::uint32_t entityId)
{
    const auto it = m_gpuEmitters.find(entityId);
    if (it == m_gpuEmitters.end())
        return;
    // As the CPU path's Reset: the live particles go, emission starts again with its burst.
    it->second.started = true;
    it->second.playing = true;
    it->second.spawnAccumulator = 0.0f;
    it->second.pendingBurst = 0;
    it->second.startBurstPending = true;
    it->second.clearPending = true;
}

void ParticleRenderer::GpuEmitterEmit(std::uint32_t entityId, std::uint32_t count)
{
    const auto it = m_gpuEmitters.find(entityId);
    if (it == m_gpuEmitters.end())
        return;
    it->second.pendingBurst += static_cast<int>(std::min<std::uint32_t>(count, 10000u));
}

void ParticleRenderer::ResetGpuEmitters()
{
    // In-place reset: the buffers stay alive (a frame in flight may still read them). The next
    // dispatch kills the editor preview's particles, and each emitter starts as its component (or a
    // script's Play/Stop before then) says. (`initialized` stays: the buffer's barrier state.)
    for (auto& [entityId, emitter] : m_gpuEmitters)
    {
        (void)entityId;
        emitter.pendingBurst = 0;
        emitter.playing = true;
        emitter.entityPaused = false;
        emitter.started = false;
        emitter.startBurstPending = false;
        emitter.clearPending = true;
        emitter.spawnAccumulator = 0.0f;
        emitter.spawnCursor = 0;
    }
}

void ParticleRenderer::Destroy()
{
    m_alphaPipeline.reset();
    m_additivePipeline.reset();
    m_gpuAlphaPipeline.reset();
    m_gpuAdditivePipeline.reset();
    m_gpuSimPipeline.reset();
    m_gpuSimLayout.reset();
    m_gpuEmitters.clear();
    m_gpuVertexShader.reset();
    m_gpuPixelShader.reset();
    m_gpuSimShader.reset();
    m_bindGroup.reset();
    m_bindLayout.reset();
    for (auto& buffer : m_instanceBuffers)
        buffer.reset();
    m_instanceCapacity = {};
    for (auto& buffer : m_uniformBuffers)
        buffer.reset();
    m_textures.clear();
    m_failedTextureIds.clear();
    m_defaultTexture = {};
    m_sceneDepth.reset();
    m_sceneDepthSampler.reset();
    m_dummyDepth.reset();
    m_boundDepth = {};
    m_sceneNear = 0.1f;
    m_sceneFar = 1000.0f;
    m_vertexShader.reset();
    m_pixelShader.reset();
    m_rhi = nullptr;
    m_assets = nullptr;
    m_targetPass = nullptr;
    m_lastFrameNumber = 0;
    m_drawSlotCursor = 0;
    m_uploadedFrameNumber = std::numeric_limits<std::uint64_t>::max();
    m_uploadedCount = 0;
    m_loggedCapacity = false;
}
