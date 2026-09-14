// WorldLabelRenderer — IXRHI-native implementation. Glyph rasterization (GDI),
// vertex building and pipeline state are unchanged; buffer/texture/sampler/
// descriptor/pipeline/command management crossed into IXRHI.

#include "WorldLabelRenderer.h"

#include "Debug.h"
#include "IXRHIShader.h"
#include "asset/IAssetReader.h"

#if defined(_WIN32)
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
namespace xm = ixtreeme::math;

constexpr uint32_t kMaxVertices = 131072;
constexpr uint32_t kFirstGlyph = 32;
constexpr uint32_t kLastGlyph = 126;
constexpr uint32_t kGlyphCount = kLastGlyph - kFirstGlyph + 1;
constexpr uint32_t kAtlasCell = 32;
constexpr uint32_t kAtlasColumns = 16;
constexpr uint32_t kAtlasRows = 6;
constexpr uint32_t kAtlasWidth = kAtlasColumns * kAtlasCell;
constexpr uint32_t kAtlasHeight = kAtlasRows * kAtlasCell;
constexpr float kHeadOffsetMeters = 2.0f;
constexpr float kNamePixelScale = 0.013f;
constexpr float kOutlinePixels = 1.6f;
constexpr float kOutlineAlpha = 0.85f;
constexpr float kFadeStartMeters = 25.0f;
constexpr float kFadeEndMeters = 45.0f;
constexpr bool kDepthTestLabels = true;

std::vector<std::uint32_t> ReadSpirv(client::asset::IAssetReader& assets, const std::string& path)
{
    auto bytes = assets.ReadAll(path);
    if (!bytes || bytes->empty() || bytes->size() % sizeof(std::uint32_t) != 0)
    {
        Tracenf("[WORLD-LABEL] failed to open shader: %s", path.c_str());
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

std::string ToPrintableAscii(const std::string& text)
{
    std::string result;
    result.reserve(text.size());
    for (unsigned char c : text)
        result.push_back(c >= kFirstGlyph && c <= kLastGlyph ? static_cast<char>(c) : '?');
    return result;
}

WorldVec3 AddScaled(WorldVec3 origin, WorldVec3 right, float x, WorldVec3 up, float y)
{
    return {
        origin.x + right.x * x + up.x * y,
        origin.y + right.y * x + up.y * y,
        origin.z + right.z * x + up.z * y};
}
}

bool WorldLabelRenderer::Create(ixrhi::IXRHIDevice& rhi, client::asset::IAssetReader& assets)
{
    Destroy();
    m_rhi = &rhi;
    m_assets = &assets;

    const bool atlas = CreateFontAtlas(rhi);
    const bool buffers = atlas ? CreateBuffers(rhi) : false;
    const bool bindings = buffers ? CreateBindGroup(rhi) : false;
    const bool pipeline = bindings ? CreatePipeline(rhi) : false;
    Tracenf("[WORLD-LABEL] Create: atlas=%d buffers=%d descriptors=%d pipeline=%d glyphs=%u",
        atlas ? 1 : 0,
        buffers ? 1 : 0,
        bindings ? 1 : 0,
        pipeline ? 1 : 0,
        kGlyphCount);

    if (atlas && buffers && bindings && pipeline)
        return true;

    Destroy();
    return false;
}

bool WorldLabelRenderer::RecreatePipeline(ixrhi::IXRHIDevice& rhi)
{
    if (!m_rhi)
        return true;

    m_rhi = &rhi;
    DestroyPipeline();
    // Deferred-true while the swapchain pass is torn down (parity); real
    // failures abort in the backend like the pre-migration VK_CHECK path.
    CreatePipeline(rhi);
    return true;
}

void WorldLabelRenderer::Render(ixrhi::IXRHICommandList& cmd,
                                const ixrhi::IXRHIFrameInfo& frame,
                                const WorldCamera& camera,
                                const std::vector<Label>& worldLabels)
{
    if (!m_pipeline || !frame.frameActive || worldLabels.empty())
        return;

    if (frame.targetWidth == 0 || frame.targetHeight == 0)
        return;

    const uint32_t frameIndex = frame.frameIndex % kFramesInFlight;
    std::vector<Vertex> vertices;
    BuildVertices(camera, worldLabels, vertices);
    if (vertices.empty())
        return;
    if (vertices.size() > kMaxVertices)
        vertices.resize(kMaxVertices - (kMaxVertices % 6u));

    UpdateUniform(frameIndex, camera);
    m_vertexBuffers[frameIndex]->Write(
        0, vertices.data(), sizeof(Vertex) * vertices.size());

    cmd.SetViewport(
        0.0f, 0.0f, static_cast<float>(frame.targetWidth), static_cast<float>(frame.targetHeight));
    cmd.SetScissor(0, 0, frame.targetWidth, frame.targetHeight);
    cmd.SetGraphicsPipeline(*m_pipeline);
    cmd.SetVertexBuffer(0, *m_vertexBuffers[frameIndex], 0);
    cmd.BindGroup(0, *m_bindGroup, frameIndex);
    cmd.Draw(static_cast<uint32_t>(vertices.size()), 1, 0, 0);

    static bool loggedRender = false;
    if (!loggedRender)
    {
        Tracenf("[WORLD-LABEL] Render: worldLabels=%zu vertices=%zu depthTest=%d",
            worldLabels.size(),
            vertices.size(),
            kDepthTestLabels ? 1 : 0);
        loggedRender = true;
    }
}

void WorldLabelRenderer::Destroy()
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
    m_fontSampler.reset();
    m_fontAtlas.reset();
    m_rhi = nullptr;
    m_assets = nullptr;
}

bool WorldLabelRenderer::CreateBuffers(ixrhi::IXRHIDevice& rhi)
{
    for (auto& buffer : m_vertexBuffers)
    {
        ixrhi::IXRHIBufferDesc desc;
        desc.sizeBytes = sizeof(Vertex) * kMaxVertices;
        desc.usage = ixrhi::IXRHIBufferUsage::Vertex;
        desc.cpuAccess = ixrhi::IXRHICpuAccess::Write;
        desc.debugName = "WorldLabel VB";
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
        desc.debugName = "WorldLabel UBO";
        buffer = rhi.CreateBuffer(desc, nullptr, 0);
        if (!buffer)
            return false;
    }
    return true;
}

bool WorldLabelRenderer::CreateFontAtlas(ixrhi::IXRHIDevice& rhi)
{
    m_fontSampler.reset();
    m_fontAtlas.reset();
    m_glyphs = {};

    std::vector<uint8_t> rgba(static_cast<size_t>(kAtlasWidth) * kAtlasHeight * 4u, 0);

#if defined(_WIN32)
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(kAtlasWidth);
    info.bmiHeader.biHeight = -static_cast<LONG>(kAtlasHeight);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC screenDc = GetDC(nullptr);
    HDC memoryDc = CreateCompatibleDC(screenDc);
    HBITMAP bitmap = CreateDIBSection(memoryDc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits)
    {
        if (bitmap)
            DeleteObject(bitmap);
        DeleteDC(memoryDc);
        ReleaseDC(nullptr, screenDc);
        return false;
    }

    HGDIOBJ oldBitmap = SelectObject(memoryDc, bitmap);
    HBRUSH blackBrush = CreateSolidBrush(RGB(0, 0, 0));
    RECT fullRect{0, 0, static_cast<LONG>(kAtlasWidth), static_cast<LONG>(kAtlasHeight)};
    FillRect(memoryDc, &fullRect, blackBrush);
    DeleteObject(blackBrush);

    HFONT font = CreateFontA(-22, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        FF_DONTCARE, "Segoe UI");
    HGDIOBJ oldFont = SelectObject(memoryDc, font);
    SetBkMode(memoryDc, TRANSPARENT);
    SetTextColor(memoryDc, RGB(255, 255, 255));

    for (uint32_t glyphIndex = 0; glyphIndex < kGlyphCount; ++glyphIndex)
    {
        const char ch = static_cast<char>(kFirstGlyph + glyphIndex);
        const uint32_t col = glyphIndex % kAtlasColumns;
        const uint32_t row = glyphIndex / kAtlasColumns;
        const int x = static_cast<int>(col * kAtlasCell + 2);
        const int y = static_cast<int>(row * kAtlasCell + 4);
        TextOutA(memoryDc, x, y, &ch, 1);

        SIZE size{};
        GetTextExtentPoint32A(memoryDc, &ch, 1, &size);

        Glyph& glyph = m_glyphs[static_cast<size_t>(ch)];
        glyph.u0 = static_cast<float>(col * kAtlasCell) / static_cast<float>(kAtlasWidth);
        glyph.v0 = static_cast<float>(row * kAtlasCell) / static_cast<float>(kAtlasHeight);
        glyph.u1 = static_cast<float>((col + 1u) * kAtlasCell) / static_cast<float>(kAtlasWidth);
        glyph.v1 = static_cast<float>((row + 1u) * kAtlasCell) / static_cast<float>(kAtlasHeight);
        glyph.width = static_cast<float>(kAtlasCell);
        glyph.height = static_cast<float>(kAtlasCell);
        glyph.advance = static_cast<float>(std::max<LONG>(size.cx + 2, 8));
    }

    SelectObject(memoryDc, oldFont);
    DeleteObject(font);

    const uint8_t* bgra = static_cast<const uint8_t*>(bits);
    for (uint32_t i = 0; i < kAtlasWidth * kAtlasHeight; ++i)
    {
        const uint8_t alpha = std::max(std::max(bgra[i * 4u + 0u], bgra[i * 4u + 1u]), bgra[i * 4u + 2u]);
        rgba[i * 4u + 0u] = 255;
        rgba[i * 4u + 1u] = 255;
        rgba[i * 4u + 2u] = 255;
        rgba[i * 4u + 3u] = alpha;
    }
    rgba[0] = 255;
    rgba[1] = 255;
    rgba[2] = 255;
    rgba[3] = 255;

    SelectObject(memoryDc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memoryDc);
    ReleaseDC(nullptr, screenDc);
#else
    for (uint32_t glyphIndex = 0; glyphIndex < kGlyphCount; ++glyphIndex)
    {
        const char ch = static_cast<char>(kFirstGlyph + glyphIndex);
        const uint32_t col = glyphIndex % kAtlasColumns;
        const uint32_t row = glyphIndex / kAtlasColumns;
        Glyph& glyph = m_glyphs[static_cast<size_t>(ch)];
        glyph.u0 = static_cast<float>(col * kAtlasCell) / static_cast<float>(kAtlasWidth);
        glyph.v0 = static_cast<float>(row * kAtlasCell) / static_cast<float>(kAtlasHeight);
        glyph.u1 = static_cast<float>((col + 1u) * kAtlasCell) / static_cast<float>(kAtlasWidth);
        glyph.v1 = static_cast<float>((row + 1u) * kAtlasCell) / static_cast<float>(kAtlasHeight);
        glyph.width = static_cast<float>(kAtlasCell);
        glyph.height = static_cast<float>(kAtlasCell);
        glyph.advance = 16.0f;

        for (uint32_t y = row * kAtlasCell + 6; y < row * kAtlasCell + kAtlasCell - 6; ++y)
        {
            for (uint32_t x = col * kAtlasCell + 6; x < col * kAtlasCell + kAtlasCell - 6; ++x)
            {
                const size_t index = (static_cast<size_t>(y) * kAtlasWidth + x) * 4u;
                rgba[index + 0] = 255;
                rgba[index + 1] = 255;
                rgba[index + 2] = 255;
                rgba[index + 3] = 180;
            }
        }
    }
#endif

    // Upload path (previously: device-local image + staging + one-time submit +
    // layout transitions + view + sampler inline). Now one backend call.
    ixrhi::IXRHITextureDesc atlasDesc;
    atlasDesc.width = kAtlasWidth;
    atlasDesc.height = kAtlasHeight;
    atlasDesc.format = ixrhi::IXRHIFormat::R8G8B8A8Unorm;
    atlasDesc.usage = ixrhi::IXRHITextureUsage::Sampled | ixrhi::IXRHITextureUsage::TransferDst;
    atlasDesc.debugName = "WorldLabel FontAtlas";
    m_fontAtlas = rhi.CreateTexture(atlasDesc, rgba.data(), rgba.size());
    if (!m_fontAtlas)
        return false;

    ixrhi::IXRHISamplerDesc samplerDesc;
    samplerDesc.minFilter = ixrhi::IXRHISamplerFilter::Linear;
    samplerDesc.magFilter = ixrhi::IXRHISamplerFilter::Linear;
    samplerDesc.mipmapFilter = ixrhi::IXRHISamplerFilter::Nearest;
    samplerDesc.addressU = ixrhi::IXRHISamplerAddress::ClampToEdge;
    samplerDesc.addressV = ixrhi::IXRHISamplerAddress::ClampToEdge;
    samplerDesc.addressW = ixrhi::IXRHISamplerAddress::ClampToEdge;
    samplerDesc.maxLod = 1.0f;
    samplerDesc.debugName = "WorldLabel FontSampler";
    m_fontSampler = rhi.CreateSampler(samplerDesc);
    return m_fontSampler != nullptr;
}

bool WorldLabelRenderer::CreateBindGroup(ixrhi::IXRHIDevice& rhi)
{
    const std::vector<ixrhi::IXRHIBinding> bindings = {
        {0, ixrhi::IXRHIBindingType::UniformBuffer, ixrhi::IXRHIShaderStage::Vertex},
        {1, ixrhi::IXRHIBindingType::SampledTexture, ixrhi::IXRHIShaderStage::Fragment},
    };
    m_bindLayout = rhi.CreateBindGroupLayout(bindings);
    if (!m_bindLayout)
        return false;
    m_bindGroup = rhi.CreateBindGroup(*m_bindLayout, kFramesInFlight);
    if (!m_bindGroup)
        return false;

    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        m_bindGroup->UpdateBuffer(frame, 0, m_uniformBuffers[frame], 0, sizeof(UniformBlock));
        m_bindGroup->UpdateTexture(frame, 1, m_fontAtlas, m_fontSampler);
    }
    return true;
}

bool WorldLabelRenderer::CreatePipeline(ixrhi::IXRHIDevice& rhi)
{
    if (!m_assets)
        return false;

    auto vs = LoadShader(rhi,
        *m_assets,
        "assets/shaders/world_label_vs.spv",
        ixrhi::IXRHIShaderStage::Vertex,
        "VSMain");
    auto ps = LoadShader(rhi,
        *m_assets,
        "assets/shaders/world_label_ps.spv",
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
        {1, 0, ixrhi::IXRHIFormat::R32G32Float, offsetof(Vertex, uv)},
        {2, 0, ixrhi::IXRHIFormat::R32G32B32A32Float, offsetof(Vertex, color)},
    };
    desc.topology = ixrhi::IXRHIPrimitiveTopology::TriangleList;
    desc.cullMode = ixrhi::IXRHICullMode::None;
    desc.frontFace = ixrhi::IXRHIFrontFace::Clockwise;
    desc.depthTestEnable = kDepthTestLabels;
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
    desc.debugName = "WorldLabel";

    auto pipeline = rhi.CreateGraphicsPipeline(desc);
    if (!pipeline)
        return false;
    m_pipeline = std::move(pipeline);
    return true;
}

void WorldLabelRenderer::DestroyPipeline()
{
    m_pipeline.reset();
}

void WorldLabelRenderer::UpdateUniform(uint32_t frameIndex, const WorldCamera& camera)
{
    const UniformBlock uniform{camera.viewProjection};
    m_uniformBuffers[frameIndex]->Write(0, &uniform, sizeof(uniform));
}

void WorldLabelRenderer::BuildVertices(const WorldCamera& camera, const std::vector<Label>& worldLabels, std::vector<Vertex>& vertices) const
{
    const WorldVec3 forward = xm::Normalize(camera.target - camera.eye);
    WorldVec3 right = xm::Normalize(xm::Cross({0.0f, 1.0f, 0.0f}, forward));
    if (xm::Dot(right, right) <= 0.000001f)
        right = {1.0f, 0.0f, 0.0f};
    const WorldVec3 up = xm::Normalize(xm::Cross(forward, right));

    vertices.reserve(std::min<size_t>(kMaxVertices, worldLabels.size() * 96u * 6u * 2u));
    for (const Label& label : worldLabels)
    {
        const float distance = xm::Distance(label.position, camera.eye);
        const float fade = std::clamp(
            (kFadeEndMeters - distance) / (kFadeEndMeters - kFadeStartMeters),
            0.0f,
            1.0f);
        if (fade <= 0.0f)
            continue;

        const std::string text = ToPrintableAscii(label.text.empty() ? std::string("Label") : label.text);

        WorldVec3 origin = label.position + WorldVec3{0.0f, kHeadOffsetMeters, 0.0f};
        const float nameLineHeight = kAtlasCell * kNamePixelScale;
        origin = origin + up * (nameLineHeight * 0.5f);

        float textColor[4] = {label.color[0], label.color[1], label.color[2], label.color[3]};
        if (label.selected)
        {
            const float selectColor[4] = {1.0f, 0.80f, 0.22f, 0.30f};
            AppendQuad(vertices, origin, right, up, 2.2f, 0.42f, selectColor, fade);
            textColor[0] = 1.0f;
            textColor[1] = 0.86f;
            textColor[2] = 0.32f;
            textColor[3] = 1.0f;
        }
        AppendLine(vertices, origin, right, up, text, kNamePixelScale, textColor, fade);
    }
}

void WorldLabelRenderer::AppendQuad(std::vector<Vertex>& vertices, WorldVec3 origin, WorldVec3 right, WorldVec3 up,
    float width, float height, const float color[4], float fade) const
{
    if (width <= 0.0f || height <= 0.0f || vertices.size() + 6u >= kMaxVertices)
        return;

    const float halfW = width * 0.5f;
    const float halfH = height * 0.5f;
    const WorldVec3 p0 = AddScaled(origin, right, -halfW, up, halfH);
    const WorldVec3 p1 = AddScaled(origin, right, halfW, up, halfH);
    const WorldVec3 p2 = AddScaled(origin, right, halfW, up, -halfH);
    const WorldVec3 p3 = AddScaled(origin, right, -halfW, up, -halfH);
    const float u = 0.5f / static_cast<float>(kAtlasWidth);
    const float v = 0.5f / static_cast<float>(kAtlasHeight);

    auto makeVertex = [&](WorldVec3 p)
    {
        Vertex vertex{};
        vertex.position[0] = p.x;
        vertex.position[1] = p.y;
        vertex.position[2] = p.z;
        vertex.uv[0] = u;
        vertex.uv[1] = v;
        vertex.color[0] = color[0];
        vertex.color[1] = color[1];
        vertex.color[2] = color[2];
        vertex.color[3] = color[3] * fade;
        return vertex;
    };

    vertices.push_back(makeVertex(p0));
    vertices.push_back(makeVertex(p1));
    vertices.push_back(makeVertex(p2));
    vertices.push_back(makeVertex(p0));
    vertices.push_back(makeVertex(p2));
    vertices.push_back(makeVertex(p3));
}

void WorldLabelRenderer::AppendLine(std::vector<Vertex>& vertices, WorldVec3 origin, WorldVec3 right, WorldVec3 up,
    const std::string& text, float pixelScale, const float color[4], float fade) const
{
    if (text.empty() || vertices.size() + 6u >= kMaxVertices)
        return;

    float textWidth = 0.0f;
    for (unsigned char c : text)
    {
        const char glyphChar = c >= kFirstGlyph && c <= kLastGlyph ? static_cast<char>(c) : '?';
        textWidth += m_glyphs[static_cast<size_t>(glyphChar)].advance * pixelScale;
    }

    auto appendPass = [&](float offsetX, float offsetY, const float passColor[4])
    {
        float penX = -textWidth * 0.5f + offsetX;
        for (unsigned char c : text)
        {
            const char glyphChar = c >= kFirstGlyph && c <= kLastGlyph ? static_cast<char>(c) : '?';
            const Glyph& glyph = m_glyphs[static_cast<size_t>(glyphChar)];
            const float x0 = penX;
            const float x1 = penX + glyph.width * pixelScale;
            const float y0 = offsetY;
            const float y1 = offsetY - glyph.height * pixelScale;

            const WorldVec3 p0 = AddScaled(origin, right, x0, up, y0);
            const WorldVec3 p1 = AddScaled(origin, right, x1, up, y0);
            const WorldVec3 p2 = AddScaled(origin, right, x1, up, y1);
            const WorldVec3 p3 = AddScaled(origin, right, x0, up, y1);

            auto makeVertex = [&](WorldVec3 p, float u, float v)
            {
                Vertex vertex{};
                vertex.position[0] = p.x;
                vertex.position[1] = p.y;
                vertex.position[2] = p.z;
                vertex.uv[0] = u;
                vertex.uv[1] = v;
                vertex.color[0] = passColor[0];
                vertex.color[1] = passColor[1];
                vertex.color[2] = passColor[2];
                vertex.color[3] = passColor[3];
                return vertex;
            };

            if (vertices.size() + 6u >= kMaxVertices)
                return;

            vertices.push_back(makeVertex(p0, glyph.u0, glyph.v0));
            vertices.push_back(makeVertex(p1, glyph.u1, glyph.v0));
            vertices.push_back(makeVertex(p2, glyph.u1, glyph.v1));
            vertices.push_back(makeVertex(p0, glyph.u0, glyph.v0));
            vertices.push_back(makeVertex(p2, glyph.u1, glyph.v1));
            vertices.push_back(makeVertex(p3, glyph.u0, glyph.v1));
            penX += glyph.advance * pixelScale;
        }
    };

    const float outlineColor[4] = {0.0f, 0.0f, 0.0f, kOutlineAlpha * fade};
    const float fillColor[4] = {color[0], color[1], color[2], color[3] * fade};
    const float diagonal = 0.70710678f;
    const float outlineOffset = kOutlinePixels * pixelScale;
    const float offsets[8][2] =
    {
        { 1.0f, 0.0f},
        {-1.0f, 0.0f},
        { 0.0f, 1.0f},
        { 0.0f,-1.0f},
        { diagonal, diagonal},
        {-diagonal, diagonal},
        { diagonal,-diagonal},
        {-diagonal,-diagonal},
    };

    for (const float (&offset)[2] : offsets)
        appendPass(offset[0] * outlineOffset, offset[1] * outlineOffset, outlineColor);
    appendPass(0.0f, 0.0f, fillColor);
}
