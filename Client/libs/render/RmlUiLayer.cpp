#include "RmlUiLayer.h"

#include "Debug.h"
#include "IXRHIBinding.h"
#include "IXRHIBuffer.h"
#include "IXRHICommandList.h"
#include "IXRHIDevice.h"
#include "IXRHIPipeline.h"
#include "IXRHIShader.h"
#include "IXRHITexture.h"
#include "asset/IAssetReader.h"
#include <RmlUi/Core.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/SystemInterface.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <charconv>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
// Loads SPIR-V words via the asset reader (aborts like the old shader loader
// when the asset is missing or misaligned).
std::vector<std::uint32_t> ReadSpirv(client::asset::IAssetReader& assets, const std::string& path)
{
    auto bytes = assets.ReadAll(path);
    if (!bytes || bytes->empty() || bytes->size() % sizeof(std::uint32_t) != 0)
    {
        Tracenf("[RMLUI] failed to read shader: %s", path.c_str());
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

std::shared_ptr<ixrhi::IXRHIBuffer> CreateRhiBuffer(ixrhi::IXRHIDevice& rhi,
                                                    std::uint64_t sizeBytes,
                                                    ixrhi::IXRHIBufferUsage usage,
                                                    const void* initialData,
                                                    const char* debugName)
{
    ixrhi::IXRHIBufferDesc desc;
    desc.sizeBytes = sizeBytes;
    desc.usage = usage;
    desc.cpuAccess = ixrhi::IXRHICpuAccess::Write;
    desc.debugName = debugName ? debugName : "";
    const std::size_t bytes = static_cast<std::size_t>(sizeBytes);
    return rhi.CreateBuffer(desc, initialData, initialData != nullptr ? bytes : 0);
}

std::string NormalizeAssetPath(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    while (path.rfind("./", 0) == 0)
        path.erase(0, 2);
    return path;
}

std::string CodepointToUtf8(uint32_t codepoint)
{
    std::string result;
    if (codepoint <= 0x7f)
    {
        result.push_back(static_cast<char>(codepoint));
    }
    else if (codepoint <= 0x7ff)
    {
        result.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
        result.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
    else if (codepoint <= 0xffff)
    {
        result.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
        result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        result.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
    else
    {
        result.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
        result.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
        result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        result.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
    return result;
}

Rml::Input::KeyIdentifier ToRmlKey(Key key)
{
    using namespace Rml::Input;
    if (key >= Key_A && key <= Key_Z)
        return static_cast<KeyIdentifier>(KI_A + (key - Key_A));
    if (key >= Key_0 && key <= Key_9)
        return static_cast<KeyIdentifier>(KI_0 + (key - Key_0));

    switch (key)
    {
    case Key_Enter: return KI_RETURN;
    case Key_Escape: return KI_ESCAPE;
    case Key_Backspace: return KI_BACK;
    case Key_Tab: return KI_TAB;
    case Key_Space: return KI_SPACE;
    case Key_Left: return KI_LEFT;
    case Key_Right: return KI_RIGHT;
    case Key_Up: return KI_UP;
    case Key_Down: return KI_DOWN;
    case Key_Shift: return KI_LSHIFT;
    case Key_Control: return KI_LCONTROL;
    case Key_Delete: return KI_DELETE;
    case Key_Home: return KI_HOME;
    case Key_End: return KI_END;
    case Key_F1: return KI_F1;
    case Key_F2: return KI_F2;
    case Key_F4: return KI_F4;
    case Key_F5: return KI_F5;
    case Key_F6: return KI_F6;
    case Key_F7: return KI_F7;
    case Key_F8: return KI_F8;
    default: return KI_UNKNOWN;
    }
}

int ToRmlMouseButton(MouseButton button)
{
    switch (button)
    {
    case MouseButton_Left: return 0;
    case MouseButton_Right: return 1;
    case MouseButton_Middle: return 2;
    default: return 0;
    }
}

constexpr uint32_t kLobbyPlayerSlots = 4;

// Maximum live UI textures (font atlas + generated images). Mirrors the old
// descriptor-pool cap (256 sets); each texture owns one bind-group slot.
constexpr std::uint32_t kMaxTextureSlots = 256;
constexpr std::uint32_t kInvalidTextureSlot = 0xffffffffu;

struct RmlGpuBuffer
{
    std::shared_ptr<ixrhi::IXRHIBuffer> buffer;
    std::uint64_t sizeBytes = 0;
};

struct RmlGpuTexture
{
    std::shared_ptr<ixrhi::IXRHITexture> image;
    std::uint32_t slot = kInvalidTextureSlot; // bind-group set index
    uint32_t width = 0;
    uint32_t height = 0;
};

struct Geometry
{
    RmlGpuBuffer vertices;
    RmlGpuBuffer indices;
    std::vector<Rml::Vertex> diagVertices;
    std::vector<int> diagIndices;
    uint32_t indexCount = 0;
};

struct PendingGeometryDelete
{
    RmlGpuBuffer vertices;
    RmlGpuBuffer indices;
    uint64_t retireFrame = 0;
};

struct PendingTextureDelete
{
    RmlGpuTexture texture;
    uint64_t retireFrame = 0;
};

// Rml::Vertex layout contract (RmlUi 6.2): position vec2, premultiplied
// RGBA colour bytes, texcoord vec2 — must match the IXRHI vertex attributes.
static_assert(sizeof(Rml::Vertex) == 20, "RmlUi vertex stride must stay 20 bytes");
static_assert(offsetof(Rml::Vertex, position) == 0, "RmlUi position offset changed");
static_assert(offsetof(Rml::Vertex, colour) == 8, "RmlUi colour offset changed");
static_assert(offsetof(Rml::Vertex, tex_coord) == 12, "RmlUi texcoord offset changed");

class RmlAssetFileInterface final : public Rml::FileInterface
{
public:
    explicit RmlAssetFileInterface(client::asset::IAssetReader& assets) : m_assets(assets) {}

    Rml::FileHandle Open(const Rml::String& path) override
    {
        const std::string normalized = NormalizeAssetPath(path);
        auto bytes = m_assets.ReadAll(normalized);
        if (!bytes)
        {
            Tracenf("[RMLUI] File open failed: %s", normalized.c_str());
            return 0;
        }

        auto file = std::make_unique<File>();
        file->bytes = std::move(*bytes);
        file->position = 0;
        const Rml::FileHandle handle = ++m_nextHandle;
        m_files.emplace(handle, std::move(file));
        return handle;
    }

    void Close(Rml::FileHandle file) override
    {
        m_files.erase(file);
    }

    size_t Read(void* buffer, size_t size, Rml::FileHandle file) override
    {
        File* item = Find(file);
        if (!item || !buffer)
            return 0;

        const size_t available = item->position < item->bytes.size() ? item->bytes.size() - item->position : 0;
        const size_t count = std::min(size, available);
        if (count > 0)
        {
            std::memcpy(buffer, item->bytes.data() + item->position, count);
            item->position += count;
        }
        return count;
    }

    bool Seek(Rml::FileHandle file, long offset, int origin) override
    {
        File* item = Find(file);
        if (!item)
            return false;

        long long base = 0;
        if (origin == SEEK_CUR)
            base = static_cast<long long>(item->position);
        else if (origin == SEEK_END)
            base = static_cast<long long>(item->bytes.size());

        const long long next = base + offset;
        if (next < 0)
            return false;

        item->position = static_cast<size_t>(std::min<long long>(next, static_cast<long long>(item->bytes.size())));
        return true;
    }

    size_t Tell(Rml::FileHandle file) override
    {
        File* item = Find(file);
        return item ? item->position : 0;
    }

private:
    struct File
    {
        std::vector<std::uint8_t> bytes;
        size_t position = 0;
    };

    File* Find(Rml::FileHandle file)
    {
        auto it = m_files.find(file);
        return it == m_files.end() ? nullptr : it->second.get();
    }

    client::asset::IAssetReader& m_assets;
    Rml::FileHandle m_nextHandle = 1;
    std::unordered_map<Rml::FileHandle, std::unique_ptr<File>> m_files;
};

class RmlSystemInterface final : public Rml::SystemInterface
{
public:
    double GetElapsedTime() override
    {
        const auto now = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(now - m_start).count();
    }

    void JoinPath(Rml::String& translated_path, const Rml::String& document_path, const Rml::String& path) override
    {
        if (path.rfind("assets/", 0) == 0 || path.rfind("/", 0) == 0)
        {
            translated_path = NormalizeAssetPath(path);
            return;
        }

        std::string base = NormalizeAssetPath(document_path);
        const size_t slash = base.find_last_of('/');
        translated_path = slash == std::string::npos ? NormalizeAssetPath(path) : base.substr(0, slash + 1) + NormalizeAssetPath(path);
    }

    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override
    {
        const char* severity = "INFO";
        if (type == Rml::Log::LT_ERROR)
            severity = "ERROR";
        else if (type == Rml::Log::LT_WARNING)
            severity = "WARN";
        Tracenf("[RMLUI] %s: %s", severity, message.c_str());
        return true;
    }

private:
    std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now();
};

class LoginButtonHandler final : public Rml::EventListener
{
public:
    std::function<void()> submit;

    void ProcessEvent(Rml::Event& event) override
    {
        Tracenf("[RMLUI] Event: type=%s element=login-button", event.GetType().c_str());
        if (submit)
            submit();
    }
};

class ClickHandler final : public Rml::EventListener
{
public:
    std::function<void(Rml::Event&)> callback;

    void ProcessEvent(Rml::Event& event) override
    {
        if (callback)
            callback(event);
    }
};

class RmlRenderInterface final : public Rml::RenderInterface
{
public:
    bool Create(ixrhi::IXRHIDevice& rhi, client::asset::IAssetReader& assets, uint32_t width, uint32_t height)
    {
        m_rhi = &rhi;
        m_assets = &assets;
        m_width = width;
        m_height = height;
        LogProjectionMatrix();

        if (!CreateBindGroup(rhi))
            return false;
        CreateWhiteTexture();
        return CreatePipeline(rhi);
    }

    void Destroy()
    {
        if (!m_rhi)
            return;

        // Teardown parity with the old device-wait-idle: in-flight frames may
        // still reference geometry/texture buffers being released here.
        m_rhi->WaitIdle();
        m_geometries.clear();
        m_pendingGeometryDeletes.clear();
        m_pendingTextureDeletes.clear();
        m_textures.clear();
        m_whiteTexture = nullptr;

        m_pipeline.reset();
        m_bindGroup.reset();
        m_bindLayout.reset();
        m_sampler.reset();
        m_targetPass = nullptr;
        m_rhi = nullptr;
        m_assets = nullptr;
    }

    void SetTargetPass(const ixrhi::IXRHIRenderPass* pass) { m_targetPass = pass; }

    void Resize(uint32_t width, uint32_t height)
    {
        m_width = width;
        m_height = height;
        LogProjectionMatrix();
    }

    bool RecreatePipeline(ixrhi::IXRHIDevice& rhi)
    {
        m_rhi = &rhi;
        m_pipeline.reset();
        // Deferred-false only while the target pass is torn down (parity);
        // real failures abort in the backend like the pre-migration path.
        return CreatePipeline(rhi);
    }

    void BeginFrame(ixrhi::IXRHICommandList& cmd, const ixrhi::IXRHIFrameInfo& frame)
    {
        m_cmd = &cmd;
        m_currentFrameNumber = frame.frameNumber;
        RetirePending(frame.frameNumber);
        m_drawCallsThisFrame = 0;
        if (!QuietLogsForLodDiag() && m_diagViewportLogsRemaining > 0)
        {
            Tracenf("[RMLUI-DIAG] SetViewport: (%.3f,%.3f) %.3fx%.3f depth=[%.3f,%.3f]",
                0.0f,
                0.0f,
                static_cast<float>(m_width),
                static_cast<float>(m_height),
                0.0f,
                1.0f);
        }
        cmd.SetViewport(0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height));

        if (!QuietLogsForLodDiag() && m_diagViewportLogsRemaining > 0)
        {
            Tracenf("[RMLUI-DIAG] SetScissor: offset=(%d,%d) extent=%ux%u", 0, 0, m_width, m_height);
            --m_diagViewportLogsRemaining;
        }
        cmd.SetScissor(0, 0, m_width, m_height);
    }

    void EndFrame()
    {
        if (m_drawCallsThisFrame > 0 && (++m_loggedFrameCounter % 300u) == 0u)
            Tracenf("[RMLUI] Frame rendered with %u draw calls", m_drawCallsThisFrame);
        m_cmd = nullptr;
    }

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override
    {
        if (!m_rhi || vertices.empty() || indices.empty())
            return 0;

        auto geometry = std::make_unique<Geometry>();
        geometry->indexCount = static_cast<uint32_t>(indices.size());
        geometry->diagVertices.assign(vertices.begin(), vertices.end());
        geometry->diagIndices.assign(indices.begin(), indices.end());
        geometry->vertices.sizeBytes = sizeof(Rml::Vertex) * vertices.size();
        geometry->vertices.buffer = CreateRhiBuffer(*m_rhi,
            geometry->vertices.sizeBytes,
            ixrhi::IXRHIBufferUsage::Vertex,
            vertices.data(),
            "RmlUi:GeometryVB");
        geometry->indices.sizeBytes = sizeof(int) * indices.size();
        geometry->indices.buffer = CreateRhiBuffer(*m_rhi,
            geometry->indices.sizeBytes,
            ixrhi::IXRHIBufferUsage::Index,
            indices.data(),
            "RmlUi:GeometryIB");
        if (!geometry->vertices.buffer || !geometry->indices.buffer)
            return 0;

        const Rml::CompiledGeometryHandle handle = ++m_nextGeometryHandle;
        m_geometries.emplace(handle, std::move(geometry));
        return handle;
    }

    void RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation, Rml::TextureHandle texture) override
    {
        if (!m_cmd || !m_pipeline || !m_bindGroup)
            return;

        auto geometryIt = m_geometries.find(handle);
        if (geometryIt == m_geometries.end())
            return;

        // Untextured geometry falls back to the 1x1 white texture (parity);
        // an unknown handle keeps the previous texture the same way.
        const RmlGpuTexture* gpuTexture = m_whiteTexture;
        if (texture != 0)
        {
            auto textureIt = m_textures.find(texture);
            if (textureIt != m_textures.end())
                gpuTexture = textureIt->second.get();
        }
        if (!gpuTexture || !gpuTexture->image || gpuTexture->slot == kInvalidTextureSlot)
            return;

        const Geometry& geometry = *geometryIt->second;
        if (!geometry.vertices.buffer || !geometry.indices.buffer)
            return;
        LogGeometryDiagnostics(geometry, translation, texture);
        struct PushConstants
        {
            float viewport[2];
            float translation[2];
        } push{{static_cast<float>(m_width), static_cast<float>(m_height)}, {translation.x, translation.y}};

        m_cmd->SetGraphicsPipeline(*m_pipeline);
        m_cmd->BindGroup(0, *m_bindGroup, gpuTexture->slot);
        m_cmd->PushConstants(&push, sizeof(push));

        m_cmd->SetVertexBuffer(0, *geometry.vertices.buffer, 0);
        m_cmd->SetIndexBuffer(*geometry.indices.buffer, 0, /*thirtyTwoBit=*/true);
        m_cmd->DrawIndexed(geometry.indexCount, 1, 0, 0, 0);
        ++m_drawCallsThisFrame;
    }

    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override
    {
        auto it = m_geometries.find(geometry);
        if (it == m_geometries.end())
            return;
        PendingGeometryDelete pending{};
        pending.vertices = it->second->vertices;
        pending.indices = it->second->indices;
        pending.retireFrame = m_currentFrameNumber + 3u;
        m_pendingGeometryDeletes.push_back(std::move(pending));
        m_geometries.erase(it);
    }

    Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) override
    {
        texture_dimensions = {};
        Tracenf("[RMLUI] Texture load skipped (no image decoder in RMLUI-1): %s", source.c_str());
        return 0;
    }

    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions) override
    {
        if (!m_rhi || source.empty() || dimensions.x <= 0 || dimensions.y <= 0)
            return 0;
        auto texture = std::make_unique<RmlGpuTexture>();
        if (!CreateTexture(source.data(),
                static_cast<uint32_t>(dimensions.x),
                static_cast<uint32_t>(dimensions.y),
                *texture))
            return 0;
        const Rml::TextureHandle handle = ++m_nextTextureHandle;
        m_textures.emplace(handle, std::move(texture));
        Tracenf("[RMLUI] Texture loaded: source=<generated> dimensions=%dx%d", dimensions.x, dimensions.y);
        return handle;
    }

    void ReleaseTexture(Rml::TextureHandle texture) override
    {
        auto it = m_textures.find(texture);
        if (it == m_textures.end())
            return;
        if (it->second.get() == m_whiteTexture)
            m_whiteTexture = nullptr;
        PendingTextureDelete pending{};
        pending.texture = *it->second;
        pending.retireFrame = m_currentFrameNumber + 3u;
        m_pendingTextureDeletes.push_back(std::move(pending));
        m_textures.erase(it);
    }

    void EnableScissorRegion(bool enable) override
    {
        m_scissorEnabled = enable;
        ApplyScissor();
    }

    void SetScissorRegion(Rml::Rectanglei region) override
    {
        m_scissor = region;
        ApplyScissor();
    }

    void SetTransform(const Rml::Matrix4f*) override
    {
    }

private:
    bool CreateBindGroup(ixrhi::IXRHIDevice& rhi)
    {
        // Separate image + sampler bindings: the fragment shader declares
        // g_texture (t0) and g_sampler (s0) apart, so the combined
        // SampledTexture binding cannot express this layout.
        const std::vector<ixrhi::IXRHIBinding> bindings = {
            {0, ixrhi::IXRHIBindingType::SampledImage, ixrhi::IXRHIShaderStage::Fragment},
            {1, ixrhi::IXRHIBindingType::Sampler, ixrhi::IXRHIShaderStage::Fragment},
        };
        m_bindLayout = rhi.CreateBindGroupLayout(bindings);
        if (!m_bindLayout)
            return false;
        m_bindGroup = rhi.CreateBindGroup(*m_bindLayout, kMaxTextureSlots);
        if (!m_bindGroup)
            return false;

        ixrhi::IXRHISamplerDesc samplerDesc;
        samplerDesc.minFilter = ixrhi::IXRHISamplerFilter::Linear;
        samplerDesc.magFilter = ixrhi::IXRHISamplerFilter::Linear;
        samplerDesc.mipmapFilter = ixrhi::IXRHISamplerFilter::Nearest;
        samplerDesc.addressU = ixrhi::IXRHISamplerAddress::ClampToEdge;
        samplerDesc.addressV = ixrhi::IXRHISamplerAddress::ClampToEdge;
        samplerDesc.addressW = ixrhi::IXRHISamplerAddress::ClampToEdge;
        samplerDesc.maxLod = 1.0f;
        samplerDesc.debugName = "RmlUi:Sampler";
        m_sampler = rhi.CreateSampler(samplerDesc);
        return m_sampler != nullptr;
    }

    std::uint32_t AllocTextureSlot()
    {
        if (!m_freeSlots.empty())
        {
            const std::uint32_t slot = m_freeSlots.back();
            m_freeSlots.pop_back();
            return slot;
        }
        if (m_nextSlot >= kMaxTextureSlots)
            return kInvalidTextureSlot;
        return m_nextSlot++;
    }

    void FreeTextureSlot(std::uint32_t slot)
    {
        if (slot != kInvalidTextureSlot)
            m_freeSlots.push_back(slot);
    }

    bool CreateWhiteTexture()
    {
        const std::uint8_t white[] = {255, 255, 255, 255};
        auto texture = std::make_unique<RmlGpuTexture>();
        if (!CreateTexture(white, 1, 1, *texture))
            return false;
        m_whiteTexture = texture.get();
        m_textures.emplace(++m_nextTextureHandle, std::move(texture));
        return true;
    }

    bool CreatePipeline(ixrhi::IXRHIDevice& rhi)
    {
        if (!m_assets || !m_bindLayout)
            return false;

        auto vs = LoadShader(rhi, *m_assets, "assets/shaders/rmlui_vs.spv",
            ixrhi::IXRHIShaderStage::Vertex, "VSMain");
        auto ps = LoadShader(rhi, *m_assets, "assets/shaders/rmlui_ps.spv",
            ixrhi::IXRHIShaderStage::Fragment, "PSMain");
        if (!vs || !ps)
            return false;

        // Parity with the pre-migration native state: triangle list, fill,
        // cull-none, depth off, premultiplied-alpha blend
        // (ONE, ONE_MINUS_SRC_ALPHA on color and alpha), 1 sample, dynamic
        // viewport/scissor (implicit in the IXRHI backend), push-constant
        // viewport+translation in the vertex stage.
        Tracen("[RMLUI-DIAG] Pipeline primitive topology: TRIANGLE_LIST");
        Tracenf("[RMLUI-DIAG] Vertex stride: %zu", sizeof(Rml::Vertex));
        Tracenf("[RMLUI-DIAG]   position offset: %zu", offsetof(Rml::Vertex, position));
        Tracenf("[RMLUI-DIAG]   tex_coord offset: %zu", offsetof(Rml::Vertex, tex_coord));
        Tracenf("[RMLUI-DIAG]   colour offset: %zu", offsetof(Rml::Vertex, colour));

        ixrhi::IXRHIGraphicsPipelineDesc desc;
        desc.vertexShader = vs;
        desc.fragmentShader = ps;
        desc.bindGroupLayouts = {m_bindLayout.get()};
        desc.pushRanges = {{ixrhi::IXRHIShaderStage::Vertex, 0, sizeof(float) * 4}};
        desc.vertexBindings = {{0, sizeof(Rml::Vertex)}};
        desc.vertexAttributes = {
            {0, 0, ixrhi::IXRHIFormat::R32G32Float, offsetof(Rml::Vertex, position)},
            {1, 0, ixrhi::IXRHIFormat::R8G8B8A8Unorm, offsetof(Rml::Vertex, colour)},
            {2, 0, ixrhi::IXRHIFormat::R32G32Float, offsetof(Rml::Vertex, tex_coord)},
        };
        desc.topology = ixrhi::IXRHIPrimitiveTopology::TriangleList;
        desc.cullMode = ixrhi::IXRHICullMode::None;
        desc.frontFace = ixrhi::IXRHIFrontFace::Clockwise;
        desc.depthTestEnable = false;
        desc.depthWriteEnable = false;
        desc.depthCompareOp = ixrhi::IXRHICompareOp::Less;
        desc.blendAttachments = {{true,
            ixrhi::IXRHIBlendFactor::One,
            ixrhi::IXRHIBlendFactor::OneMinusSrcAlpha,
            ixrhi::IXRHIBlendOp::Add,
            ixrhi::IXRHIBlendFactor::One,
            ixrhi::IXRHIBlendFactor::OneMinusSrcAlpha,
            ixrhi::IXRHIBlendOp::Add}};
        desc.sampleCount = 1;
        desc.targetRenderPass = m_targetPass;
        desc.debugName = "RmlUi:Textured";
        m_pipeline = rhi.CreateGraphicsPipeline(desc);
        return m_pipeline != nullptr;
    }

    // RmlUi texture upload: premultiplied-alpha RGBA8 (font atlas + runtime
    // images), linear clamp sampling, single level — format and sampling
    // preserved exactly, staging + TransferDst→ShaderRead transition owned
    // by the backend (no queue stall on the UI path).
    bool CreateTexture(const void* rgba, uint32_t width, uint32_t height, RmlGpuTexture& out)
    {
        if (!m_rhi || !m_bindGroup || !m_sampler || rgba == nullptr || width == 0 || height == 0)
            return false;

        const std::uint32_t slot = AllocTextureSlot();
        if (slot == kInvalidTextureSlot)
        {
            Tracenf("[RMLUI] texture slot pool exhausted (%u live textures)", kMaxTextureSlots);
            return false;
        }

        const std::size_t byteSize = static_cast<std::size_t>(width) * height * 4u;
        ixrhi::IXRHITextureDesc imageDesc;
        imageDesc.width = width;
        imageDesc.height = height;
        imageDesc.format = ixrhi::IXRHIFormat::R8G8B8A8Unorm;
        imageDesc.usage = ixrhi::IXRHITextureUsage::Sampled | ixrhi::IXRHITextureUsage::TransferDst;
        imageDesc.debugName = "RmlUi:Generated";
        out.image = m_rhi->CreateTexture(imageDesc, rgba, byteSize);
        if (!out.image)
        {
            FreeTextureSlot(slot);
            return false;
        }

        m_bindGroup->UpdateSampledImage(slot, 0, out.image);
        m_bindGroup->UpdateSampler(slot, 1, m_sampler);
        out.slot = slot;
        out.width = width;
        out.height = height;
        return true;
    }

    void RetirePending(uint64_t frameNumber)
    {
        // Same guarantee as the pre-migration safe-frame retirement
        // (release at frame N is destroyed once the frame counter passes
        // N+3 — conservative over 2 frames in flight): resetting the last
        // shared_ptr here cannot race in-flight command lists, and a freed
        // bind-group slot is only reused afterwards.
        auto it = m_pendingGeometryDeletes.begin();
        while (it != m_pendingGeometryDeletes.end())
        {
            if (it->retireFrame > frameNumber)
            {
                ++it;
                continue;
            }
            it = m_pendingGeometryDeletes.erase(it);
        }

        auto textureIt = m_pendingTextureDeletes.begin();
        while (textureIt != m_pendingTextureDeletes.end())
        {
            if (textureIt->retireFrame > frameNumber)
            {
                ++textureIt;
                continue;
            }
            FreeTextureSlot(textureIt->texture.slot);
            textureIt = m_pendingTextureDeletes.erase(textureIt);
        }
    }

    void ApplyScissor()
    {
        if (!m_cmd)
            return;

        // Clamp math preserved exactly (negative origins, viewport clip,
        // zero-area collapse); only the recording call changed.
        std::uint32_t scissorX = 0;
        std::uint32_t scissorY = 0;
        std::uint32_t scissorW = m_width;
        std::uint32_t scissorH = m_height;
        if (m_scissorEnabled)
        {
            const int left = std::max(0, m_scissor.Left());
            const int top = std::max(0, m_scissor.Top());
            const int right = std::min<int>(static_cast<int>(m_width), m_scissor.Right());
            const int bottom = std::min<int>(static_cast<int>(m_height), m_scissor.Bottom());
            scissorX = static_cast<std::uint32_t>(left);
            scissorY = static_cast<std::uint32_t>(top);
            scissorW = static_cast<std::uint32_t>(std::max(0, right - left));
            scissorH = static_cast<std::uint32_t>(std::max(0, bottom - top));
        }
        if (!QuietLogsForLodDiag() && m_diagScissorLogsRemaining > 0)
        {
            Tracenf("[RMLUI-DIAG] Rml scissor: enabled=%d region=(%d,%d)-(%d,%d) applied offset=(%d,%d) extent=%ux%u",
                m_scissorEnabled ? 1 : 0,
                m_scissor.Left(),
                m_scissor.Top(),
                m_scissor.Right(),
                m_scissor.Bottom(),
                scissorX,
                scissorY,
                scissorW,
                scissorH);
            --m_diagScissorLogsRemaining;
        }
        m_cmd->SetScissor(scissorX, scissorY, scissorW, scissorH);
    }

    void LogProjectionMatrix()
    {
        if (m_width == 0 || m_height == 0)
            return;

        const float matrix[16] = {
            2.0f / static_cast<float>(m_width), 0.0f, 0.0f, -1.0f,
            0.0f, 2.0f / static_cast<float>(m_height), 0.0f, -1.0f,
            0.0f, 0.0f, -1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f};
        Tracen("[RMLUI-DIAG] Projection matrix:");
        for (int row = 0; row < 4; ++row)
        {
            Tracenf("[RMLUI-DIAG]   [%.6f, %.6f, %.6f, %.6f]",
                matrix[row * 4 + 0],
                matrix[row * 4 + 1],
                matrix[row * 4 + 2],
                matrix[row * 4 + 3]);
        }
    }

    void LogGeometryDiagnostics(const Geometry& geometry, Rml::Vector2f translation, Rml::TextureHandle texture)
    {
        if (QuietLogsForLodDiag() || m_diagGeometryLogsRemaining <= 0)
            return;

        Tracenf("[RMLUI-DIAG] RenderGeometry: vertices=%zu indices=%zu translation=(%.3f,%.3f) texture=%llu",
            geometry.diagVertices.size(),
            geometry.diagIndices.size(),
            translation.x,
            translation.y,
            static_cast<unsigned long long>(texture));
        const size_t vertexCount = std::min<size_t>(geometry.diagVertices.size(), 4);
        for (size_t i = 0; i < vertexCount; ++i)
        {
            const Rml::Vertex& v = geometry.diagVertices[i];
            Tracenf("[RMLUI-DIAG]   vertex[%zu]: pos=(%.3f,%.3f) uv=(%.3f,%.3f) color=(%u,%u,%u,%u)",
                i,
                v.position.x,
                v.position.y,
                v.tex_coord.x,
                v.tex_coord.y,
                static_cast<unsigned>(v.colour.red),
                static_cast<unsigned>(v.colour.green),
                static_cast<unsigned>(v.colour.blue),
                static_cast<unsigned>(v.colour.alpha));
        }
        --m_diagGeometryLogsRemaining;
    }

    ixrhi::IXRHIDevice* m_rhi = nullptr;
    client::asset::IAssetReader* m_assets = nullptr;
    const ixrhi::IXRHIRenderPass* m_targetPass = nullptr; // borrowed (frame owner)
    uint32_t m_width = 1;
    uint32_t m_height = 1;
    ixrhi::IXRHICommandList* m_cmd = nullptr; // borrowed (frame recording)
    std::unique_ptr<ixrhi::IXRHIBindGroupLayout> m_bindLayout;
    std::unique_ptr<ixrhi::IXRHIBindGroup> m_bindGroup;
    std::shared_ptr<ixrhi::IXRHISampler> m_sampler; // one shared linear/clamp sampler
    std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> m_pipeline;
    bool m_scissorEnabled = false;
    Rml::Rectanglei m_scissor;
    uint32_t m_drawCallsThisFrame = 0;
    uint32_t m_loggedFrameCounter = 0;
    uint64_t m_currentFrameNumber = 0;
    int m_diagViewportLogsRemaining = 3;
    int m_diagScissorLogsRemaining = 8;
    int m_diagGeometryLogsRemaining = 5;
    Rml::CompiledGeometryHandle m_nextGeometryHandle = 1;
    Rml::TextureHandle m_nextTextureHandle = 1;
    std::uint32_t m_nextSlot = 0;
    std::vector<std::uint32_t> m_freeSlots;
    std::unordered_map<Rml::CompiledGeometryHandle, std::unique_ptr<Geometry>> m_geometries;
    std::vector<PendingGeometryDelete> m_pendingGeometryDeletes;
    std::unordered_map<Rml::TextureHandle, std::unique_ptr<RmlGpuTexture>> m_textures;
    std::vector<PendingTextureDelete> m_pendingTextureDeletes;
    RmlGpuTexture* m_whiteTexture = nullptr;
};
}

struct RmlUiLayer::Impl
{
    struct HudElements
    {
        Rml::Element* playerName = nullptr;
        Rml::Element* playerLevel = nullptr;
        Rml::Element* hpText = nullptr;
        Rml::Element* hpFill = nullptr;
        Rml::Element* mpText = nullptr;
        Rml::Element* mpFill = nullptr;
        Rml::Element* xpText = nullptr;
        Rml::Element* xpFill = nullptr;
        Rml::Element* targetFrame = nullptr;
        Rml::Element* targetName = nullptr;
        Rml::Element* targetLevel = nullptr;
        Rml::Element* targetHpFill = nullptr;
        Rml::Element* targetHpText = nullptr;
        Rml::Element* zoneName = nullptr;
        Rml::Element* playerCoords = nullptr;
    };

    client::asset::IAssetReader* assets = nullptr;
    RmlRenderInterface renderer;
    RmlSystemInterface system;
    std::unique_ptr<RmlAssetFileInterface> fileInterface;
    Rml::Context* context = nullptr;
    Rml::ElementDocument* loginDocument = nullptr;
    Rml::ElementDocument* lobbyDocument = nullptr;
    Rml::ElementDocument* hudDocument = nullptr;
    Rml::ElementDocument* menuDocument = nullptr;
    Rml::ElementDocument* settingsDocument = nullptr;
    Rml::ElementDocument* inventoryDocument = nullptr;
    Rml::ElementDocument* characterCreationDocument = nullptr;
    HudElements hud;
    LoginButtonHandler loginButtonHandler;
    ClickHandler lobbyEnterHandler;
    ClickHandler lobbyNewCharacterHandler;
    ClickHandler lobbyDeleteHandler;
    ClickHandler lobbyLogoutHandler;
    ClickHandler lobbyConfirmDeleteYesHandler;
    ClickHandler lobbyConfirmDeleteNoHandler;
    ClickHandler menuResumeHandler;
    ClickHandler menuSettingsHandler;
    ClickHandler menuLogoutHandler;
    ClickHandler menuQuitHandler;
    ClickHandler settingsBackHandler;
    ClickHandler settingsApplyHandler;
    ClickHandler settingsVideoTabHandler;
    ClickHandler settingsAudioTabHandler;
    ClickHandler settingsControlsTabHandler;
    ClickHandler inventoryCloseHandler;
    ClickHandler creationCreateHandler;
    ClickHandler creationCancelHandler;
    std::unordered_map<std::uint64_t, std::unique_ptr<ClickHandler>> lobbyCharacterHandlers;
    std::function<void(const std::string&, const std::string&, bool)> loginSubmitCallback;
    std::function<void(std::uint64_t)> lobbyEnterWorldCallback;
    std::function<void()> lobbyNewCharacterCallback;
    std::function<void(std::uint64_t)> lobbyDeleteCharacterCallback;
    std::function<void()> lobbyLogoutCallback;
    std::function<void()> menuResumeCallback;
    std::function<void()> menuLogoutCallback;
    std::function<void()> menuQuitCallback;
    std::vector<RmlCharacterListItem> lobbyCharacters;
    std::uint64_t selectedCharacterId = 0;
    bool loginVisible = true;
    bool lobbyVisible = false;
    bool hudVisible = false;
    bool menuVisible = false;
    bool settingsVisible = false;
    bool inventoryVisible = false;
    bool characterCreationVisible = false;
    bool initialized = false;
    uint32_t viewportWidth = 0;
    uint32_t viewportHeight = 0;
    uint64_t lastFrameDiagSecond = UINT64_MAX;
};

RmlUiLayer::RmlUiLayer() = default;
RmlUiLayer::~RmlUiLayer() { Destroy(); }

namespace
{
Rml::ElementFormControlInput* FindLoginInput(Rml::ElementDocument* document, const char* id)
{
    if (!document)
        return nullptr;

    return dynamic_cast<Rml::ElementFormControlInput*>(document->GetElementById(id));
}

std::string GetLoginInputValue(Rml::ElementDocument* document, const char* id)
{
    Rml::ElementFormControlInput* input = FindLoginInput(document, id);
    return input ? input->GetValue() : std::string{};
}

std::string EscapeRmlText(const std::string& text)
{
    std::string escaped;
    escaped.reserve(text.size());
    for (char c : text)
    {
        switch (c)
        {
        case '&': escaped += "&amp;"; break;
        case '<': escaped += "&lt;"; break;
        case '>': escaped += "&gt;"; break;
        default: escaped.push_back(c); break;
        }
    }
    return escaped;
}

bool GetLoginCheckboxValue(Rml::ElementDocument* document, const char* id)
{
    if (Rml::Element* element = document ? document->GetElementById(id) : nullptr)
        return element->GetAttribute<bool>("checked", false);
    return false;
}

void SetLoginStatusText(Rml::ElementDocument* document, const std::string& message)
{
    if (!document)
        return;

    Rml::Element* status = document->GetElementById("login-error");
    if (!status)
        return;

    status->SetInnerRML(EscapeRmlText(message));
    status->SetProperty("display", message.empty() ? "none" : "block");
}

std::string ClassLabel(std::uint16_t classId)
{
    return "Class " + std::to_string(classId);
}

std::string CharacterDetail(const RmlCharacterListItem& character)
{
    if (character.id == 0)
        return "Character creation not available yet";
    return "Level " + std::to_string(character.level) + "  " + ClassLabel(character.classId);
}

template <typename ImplT>
const RmlCharacterListItem* FindLobbyCharacter(const ImplT* impl, std::uint64_t id)
{
    if (!impl || id == 0)
        return nullptr;

    for (const RmlCharacterListItem& character : impl->lobbyCharacters)
    {
        if (character.id == id)
            return &character;
    }
    return nullptr;
}

void SetElementText(Rml::ElementDocument* document, const char* id, const std::string& text)
{
    if (Rml::Element* element = document ? document->GetElementById(id) : nullptr)
        element->SetInnerRML(EscapeRmlText(text));
}

void SetElementRml(Rml::ElementDocument* document, const char* id, const std::string& rml)
{
    if (Rml::Element* element = document ? document->GetElementById(id) : nullptr)
        element->SetInnerRML(rml);
}

void SetButtonDisabled(Rml::ElementDocument* document, const char* id, bool disabled)
{
    Rml::Element* button = document ? document->GetElementById(id) : nullptr;
    if (!button)
        return;

    if (disabled)
        button->SetAttribute("disabled", "disabled");
    else
        button->RemoveAttribute("disabled");
}

void SetElementDisplay(Rml::ElementDocument* document, const char* id, bool visible)
{
    if (Rml::Element* element = document ? document->GetElementById(id) : nullptr)
        element->SetProperty("display", visible ? "block" : "none");
}

template <typename ImplT>
void UpdateLobbySelectionPanel(ImplT* impl)
{
    if (!impl || !impl->lobbyDocument)
        return;

    const RmlCharacterListItem* selected = FindLobbyCharacter(impl, impl->selectedCharacterId);
    if (!selected)
    {
        SetElementText(impl->lobbyDocument, "selected-char-name", "No character selected");
        SetElementText(impl->lobbyDocument, "selected-char-meta", "");
        SetButtonDisabled(impl->lobbyDocument, "enter-world-btn", true);
        SetButtonDisabled(impl->lobbyDocument, "delete-char-btn", true);
        return;
    }

    SetElementText(impl->lobbyDocument, "selected-char-name", selected->name);
    SetElementRml(impl->lobbyDocument,
        "selected-char-meta",
        EscapeRmlText(CharacterDetail(*selected)) + "<br/>Slot " + std::to_string(selected->slot + 1));
    SetButtonDisabled(impl->lobbyDocument, "enter-world-btn", false);
    SetButtonDisabled(impl->lobbyDocument, "delete-char-btn", false);
}

template <typename ImplT>
void UpdateLobbySelectionClasses(ImplT* impl)
{
    if (!impl || !impl->lobbyDocument)
        return;

    Rml::ElementList items;
    impl->lobbyDocument->GetElementsByClassName(items, "character-item");
    for (Rml::Element* item : items)
    {
        const Rml::String idText = item->GetAttribute<Rml::String>("data-char-id", "");
        std::uint64_t id = 0;
        std::from_chars(idText.data(), idText.data() + idText.size(), id);
        item->SetClass("selected", id != 0 && id == impl->selectedCharacterId);
    }
}

template <typename ImplT>
void SetLobbyStatusText(ImplT* impl, const std::string& message)
{
    if (!impl || !impl->lobbyDocument)
        return;

    SetElementText(impl->lobbyDocument, "error-message", message);
    SetElementDisplay(impl->lobbyDocument, "error-message", !message.empty());
    if (!message.empty())
        Tracenf("[RMLUI-LOBBY] Error: %s", message.c_str());
}

float Percent(float current, float max)
{
    if (max <= 0.0f)
        return 0.0f;
    return std::clamp((current / max) * 100.0f, 0.0f, 100.0f);
}

std::string PercentWidth(float current, float max)
{
    return std::to_string(static_cast<int>(Percent(current, max))) + "%";
}

void SetText(Rml::Element* element, const std::string& text)
{
    if (element)
        element->SetInnerRML(EscapeRmlText(text));
}

void SetWidthPercent(Rml::Element* element, float current, float max)
{
    if (element)
        element->SetProperty("width", PercentWidth(current, max));
}

template <typename ImplT>
void CacheHudElements(ImplT& impl)
{
    Rml::ElementDocument* document = impl.hudDocument;
    if (!document)
        return;

    impl.hud.playerName = document->GetElementById("player-name");
    impl.hud.playerLevel = document->GetElementById("player-level");
    impl.hud.hpText = document->GetElementById("hp-text");
    impl.hud.hpFill = document->GetElementById("hp-fill");
    impl.hud.mpText = document->GetElementById("mp-text");
    impl.hud.mpFill = document->GetElementById("mp-fill");
    impl.hud.xpText = document->GetElementById("xp-text");
    impl.hud.xpFill = document->GetElementById("xp-fill");
    impl.hud.targetFrame = document->GetElementById("target-frame");
    impl.hud.targetName = document->GetElementById("target-name");
    impl.hud.targetLevel = document->GetElementById("target-level");
    impl.hud.targetHpFill = document->GetElementById("target-hp-fill");
    impl.hud.targetHpText = document->GetElementById("target-hp-text");
    impl.hud.zoneName = document->GetElementById("zone-name");
    impl.hud.playerCoords = document->GetElementById("player-coords");
}

void SetFullscreenDocumentSize(Rml::ElementDocument* document, uint32_t width, uint32_t height)
{
    if (!document)
        return;

    document->SetProperty("position", "absolute");
    document->SetProperty("left", "0px");
    document->SetProperty("top", "0px");
    document->SetProperty("width", std::to_string(width) + "px");
    document->SetProperty("height", std::to_string(height) + "px");
}

void SetSettingsTab(Rml::ElementDocument* document, const char* tab)
{
    if (!document)
        return;

    const bool video = std::strcmp(tab, "video") == 0;
    const bool audio = std::strcmp(tab, "audio") == 0;
    const bool controls = std::strcmp(tab, "controls") == 0;
    SetElementDisplay(document, "settings-video", video);
    SetElementDisplay(document, "settings-audio", audio);
    SetElementDisplay(document, "settings-controls", controls);

    if (Rml::Element* button = document->GetElementById("settings-tab-video"))
        button->SetClass("active", video);
    if (Rml::Element* button = document->GetElementById("settings-tab-audio"))
        button->SetClass("active", audio);
    if (Rml::Element* button = document->GetElementById("settings-tab-controls"))
        button->SetClass("active", controls);
}

void PopulateInventoryGrid(Rml::ElementDocument* document)
{
    if (!document)
        return;

    Rml::Element* grid = document->GetElementById("inventory-grid");
    if (!grid)
        return;

    std::string rml;
    for (int slot = 0; slot < 30; ++slot)
        rml += "<div class='slot'></div>";
    grid->SetInnerRML(rml);
}
}

bool RmlUiLayer::Create(ixrhi::IXRHIDevice& rhi, client::asset::IAssetReader& assets, uint32_t width, uint32_t height)
{
    m_impl = std::make_unique<Impl>();
    m_impl->assets = &assets;
    m_impl->viewportWidth = width;
    m_impl->viewportHeight = height;
    if (!m_impl->renderer.Create(rhi, assets, width, height))
        return false;

    m_impl->fileInterface = std::make_unique<RmlAssetFileInterface>(assets);
    Rml::SetRenderInterface(&m_impl->renderer);
    Rml::SetSystemInterface(&m_impl->system);
    Rml::SetFileInterface(m_impl->fileInterface.get());

    if (!Rml::Initialise())
    {
        Tracen("[RMLUI] Initialise() failed");
        return false;
    }

    Rml::LoadFontFace("assets/fonts/Roboto-Regular.ttf");
    Tracenf("[RMLUI-DIAG] CreateContext: viewport=%ux%u", width, height);
    m_impl->context = Rml::CreateContext("main", Rml::Vector2i(static_cast<int>(width), static_cast<int>(height)));
    if (!m_impl->context)
    {
        Tracen("[RMLUI] CreateContext failed");
        return false;
    }
    const Rml::Vector2i contextDimensions = m_impl->context->GetDimensions();
    Tracenf("[RMLUI-DIAG] Context dimensions after create: %dx%d",
        contextDimensions.x,
        contextDimensions.y);

    m_impl->loginButtonHandler.submit = [this]() {
        if (!m_impl || !m_impl->loginVisible)
            return;

        const std::string username = GetLoginInputValue(m_impl->loginDocument, "login-username");
        const std::string password = GetLoginInputValue(m_impl->loginDocument, "login-password");
        const bool remember = GetLoginCheckboxValue(m_impl->loginDocument, "login-remember");
        Tracenf("[RMLUI-LOGIN] attempt user='%s' remember=%s", username.c_str(), remember ? "true" : "false");

        if (username.empty() || password.empty())
        {
            SetLoginStatusText(m_impl->loginDocument, "Username and password required");
            return;
        }

        SetLoginStatusText(m_impl->loginDocument, "Connecting...");
        if (m_impl->loginSubmitCallback)
            m_impl->loginSubmitCallback(username, password, remember);
        else
            SetLoginStatusText(m_impl->loginDocument, "Network session is not ready");
    };

    m_impl->loginDocument = m_impl->context->LoadDocument("assets/ui/login.rml");
    if (!m_impl->loginDocument)
    {
        Tracen("[RMLUI] Optional runtime document missing: assets/ui/login.rml");
    }
    else
    {
        m_impl->loginDocument->SetProperty("position", "absolute");
        m_impl->loginDocument->SetProperty("left", "0px");
        m_impl->loginDocument->SetProperty("top", "0px");
        m_impl->loginDocument->SetProperty("width", std::to_string(width) + "px");
        m_impl->loginDocument->SetProperty("height", std::to_string(height) + "px");

        if (Rml::Element* button = m_impl->loginDocument->GetElementById("login-button"))
            button->AddEventListener("click", &m_impl->loginButtonHandler);
        SetLoginStatusText(m_impl->loginDocument, "");
        m_impl->loginDocument->Hide();
    }

    m_impl->lobbyEnterHandler.callback = [this](Rml::Event&) {
        if (!m_impl || !m_impl->lobbyVisible)
            return;
        if (m_impl->selectedCharacterId == 0)
        {
            SetLobbyStatusText(m_impl.get(), "Please select a character first");
            return;
        }
        Tracenf("[RMLUI-LOBBY] Enter world: char_id=%llu",
            static_cast<unsigned long long>(m_impl->selectedCharacterId));
        SetLobbyStatusText(m_impl.get(), "Entering world...");
        if (m_impl->lobbyEnterWorldCallback)
            m_impl->lobbyEnterWorldCallback(m_impl->selectedCharacterId);
    };
    m_impl->lobbyNewCharacterHandler.callback = [this](Rml::Event&) {
        if (!m_impl || !m_impl->lobbyVisible)
            return;
        Tracen("[RMLUI-LOBBY] New character");
        if (m_impl->lobbyNewCharacterCallback)
            m_impl->lobbyNewCharacterCallback();
        else
            SetLobbyStatusText(m_impl.get(), "Character creation not available yet");
    };
    m_impl->lobbyDeleteHandler.callback = [this](Rml::Event&) {
        if (!m_impl || !m_impl->lobbyVisible || m_impl->selectedCharacterId == 0)
            return;
        SetElementDisplay(m_impl->lobbyDocument, "delete-confirm", true);
    };
    m_impl->lobbyLogoutHandler.callback = [this](Rml::Event&) {
        if (!m_impl || !m_impl->lobbyVisible)
            return;
        Tracen("[RMLUI-LOBBY] Logout");
        if (m_impl->lobbyLogoutCallback)
            m_impl->lobbyLogoutCallback();
    };
    m_impl->lobbyConfirmDeleteYesHandler.callback = [this](Rml::Event&) {
        if (!m_impl || !m_impl->lobbyVisible || m_impl->selectedCharacterId == 0)
            return;
        const std::uint64_t id = m_impl->selectedCharacterId;
        Tracenf("[RMLUI-LOBBY] Delete character: char_id=%llu", static_cast<unsigned long long>(id));
        SetElementDisplay(m_impl->lobbyDocument, "delete-confirm", false);
        if (m_impl->lobbyDeleteCharacterCallback)
            m_impl->lobbyDeleteCharacterCallback(id);
        else
            SetLobbyStatusText(m_impl.get(), "Character delete is not available yet");
    };
    m_impl->lobbyConfirmDeleteNoHandler.callback = [this](Rml::Event&) {
        if (m_impl)
            SetElementDisplay(m_impl->lobbyDocument, "delete-confirm", false);
    };
    m_impl->menuResumeHandler.callback = [this](Rml::Event&) {
        HideInGameMenu();
        if (m_impl && m_impl->menuResumeCallback)
            m_impl->menuResumeCallback();
    };
    m_impl->menuSettingsHandler.callback = [this](Rml::Event&) {
        ShowSettings();
    };
    m_impl->menuLogoutHandler.callback = [this](Rml::Event&) {
        if (!m_impl)
            return;
        HideSettings();
        HideInventory();
        HideInGameMenu();
        if (m_impl->menuLogoutCallback)
            m_impl->menuLogoutCallback();
    };
    m_impl->menuQuitHandler.callback = [this](Rml::Event&) {
        if (m_impl && m_impl->menuQuitCallback)
            m_impl->menuQuitCallback();
    };
    m_impl->settingsBackHandler.callback = [this](Rml::Event&) {
        HideSettings();
    };
    m_impl->settingsApplyHandler.callback = [this](Rml::Event&) {
        if (!m_impl || !m_impl->settingsDocument)
            return;
        if (Rml::Element* status = m_impl->settingsDocument->GetElementById("settings-status"))
            status->SetInnerRML("Settings placeholder applied");
    };
    m_impl->settingsVideoTabHandler.callback = [this](Rml::Event&) {
        if (m_impl)
            SetSettingsTab(m_impl->settingsDocument, "video");
    };
    m_impl->settingsAudioTabHandler.callback = [this](Rml::Event&) {
        if (m_impl)
            SetSettingsTab(m_impl->settingsDocument, "audio");
    };
    m_impl->settingsControlsTabHandler.callback = [this](Rml::Event&) {
        if (m_impl)
            SetSettingsTab(m_impl->settingsDocument, "controls");
    };
    m_impl->inventoryCloseHandler.callback = [this](Rml::Event&) {
        HideInventory();
    };
    m_impl->creationCreateHandler.callback = [this](Rml::Event&) {
        if (!m_impl || !m_impl->characterCreationDocument)
            return;
        if (Rml::Element* status = m_impl->characterCreationDocument->GetElementById("creation-status"))
            status->SetInnerRML("Character creation backend is not wired yet");
    };
    m_impl->creationCancelHandler.callback = [this](Rml::Event&) {
        HideCharacterCreation();
        ShowLobby();
    };

    m_impl->lobbyDocument = m_impl->context->LoadDocument("assets/ui/lobby.rml");
    if (!m_impl->lobbyDocument)
    {
        Tracen("[RMLUI] Optional runtime document missing: assets/ui/lobby.rml");
    }
    else
    {
        m_impl->lobbyDocument->SetProperty("position", "absolute");
        m_impl->lobbyDocument->SetProperty("left", "0px");
        m_impl->lobbyDocument->SetProperty("top", "0px");
        m_impl->lobbyDocument->SetProperty("width", std::to_string(width) + "px");
        m_impl->lobbyDocument->SetProperty("height", std::to_string(height) + "px");
        if (Rml::Element* button = m_impl->lobbyDocument->GetElementById("enter-world-btn"))
            button->AddEventListener("click", &m_impl->lobbyEnterHandler);
        if (Rml::Element* button = m_impl->lobbyDocument->GetElementById("new-character-btn"))
            button->AddEventListener("click", &m_impl->lobbyNewCharacterHandler);
        if (Rml::Element* button = m_impl->lobbyDocument->GetElementById("delete-char-btn"))
            button->AddEventListener("click", &m_impl->lobbyDeleteHandler);
        if (Rml::Element* button = m_impl->lobbyDocument->GetElementById("logout-btn"))
            button->AddEventListener("click", &m_impl->lobbyLogoutHandler);
        if (Rml::Element* button = m_impl->lobbyDocument->GetElementById("confirm-delete-yes"))
            button->AddEventListener("click", &m_impl->lobbyConfirmDeleteYesHandler);
        if (Rml::Element* button = m_impl->lobbyDocument->GetElementById("confirm-delete-no"))
            button->AddEventListener("click", &m_impl->lobbyConfirmDeleteNoHandler);
        SetElementDisplay(m_impl->lobbyDocument, "delete-confirm", false);
        SetLobbyStatusText(m_impl.get(), "");
        m_impl->lobbyDocument->Hide();
    }

    m_impl->hudDocument = m_impl->context->LoadDocument("assets/ui/worldhud.rml");
    if (!m_impl->hudDocument)
    {
        Tracen("[RMLUI] Optional runtime document missing: assets/ui/worldhud.rml");
    }
    else
    {
        m_impl->hudDocument->SetProperty("position", "absolute");
        m_impl->hudDocument->SetProperty("left", "0px");
        m_impl->hudDocument->SetProperty("top", "0px");
        m_impl->hudDocument->SetProperty("width", std::to_string(width) + "px");
        m_impl->hudDocument->SetProperty("height", std::to_string(height) + "px");
        CacheHudElements(*m_impl);
        m_impl->hudDocument->Hide();
    }

    m_impl->menuDocument = m_impl->context->LoadDocument("assets/ui/ingame_menu.rml");
    if (!m_impl->menuDocument)
    {
        Tracen("[RMLUI] Optional runtime document missing: assets/ui/ingame_menu.rml");
    }
    else
    {
        SetFullscreenDocumentSize(m_impl->menuDocument, width, height);
        if (Rml::Element* button = m_impl->menuDocument->GetElementById("menu-resume-btn"))
            button->AddEventListener("click", &m_impl->menuResumeHandler);
        if (Rml::Element* button = m_impl->menuDocument->GetElementById("menu-settings-btn"))
            button->AddEventListener("click", &m_impl->menuSettingsHandler);
        if (Rml::Element* button = m_impl->menuDocument->GetElementById("menu-logout-btn"))
            button->AddEventListener("click", &m_impl->menuLogoutHandler);
        if (Rml::Element* button = m_impl->menuDocument->GetElementById("menu-quit-btn"))
            button->AddEventListener("click", &m_impl->menuQuitHandler);
        m_impl->menuDocument->Hide();
    }

    m_impl->settingsDocument = m_impl->context->LoadDocument("assets/ui/settings.rml");
    if (!m_impl->settingsDocument)
    {
        Tracen("[RMLUI] Optional runtime document missing: assets/ui/settings.rml");
    }
    else
    {
        SetFullscreenDocumentSize(m_impl->settingsDocument, width, height);
        if (Rml::Element* button = m_impl->settingsDocument->GetElementById("settings-back-btn"))
            button->AddEventListener("click", &m_impl->settingsBackHandler);
        if (Rml::Element* button = m_impl->settingsDocument->GetElementById("settings-apply-btn"))
            button->AddEventListener("click", &m_impl->settingsApplyHandler);
        if (Rml::Element* button = m_impl->settingsDocument->GetElementById("settings-tab-video"))
            button->AddEventListener("click", &m_impl->settingsVideoTabHandler);
        if (Rml::Element* button = m_impl->settingsDocument->GetElementById("settings-tab-audio"))
            button->AddEventListener("click", &m_impl->settingsAudioTabHandler);
        if (Rml::Element* button = m_impl->settingsDocument->GetElementById("settings-tab-controls"))
            button->AddEventListener("click", &m_impl->settingsControlsTabHandler);
        SetSettingsTab(m_impl->settingsDocument, "video");
        m_impl->settingsDocument->Hide();
    }

    m_impl->inventoryDocument = m_impl->context->LoadDocument("assets/ui/inventory.rml");
    if (!m_impl->inventoryDocument)
    {
        Tracen("[RMLUI] Optional runtime document missing: assets/ui/inventory.rml");
    }
    else
    {
        SetFullscreenDocumentSize(m_impl->inventoryDocument, width, height);
        if (Rml::Element* button = m_impl->inventoryDocument->GetElementById("inventory-close-btn"))
            button->AddEventListener("click", &m_impl->inventoryCloseHandler);
        PopulateInventoryGrid(m_impl->inventoryDocument);
        m_impl->inventoryDocument->Hide();
    }

    m_impl->characterCreationDocument = m_impl->context->LoadDocument("assets/ui/character_creation.rml");
    if (!m_impl->characterCreationDocument)
    {
        Tracen("[RMLUI] Optional runtime document missing: assets/ui/character_creation.rml");
    }
    else
    {
        SetFullscreenDocumentSize(m_impl->characterCreationDocument, width, height);
        if (Rml::Element* button = m_impl->characterCreationDocument->GetElementById("creation-create-btn"))
            button->AddEventListener("click", &m_impl->creationCreateHandler);
        if (Rml::Element* button = m_impl->characterCreationDocument->GetElementById("creation-cancel-btn"))
            button->AddEventListener("click", &m_impl->creationCancelHandler);
        m_impl->characterCreationDocument->Hide();
    }

    m_impl->initialized = true;
    Tracenf("[RMLUI] Initialized: version 6.2, viewport=%ux%u", width, height);
    Tracenf("[RMLUI] Runtime documents loaded: login=%d lobby=%d hud=%d menu=%d settings=%d inventory=%d character=%d",
        m_impl->loginDocument ? 1 : 0,
        m_impl->lobbyDocument ? 1 : 0,
        m_impl->hudDocument ? 1 : 0,
        m_impl->menuDocument ? 1 : 0,
        m_impl->settingsDocument ? 1 : 0,
        m_impl->inventoryDocument ? 1 : 0,
        m_impl->characterCreationDocument ? 1 : 0);
    return true;
}

void RmlUiLayer::Update()
{
    if (m_impl && m_impl->context)
        m_impl->context->Update();
}

void RmlUiLayer::Render(ixrhi::IXRHICommandList& cmd, const ixrhi::IXRHIFrameInfo& frame)
{
    if (!m_impl || !m_impl->context || !frame.frameActive || frame.commandList == nullptr)
    {
#if defined(IXTREEME_DEBUG_LOGS)
        static uint32_t skippedLogs = 0;
        if (!QuietLogsForLodDiag() && skippedLogs < 3)
        {
            ++skippedLogs;
            TraceDiagf("[FRAME] rmlui_render called = no, reason=%s",
                !m_impl ? "no_impl" : !m_impl->context ? "no_context" : "inactive_frame");
        }
#endif
        return;
    }

#if defined(IXTREEME_DEBUG_LOGS)
    const uint32_t visibleDocs =
        (m_impl->loginVisible && m_impl->loginDocument ? 1u : 0u) +
        (m_impl->lobbyVisible && m_impl->lobbyDocument ? 1u : 0u) +
        (m_impl->hudVisible && m_impl->hudDocument ? 1u : 0u) +
        (m_impl->menuVisible && m_impl->menuDocument ? 1u : 0u) +
        (m_impl->settingsVisible && m_impl->settingsDocument ? 1u : 0u) +
        (m_impl->inventoryVisible && m_impl->inventoryDocument ? 1u : 0u) +
        (m_impl->characterCreationVisible && m_impl->characterCreationDocument ? 1u : 0u);
    const uint64_t frameNumber = frame.frameNumber;
    if (!QuietLogsForLodDiag() && (frameNumber < 3 || (frameNumber % 60u) == 0u))
    {
        TraceDiagf("[FRAME] rmlui_render called = yes, visible_docs = %u, login=%d lobby=%d hud=%d menu=%d settings=%d inventory=%d character=%d viewport=%ux%u",
            visibleDocs,
            m_impl->loginVisible ? 1 : 0,
            m_impl->lobbyVisible ? 1 : 0,
            m_impl->hudVisible ? 1 : 0,
            m_impl->menuVisible ? 1 : 0,
            m_impl->settingsVisible ? 1 : 0,
            m_impl->inventoryVisible ? 1 : 0,
            m_impl->characterCreationVisible ? 1 : 0,
            m_impl->viewportWidth,
            m_impl->viewportHeight);
    }
#endif

    m_impl->renderer.BeginFrame(cmd, frame);
    m_impl->context->Render();
    m_impl->renderer.EndFrame();
}

void RmlUiLayer::Resize(uint32_t width, uint32_t height)
{
    if (!m_impl)
        return;

    m_impl->viewportWidth = width;
    m_impl->viewportHeight = height;
    m_impl->renderer.Resize(width, height);
    if (m_impl->context)
        m_impl->context->SetDimensions(Rml::Vector2i(static_cast<int>(width), static_cast<int>(height)));
    if (m_impl->loginDocument)
    {
        m_impl->loginDocument->SetProperty("width", std::to_string(width) + "px");
        m_impl->loginDocument->SetProperty("height", std::to_string(height) + "px");
    }
    if (m_impl->lobbyDocument)
    {
        m_impl->lobbyDocument->SetProperty("width", std::to_string(width) + "px");
        m_impl->lobbyDocument->SetProperty("height", std::to_string(height) + "px");
    }
    if (m_impl->hudDocument)
    {
        m_impl->hudDocument->SetProperty("width", std::to_string(width) + "px");
        m_impl->hudDocument->SetProperty("height", std::to_string(height) + "px");
    }
    SetFullscreenDocumentSize(m_impl->menuDocument, width, height);
    SetFullscreenDocumentSize(m_impl->settingsDocument, width, height);
    SetFullscreenDocumentSize(m_impl->inventoryDocument, width, height);
    SetFullscreenDocumentSize(m_impl->characterCreationDocument, width, height);
}

void RmlUiLayer::SetTargetPass(const ixrhi::IXRHIRenderPass* pass)
{
    if (m_impl)
        m_impl->renderer.SetTargetPass(pass);
}

bool RmlUiLayer::RecreatePipeline(ixrhi::IXRHIDevice& rhi)
{
    if (!m_impl)
        return true;
    return m_impl->renderer.RecreatePipeline(rhi);
}

bool RmlUiLayer::OnInput(const InputEvent& event)
{
    if (!m_impl || !m_impl->context)
        return false;

    const bool loginVisible = m_impl->loginVisible && m_impl->loginDocument;
    const bool lobbyVisible = m_impl->lobbyVisible && m_impl->lobbyDocument;
    const bool menuVisible = m_impl->menuVisible && m_impl->menuDocument;
    const bool settingsVisible = m_impl->settingsVisible && m_impl->settingsDocument;
    const bool inventoryVisible = m_impl->inventoryVisible && m_impl->inventoryDocument;
    const bool creationVisible = m_impl->characterCreationVisible && m_impl->characterCreationDocument;
    const bool capturesInput = loginVisible || lobbyVisible || menuVisible || settingsVisible ||
        inventoryVisible || creationVisible;
    switch (event.type)
    {
    case InputEvent::MouseMove:
        m_impl->context->ProcessMouseMove(event.x, event.y, 0);
        return capturesInput;
    case InputEvent::MouseDown:
        m_impl->context->ProcessMouseButtonDown(ToRmlMouseButton(event.button), 0);
        return capturesInput;
    case InputEvent::MouseUp:
        m_impl->context->ProcessMouseButtonUp(ToRmlMouseButton(event.button), 0);
        return capturesInput;
    case InputEvent::MouseWheel:
        m_impl->context->ProcessMouseWheel(static_cast<float>(event.wheelDelta) / 120.0f, 0);
        return capturesInput;
    case InputEvent::KeyDown:
        if (loginVisible && event.key == Key_Enter)
        {
            if (m_impl->loginButtonHandler.submit)
                m_impl->loginButtonHandler.submit();
            return true;
        }
        m_impl->context->ProcessKeyDown(ToRmlKey(event.key), 0);
        return capturesInput;
    case InputEvent::KeyUp:
        m_impl->context->ProcessKeyUp(ToRmlKey(event.key), 0);
        return capturesInput;
    case InputEvent::Char:
        m_impl->context->ProcessTextInput(CodepointToUtf8(event.codepoint));
        return capturesInput;
    default:
        return false;
    }
}

void RmlUiLayer::HideAll()
{
    Tracenf("[RMLUI] HideAll called: docs login=%d lobby=%d hud=%d menu=%d settings=%d inventory=%d character=%d viewport=%ux%u",
        m_impl && m_impl->loginDocument ? 1 : 0,
        m_impl && m_impl->lobbyDocument ? 1 : 0,
        m_impl && m_impl->hudDocument ? 1 : 0,
        m_impl && m_impl->menuDocument ? 1 : 0,
        m_impl && m_impl->settingsDocument ? 1 : 0,
        m_impl && m_impl->inventoryDocument ? 1 : 0,
        m_impl && m_impl->characterCreationDocument ? 1 : 0,
        m_impl ? m_impl->viewportWidth : 0u,
        m_impl ? m_impl->viewportHeight : 0u);
    HideLogin();
    HideLobby();
    HideHud();
    HideInventory();
    HideSettings();
    HideInGameMenu();
    HideCharacterCreation();
}

void RmlUiLayer::SetLoginSubmitCallback(std::function<void(const std::string&, const std::string&, bool)> callback)
{
    if (m_impl)
        m_impl->loginSubmitCallback = std::move(callback);
}

void RmlUiLayer::ShowLogin()
{
    if (!m_impl || !m_impl->loginDocument)
    {
        Tracenf("[RMLUI] ShowLogin called: document_loaded=%d visible=0 viewport=%ux%u",
            m_impl && m_impl->loginDocument ? 1 : 0,
            m_impl ? m_impl->viewportWidth : 0u,
            m_impl ? m_impl->viewportHeight : 0u);
        return;
    }

    m_impl->loginVisible = true;
    m_impl->lobbyVisible = false;
    m_impl->hudVisible = false;
    m_impl->menuVisible = false;
    m_impl->settingsVisible = false;
    m_impl->inventoryVisible = false;
    m_impl->characterCreationVisible = false;
    if (m_impl->lobbyDocument)
        m_impl->lobbyDocument->Hide();
    if (m_impl->hudDocument)
        m_impl->hudDocument->Hide();
    if (m_impl->menuDocument)
        m_impl->menuDocument->Hide();
    if (m_impl->settingsDocument)
        m_impl->settingsDocument->Hide();
    if (m_impl->inventoryDocument)
        m_impl->inventoryDocument->Hide();
    if (m_impl->characterCreationDocument)
        m_impl->characterCreationDocument->Hide();
    m_impl->loginDocument->Show();
    Tracenf("[RMLUI] ShowLogin called: document_loaded=1 visible=%d viewport=%ux%u",
        m_impl->loginVisible ? 1 : 0,
        m_impl->viewportWidth,
        m_impl->viewportHeight);
    if (Rml::Element* username = m_impl->loginDocument->GetElementById("login-username"))
        username->Focus(true);
}

void RmlUiLayer::HideLogin()
{
    if (!m_impl || !m_impl->loginDocument)
        return;

    m_impl->loginVisible = false;
    m_impl->loginDocument->Hide();
}

void RmlUiLayer::SetLoginError(const std::string& message)
{
    if (!m_impl || !m_impl->loginDocument)
        return;

    SetLoginStatusText(m_impl->loginDocument, message);
}

bool RmlUiLayer::IsLoginVisible() const
{
    return m_impl && m_impl->loginVisible;
}

void RmlUiLayer::SetLobbyCallbacks(std::function<void(std::uint64_t)> enterWorldCallback,
                                   std::function<void()> newCharacterCallback,
                                   std::function<void(std::uint64_t)> deleteCharacterCallback,
                                   std::function<void()> logoutCallback)
{
    if (!m_impl)
        return;

    m_impl->lobbyEnterWorldCallback = std::move(enterWorldCallback);
    m_impl->lobbyNewCharacterCallback = std::move(newCharacterCallback);
    m_impl->lobbyDeleteCharacterCallback = std::move(deleteCharacterCallback);
    m_impl->lobbyLogoutCallback = std::move(logoutCallback);
}

void RmlUiLayer::ShowLobby()
{
    if (!m_impl || !m_impl->lobbyDocument)
    {
        Tracenf("[RMLUI] ShowLobby called: document_loaded=%d visible=0 viewport=%ux%u",
            m_impl && m_impl->lobbyDocument ? 1 : 0,
            m_impl ? m_impl->viewportWidth : 0u,
            m_impl ? m_impl->viewportHeight : 0u);
        return;
    }

    m_impl->loginVisible = false;
    m_impl->hudVisible = false;
    m_impl->menuVisible = false;
    m_impl->settingsVisible = false;
    m_impl->inventoryVisible = false;
    m_impl->characterCreationVisible = false;
    if (m_impl->loginDocument)
        m_impl->loginDocument->Hide();
    if (m_impl->hudDocument)
        m_impl->hudDocument->Hide();
    if (m_impl->menuDocument)
        m_impl->menuDocument->Hide();
    if (m_impl->settingsDocument)
        m_impl->settingsDocument->Hide();
    if (m_impl->inventoryDocument)
        m_impl->inventoryDocument->Hide();
    if (m_impl->characterCreationDocument)
        m_impl->characterCreationDocument->Hide();
    m_impl->lobbyVisible = true;
    m_impl->lobbyDocument->Show();
    SetLobbyStatusText(m_impl.get(), "Fetching characters...");
    Tracenf("[RMLUI] ShowLobby called: document_loaded=1 visible=%d viewport=%ux%u",
        m_impl->lobbyVisible ? 1 : 0,
        m_impl->viewportWidth,
        m_impl->viewportHeight);
    Tracen("[RMLUI-LOBBY] Lobby shown");
}

void RmlUiLayer::HideLobby()
{
    if (!m_impl || !m_impl->lobbyDocument)
        return;

    m_impl->lobbyVisible = false;
    m_impl->lobbyDocument->Hide();
    SetElementDisplay(m_impl->lobbyDocument, "delete-confirm", false);
}

void RmlUiLayer::SetLobbyCharacters(const std::vector<RmlCharacterListItem>& characters)
{
    if (!m_impl || !m_impl->lobbyDocument)
        return;

    m_impl->lobbyCharacters = characters;
    m_impl->selectedCharacterId = 0;
    m_impl->lobbyCharacterHandlers.clear();

    Rml::Element* list = m_impl->lobbyDocument->GetElementById("character-list");
    if (!list)
        return;

    std::string rml;
    if (characters.empty())
        rml += "<div class='character-item-empty'>No characters yet. Create one to start!</div>";

    for (const RmlCharacterListItem& character : characters)
    {
        if (character.id == 0)
        {
            rml += "<div class='character-item character-item-disabled'>";
            rml += "<div class='char-name'>Empty slot</div>";
            rml += "<div class='char-meta'>Character creation not available yet</div>";
        }
        else
        {
            rml += "<div class='character-item' data-char-id='" + std::to_string(character.id) + "'>";
            rml += "<div class='char-name'>" + EscapeRmlText(character.name) + "</div>";
            rml += "<div class='char-meta'>" + EscapeRmlText(CharacterDetail(character)) + "</div>";
        }
        rml += "</div>";
    }

    for (uint32_t slot = static_cast<uint32_t>(characters.size()); slot < kLobbyPlayerSlots; ++slot)
    {
        rml += "<div class='character-item character-item-disabled'>";
        rml += "<div class='char-name'>Empty slot</div>";
        rml += "<div class='char-meta'>Character creation not available yet</div>";
        rml += "</div>";
    }

    list->SetInnerRML(rml);

    for (const RmlCharacterListItem& character : m_impl->lobbyCharacters)
    {
        if (character.id == 0)
            continue;

        Rml::ElementList items;
        m_impl->lobbyDocument->GetElementsByClassName(items, "character-item");
        for (Rml::Element* item : items)
        {
            const Rml::String idText = item->GetAttribute<Rml::String>("data-char-id", "");
            if (idText != std::to_string(character.id))
                continue;

            auto handler = std::make_unique<ClickHandler>();
            handler->callback = [this, id = character.id](Rml::Event&) {
                if (!m_impl || !m_impl->lobbyVisible)
                    return;

                m_impl->selectedCharacterId = id;
                UpdateLobbySelectionClasses(m_impl.get());
                UpdateLobbySelectionPanel(m_impl.get());
                SetLobbyStatusText(m_impl.get(), "");
                Tracenf("[RMLUI-LOBBY] Selected character: id=%llu",
                    static_cast<unsigned long long>(id));
            };
            item->AddEventListener("click", handler.get());
            m_impl->lobbyCharacterHandlers[character.id] = std::move(handler);
            break;
        }
    }

    UpdateLobbySelectionPanel(m_impl.get());
    SetLobbyStatusText(m_impl.get(), characters.empty() ? "No characters yet" : "Select a character");
    Tracenf("[RMLUI-LOBBY] Character list populated: %zu characters", characters.size());
}

void RmlUiLayer::SetLobbyStatus(const std::string& message)
{
    if (m_impl)
        SetLobbyStatusText(m_impl.get(), message);
}

bool RmlUiLayer::IsLobbyVisible() const
{
    return m_impl && m_impl->lobbyVisible;
}

void RmlUiLayer::ShowHud()
{
    if (!m_impl || !m_impl->hudDocument)
    {
        Tracenf("[RMLUI] ShowHud called: document_loaded=%d visible=0 viewport=%ux%u",
            m_impl && m_impl->hudDocument ? 1 : 0,
            m_impl ? m_impl->viewportWidth : 0u,
            m_impl ? m_impl->viewportHeight : 0u);
        return;
    }

    m_impl->loginVisible = false;
    m_impl->lobbyVisible = false;
    m_impl->menuVisible = false;
    m_impl->settingsVisible = false;
    m_impl->characterCreationVisible = false;
    m_impl->hudVisible = true;
    if (m_impl->loginDocument)
        m_impl->loginDocument->Hide();
    if (m_impl->lobbyDocument)
        m_impl->lobbyDocument->Hide();
    if (m_impl->menuDocument)
        m_impl->menuDocument->Hide();
    if (m_impl->settingsDocument)
        m_impl->settingsDocument->Hide();
    if (m_impl->characterCreationDocument)
        m_impl->characterCreationDocument->Hide();
    m_impl->hudDocument->Show();
    Tracenf("[RMLUI] ShowHud called: document_loaded=1 visible=%d viewport=%ux%u",
        m_impl->hudVisible ? 1 : 0,
        m_impl->viewportWidth,
        m_impl->viewportHeight);
    Tracen("[RMLUI-HUD] HUD shown");
}

void RmlUiLayer::HideHud()
{
    if (!m_impl || !m_impl->hudDocument)
        return;

    m_impl->hudVisible = false;
    m_impl->hudDocument->Hide();
    Tracen("[RMLUI-HUD] HUD hidden");
}

void RmlUiLayer::UpdateHud(const RmlHudData& data)
{
    if (!m_impl || !m_impl->hudVisible || !m_impl->hudDocument)
        return;

    SetText(m_impl->hud.playerName, data.playerName);
    SetText(m_impl->hud.playerLevel, "Lv. " + std::to_string(data.playerLevel));

    SetWidthPercent(m_impl->hud.hpFill, data.currentHp, data.maxHp);
    SetText(m_impl->hud.hpText,
        std::to_string(static_cast<int>(data.currentHp)) + " / " + std::to_string(static_cast<int>(data.maxHp)));

    SetWidthPercent(m_impl->hud.mpFill, data.currentMp, data.maxMp);
    SetText(m_impl->hud.mpText,
        std::to_string(static_cast<int>(data.currentMp)) + " / " + std::to_string(static_cast<int>(data.maxMp)));

    SetWidthPercent(m_impl->hud.xpFill,
        static_cast<float>(data.currentXp),
        static_cast<float>(data.xpForNextLevel));
    SetText(m_impl->hud.xpText, std::to_string(data.currentXp) + " / " + std::to_string(data.xpForNextLevel));

    if (m_impl->hud.targetFrame)
        m_impl->hud.targetFrame->SetProperty("display", data.hasTarget ? "block" : "none");
    if (data.hasTarget)
    {
        SetText(m_impl->hud.targetName, data.targetName);
        SetText(m_impl->hud.targetLevel, "Lv. " + std::to_string(data.targetLevel));
        SetWidthPercent(m_impl->hud.targetHpFill, data.targetCurrentHp, data.targetMaxHp);
        SetText(m_impl->hud.targetHpText,
            std::to_string(static_cast<int>(data.targetCurrentHp)) + " / " +
            std::to_string(static_cast<int>(data.targetMaxHp)));
    }

    SetText(m_impl->hud.zoneName, data.zoneName);
    SetText(m_impl->hud.playerCoords,
        "X: " + std::to_string(static_cast<int>(data.playerX)) +
        ", Z: " + std::to_string(static_cast<int>(data.playerZ)));
}

bool RmlUiLayer::IsHudVisible() const
{
    return m_impl && m_impl->hudVisible;
}

void RmlUiLayer::SetInGameMenuCallbacks(std::function<void()> resumeCallback,
                                        std::function<void()> logoutCallback,
                                        std::function<void()> quitCallback)
{
    if (!m_impl)
        return;

    m_impl->menuResumeCallback = std::move(resumeCallback);
    m_impl->menuLogoutCallback = std::move(logoutCallback);
    m_impl->menuQuitCallback = std::move(quitCallback);
}

void RmlUiLayer::ShowInGameMenu()
{
    if (!m_impl || !m_impl->menuDocument)
        return;

    m_impl->menuVisible = true;
    m_impl->menuDocument->Show();
}

void RmlUiLayer::HideInGameMenu()
{
    if (!m_impl || !m_impl->menuDocument)
        return;

    m_impl->menuVisible = false;
    m_impl->menuDocument->Hide();
}

void RmlUiLayer::ToggleInGameMenu()
{
    if (IsInGameMenuVisible())
        HideInGameMenu();
    else
        ShowInGameMenu();
}

bool RmlUiLayer::IsInGameMenuVisible() const
{
    return m_impl && m_impl->menuVisible;
}

void RmlUiLayer::ShowSettings()
{
    if (!m_impl || !m_impl->settingsDocument)
        return;

    m_impl->settingsVisible = true;
    m_impl->settingsDocument->Show();
}

void RmlUiLayer::HideSettings()
{
    if (!m_impl || !m_impl->settingsDocument)
        return;

    m_impl->settingsVisible = false;
    m_impl->settingsDocument->Hide();
}

bool RmlUiLayer::IsSettingsVisible() const
{
    return m_impl && m_impl->settingsVisible;
}

void RmlUiLayer::ShowInventory()
{
    if (!m_impl || !m_impl->inventoryDocument)
        return;

    m_impl->inventoryVisible = true;
    m_impl->inventoryDocument->Show();
}

void RmlUiLayer::HideInventory()
{
    if (!m_impl || !m_impl->inventoryDocument)
        return;

    m_impl->inventoryVisible = false;
    m_impl->inventoryDocument->Hide();
}

void RmlUiLayer::ToggleInventory()
{
    if (IsInventoryVisible())
        HideInventory();
    else
        ShowInventory();
}

bool RmlUiLayer::IsInventoryVisible() const
{
    return m_impl && m_impl->inventoryVisible;
}

void RmlUiLayer::ShowCharacterCreation()
{
    if (!m_impl || !m_impl->characterCreationDocument)
        return;

    m_impl->loginVisible = false;
    m_impl->lobbyVisible = false;
    m_impl->hudVisible = false;
    m_impl->menuVisible = false;
    m_impl->settingsVisible = false;
    m_impl->inventoryVisible = false;
    if (m_impl->loginDocument)
        m_impl->loginDocument->Hide();
    if (m_impl->lobbyDocument)
        m_impl->lobbyDocument->Hide();
    if (m_impl->hudDocument)
        m_impl->hudDocument->Hide();
    if (m_impl->menuDocument)
        m_impl->menuDocument->Hide();
    if (m_impl->settingsDocument)
        m_impl->settingsDocument->Hide();
    if (m_impl->inventoryDocument)
        m_impl->inventoryDocument->Hide();

    m_impl->characterCreationVisible = true;
    m_impl->characterCreationDocument->Show();
    if (Rml::Element* input = m_impl->characterCreationDocument->GetElementById("character-name"))
        input->Focus(true);
}

void RmlUiLayer::HideCharacterCreation()
{
    if (!m_impl || !m_impl->characterCreationDocument)
        return;

    m_impl->characterCreationVisible = false;
    m_impl->characterCreationDocument->Hide();
}

bool RmlUiLayer::IsCharacterCreationVisible() const
{
    return m_impl && m_impl->characterCreationVisible;
}

void RmlUiLayer::Destroy()
{
    if (!m_impl)
        return;

    if (m_impl->initialized)
        Rml::Shutdown();
    m_impl->renderer.Destroy();
    m_impl.reset();
}
