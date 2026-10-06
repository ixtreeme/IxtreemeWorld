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

bool DecodeTextureFile(const std::string& path, int& width, int& height, std::vector<std::uint8_t>& pixels)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    std::vector<std::uint8_t> encoded((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (encoded.empty())
        return false;

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

    if (!CreateBuffers(rhi) || !CreateDefaultTexture(rhi) || !CreateDefaultDepthTexture(rhi) ||
        !CreateBindGroup(rhi))
    {
        TraceError("[PARTICLE] renderer resource creation failed");
        Destroy();
        return false;
    }
    return RecreatePipeline(rhi);
}

bool ParticleRenderer::CreateBuffers(ixrhi::IXRHIDevice& rhi)
{
    for (std::uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        ixrhi::IXRHIBufferDesc desc;
        desc.sizeBytes = sizeof(InstanceData) * kMaxInstances;
        desc.usage = ixrhi::IXRHIBufferUsage::Storage;
        desc.cpuAccess = ixrhi::IXRHICpuAccess::Write;
        desc.debugName = "Particle:Instances";
        m_instanceBuffers[frame] = rhi.CreateBuffer(desc, nullptr, 0);
        if (!m_instanceBuffers[frame])
            return false;
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
        {0, ixrhi::IXRHIBindingType::UniformBuffer, ixrhi::IXRHIShaderStage::Vertex},
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
                sizeof(InstanceData) * kMaxInstances);
            if (m_dummyDepth && m_defaultTexture.sampler)
                m_bindGroup->UpdateTexture(setIndex, 3, m_dummyDepth, m_defaultTexture.sampler);
        }
    }
    m_boundDepth = m_dummyDepth.get();
    return true;
}

void ParticleRenderer::SetSceneDepth(std::shared_ptr<ixrhi::IXRHITexture> depth,
                                     std::shared_ptr<ixrhi::IXRHISampler> sampler,
                                     float nearPlane,
                                     float farPlane)
{
    m_sceneNear = nearPlane > 0.0f ? nearPlane : 0.1f;
    m_sceneFar = farPlane > m_sceneNear ? farPlane : m_sceneNear + 1.0f;
    if (!depth || !sampler)
    {
        if (m_boundDepth != m_dummyDepth.get() && m_bindGroup && m_dummyDepth && m_defaultTexture.sampler)
        {
            for (std::uint32_t slot = 0; slot < kFramesInFlight * kDrawSlots; ++slot)
                m_bindGroup->UpdateTexture(slot, 3, m_dummyDepth, m_defaultTexture.sampler);
            m_boundDepth = m_dummyDepth.get();
        }
        return;
    }
    m_sceneDepth = std::move(depth);
    m_sceneDepthSampler = std::move(sampler);
    if (!m_bindGroup || m_boundDepth == m_sceneDepth.get())
        return;
    for (std::uint32_t slot = 0; slot < kFramesInFlight * kDrawSlots; ++slot)
        m_bindGroup->UpdateTexture(slot, 3, m_sceneDepth, m_sceneDepthSampler);
    m_boundDepth = m_sceneDepth.get();
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
    if (!DecodeTextureFile(path, width, height, pixels))
    {
        TraceError("[PARTICLE] texture '%s' could not be decoded (using the default sprite)", path.c_str());
        m_failedTextureIds.push_back(assetId);
        return &m_defaultTexture;
    }

    ixrhi::IXRHITextureDesc desc;
    desc.width = static_cast<std::uint32_t>(width);
    desc.height = static_cast<std::uint32_t>(height);
    desc.format = ixrhi::IXRHIFormat::R8G8B8A8Unorm;
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
    if (!m_rhi || !frame.frameActive || batch.instances.empty())
        return;
    const ixrhi::IXRHIGraphicsPipeline* pipeline =
        batch.additive ? m_additivePipeline.get() : m_alphaPipeline.get();
    if (!pipeline || !m_bindGroup)
        return;
    if (width == 0 || height == 0)
        return;

    const std::uint32_t frameIndex = frame.frameIndex % kFramesInFlight;
    if (frame.frameNumber != m_lastFrameNumber)
    {
        m_lastFrameNumber = frame.frameNumber;
        m_instanceCursor = 0;
        m_drawSlotCursor = 0;
    }
    if (m_instanceCursor >= kMaxInstances)
        return;  // this frame's buffer is full (the capacity warning was logged)

    std::uint32_t count = static_cast<std::uint32_t>(batch.instances.size());
    if (count > kMaxInstances - m_instanceCursor)
    {
        count = kMaxInstances - m_instanceCursor;
        if (!m_loggedCapacity)
        {
            TraceError("[PARTICLE] instance buffer full (%u): %zu particles dropped this frame",
                kMaxInstances, batch.instances.size() - count);
            m_loggedCapacity = true;
        }
    }
    if (count == 0)
        return;

    const std::uint32_t base = m_instanceCursor;
    m_instanceBuffers[frameIndex]->Write(static_cast<std::uint64_t>(base) * sizeof(InstanceData),
        batch.instances.data(), static_cast<std::size_t>(count) * sizeof(InstanceData));
    m_instanceCursor = base + count;

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

    const std::uint32_t slotInFrame = m_drawSlotCursor++ % kDrawSlots;
    const std::uint32_t bindSlot = frameIndex * kDrawSlots + slotInFrame;
    m_uniformBuffers[bindSlot]->Write(0, &uniform, sizeof(uniform));
    const TextureEntry* texture = ResolveTexture(batch.textureAssetId);
    if (texture && texture->texture && texture->sampler)
        m_bindGroup->UpdateTexture(bindSlot, 1, texture->texture, texture->sampler);

    cmd.SetViewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    cmd.SetScissor(0, 0, width, height);
    cmd.SetGraphicsPipeline(*pipeline);
    cmd.BindGroup(0, *m_bindGroup, bindSlot);
    cmd.Draw(6, count, 0, base);
}

void ParticleRenderer::Destroy()
{
    m_alphaPipeline.reset();
    m_additivePipeline.reset();
    m_bindGroup.reset();
    m_bindLayout.reset();
    for (auto& buffer : m_instanceBuffers)
        buffer.reset();
    for (auto& buffer : m_uniformBuffers)
        buffer.reset();
    m_textures.clear();
    m_failedTextureIds.clear();
    m_defaultTexture = {};
    m_sceneDepth.reset();
    m_sceneDepthSampler.reset();
    m_dummyDepth.reset();
    m_boundDepth = nullptr;
    m_sceneNear = 0.1f;
    m_sceneFar = 1000.0f;
    m_vertexShader.reset();
    m_pixelShader.reset();
    m_rhi = nullptr;
    m_assets = nullptr;
    m_targetPass = nullptr;
    m_lastFrameNumber = 0;
    m_instanceCursor = 0;
    m_drawSlotCursor = 0;
    m_loggedCapacity = false;
}
