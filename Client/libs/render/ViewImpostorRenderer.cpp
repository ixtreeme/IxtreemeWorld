#include "ViewImpostorRenderer.h"
#include "ViewImpostorPolicy.h"

#include "Debug.h"
#include "IXRHIBinding.h"
#include "IXRHIPipeline.h"
#include "IXRHIShader.h"
#include "OffscreenSceneRenderer.h"
#include "asset/IAssetReader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <vector>

namespace
{
thread_local ViewImpostorRenderer* active = nullptr;
using namespace ixrhi;
std::shared_ptr<IXRHIShader> Shader(IXRHIDevice& rhi, client::asset::IAssetReader& assets, const char* path,
                                    IXRHIShaderStage stage, const char* entry)
{
    auto bytes = assets.ReadAll(path);
    if (!bytes || bytes->empty() || bytes->size() % 4)
        return {};
    IXRHIShaderDesc desc;
    desc.stage = stage;
    desc.entryPoint = entry;
    desc.spirv.resize(bytes->size() / 4);
    std::memcpy(desc.spirv.data(), bytes->data(), bytes->size());
    desc.debugName = path;
    return rhi.CreateShader(desc);
}

// Padding allows the projected bound to change without reallocating the image every frame.
// Bucket dimensions as well: captures of a slowly rotating model generally fit the same allocation.
ViewImpostorRenderer::Rect CaptureRectangle(ViewImpostorRenderer::Rect pixels, std::uint32_t width,
                                            std::uint32_t height, ViewImpostorRenderer::Rect allocation = {})
{
    const auto requiredWidth = std::min(width, (pixels.width + 16 + 15) / 16 * 16);
    const auto requiredHeight = std::min(height, (pixels.height + 16 + 15) / 16 * 16);
    const auto w = allocation.width >= requiredWidth ? allocation.width : requiredWidth;
    const auto h = allocation.height >= requiredHeight ? allocation.height : requiredHeight;
    const auto x = std::min(pixels.x > 8 ? pixels.x - 8 : 0, width - w);
    const auto y = std::min(pixels.y > 8 ? pixels.y - 8 : 0, height - h);
    return {x, y, w, h};
}
} // namespace

struct ViewImpostorRenderer::Impl
{
    struct Key
    {
        const void* owner;
        std::uint64_t id;
        std::uint32_t view;
        bool operator==(const Key&) const = default;
    };
    struct Hash
    {
        std::size_t operator()(const Key& key) const
        {
            return reinterpret_cast<std::uintptr_t>(key.owner) ^ (key.id * 0x9e3779b97f4a7c15ull) ^ key.view;
        }
    };
    struct Entry
    {
        Rect rectangle;
        WorldCamera camera{};
        std::uint64_t sourceRevision = 0, lightingRevision = 0, cameraRevision = 0, lastSeen = 0;
        std::uint64_t resourceRevision = 0;
        double captured = -1;
        std::uint64_t bytes = 0, triangles = 0;
        std::shared_ptr<IXRHITexture> color, depth;
        std::unique_ptr<IXRHIRenderTarget> target;
        std::unique_ptr<IXRHIBindGroup> group;
    };
    struct Retired
    {
        std::unique_ptr<Entry> entry;
        std::uint64_t frame;
    };
    struct View
    {
        WorldCamera camera{};
        const IXRHITexture* target = nullptr;
        std::uint32_t width = 0, height = 0;
        double changed = 0;
        double lightingChanged = 0;
        std::uint64_t lightingRevision = 0;
        std::uint64_t revision = 0;
        std::uint64_t resourceRevision = 0;
    };
    Settings settings;
    ViewImpostorPolicy policy;
    std::uint64_t policyFrame = std::numeric_limits<std::uint64_t>::max();
    std::uint32_t policyView = 2;
    const char* lastPolicy = nullptr;
    bool forced = false, policyAllows = false;
    Stats stats;
    IXRHIDevice* rhi = nullptr;
    std::shared_ptr<IXRHIShader> vs, ps;
    std::shared_ptr<IXRHISampler> sampler;
    std::unique_ptr<IXRHIBindGroupLayout> layout;
    std::unique_ptr<IXRHIGraphicsPipeline> pipeline;
    const IXRHIRenderPass* pipelinePass = nullptr;
    std::unordered_map<Key, std::unique_ptr<Entry>, Hash> entries;
    std::vector<Retired> retired;
    std::array<View, 2> views{};
    OffscreenSceneRenderer* target = nullptr;
    IXRHICommandList* cmd = nullptr;
    IXRHIFrameInfo frame{};
    WorldCamera camera{};
    std::uint32_t view = 0, usedCaptures = 0;
    std::uint64_t lightingRevision = 0, lastFrame = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t allocated = 0;
    double now = 0;
    bool ready = false, capturing = false;
    Rect captureRect;

    void Retire(std::unordered_map<Key, std::unique_ptr<Entry>, Hash>::iterator it)
    {
        retired.push_back({std::move(it->second), frame.frameNumber});
        entries.erase(it);
    }
    std::unique_ptr<Entry> MakeEntry(Rect rectangle)
    {
        const std::uint64_t minimumBytes = static_cast<std::uint64_t>(rectangle.width) * rectangle.height * 12;
        // Do not free resources still referenced by an in-flight command buffer to fit the budget.
        if (minimumBytes > settings.memoryBytes || allocated > settings.memoryBytes - minimumBytes)
            return {};
        auto entry = std::make_unique<Entry>();
        entry->rectangle = rectangle;
        IXRHITextureDesc desc;
        desc.width = rectangle.width;
        desc.height = rectangle.height;
        desc.format = OffscreenSceneRenderer::kSceneColorFormat;
        desc.usage = IXRHITextureUsage::ColorAttachment | IXRHITextureUsage::Sampled;
        desc.debugName = "ViewImpostor.HDR";
        entry->color = rhi->CreateTexture(desc, nullptr, 0);
        desc.format = target->DepthFormat();
        desc.usage = IXRHITextureUsage::DepthStencilAttachment | IXRHITextureUsage::Sampled;
        desc.debugName = "ViewImpostor.Depth";
        entry->depth = rhi->CreateTexture(desc, nullptr, 0);
        if (!entry->color || !entry->depth)
            return {};
        const auto colorBytes = entry->color->AllocatedBytes(), depthBytes = entry->depth->AllocatedBytes();
        // Unsupported backends keep geometry rather than silently exceeding the image budget.
        if (!colorBytes || !depthBytes || colorBytes > settings.memoryBytes ||
            depthBytes > settings.memoryBytes - colorBytes)
            return {};
        const auto bytes = colorBytes + depthBytes;
        if (allocated > settings.memoryBytes - bytes)
            return {};
        IXRHIRenderTargetDesc targetDesc;
        targetDesc.color = entry->color;
        targetDesc.depth = entry->depth;
        targetDesc.clearColor[0] = targetDesc.clearColor[1] = targetDesc.clearColor[2] = targetDesc.clearColor[3] = 0;
        targetDesc.debugName = "ViewImpostor.Capture";
        entry->target = rhi->CreateRenderTarget(targetDesc);
        entry->group = rhi->CreateBindGroup(*layout, 1);
        if (!entry->target || !entry->group)
            return {};
        entry->group->UpdateTexture(0, 0, entry->color, sampler);
        entry->group->UpdateTexture(0, 1, entry->depth, sampler);
        entry->bytes = bytes;
        allocated += bytes;
        return entry;
    }
};

ViewImpostorRenderer::ViewImpostorRenderer() : m(std::make_unique<Impl>())
{
}
ViewImpostorRenderer::~ViewImpostorRenderer()
{
    EndView();
}

bool ViewImpostorRenderer::Create(ixrhi::IXRHIDevice& rhi, client::asset::IAssetReader& assets)
{
    Destroy();
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, "IX_SCENE_IMPOSTORS") == 0 && value)
    {
        m->settings.enabled = std::strcmp(value, "0") != 0;
        m->forced = std::strcmp(value, "2") == 0;
    }
    std::free(value);
#else
    if (const char* value = std::getenv("IX_SCENE_IMPOSTORS"))
    {
        m->settings.enabled = std::strcmp(value, "0") != 0;
        m->forced = std::strcmp(value, "2") == 0;
    }
#endif
    if (!m->settings.enabled)
        return false;
    m->rhi = &rhi;
    m->vs = Shader(rhi, assets, "assets/shaders/view_impostor_vs.spv", IXRHIShaderStage::Vertex, "VSMain");
    m->ps = Shader(rhi, assets, "assets/shaders/view_impostor_ps.spv", IXRHIShaderStage::Fragment, "PSMain");
    if (!m->vs || !m->ps)
        return false;
    m->layout = rhi.CreateBindGroupLayout({{0, IXRHIBindingType::SampledTexture, IXRHIShaderStage::Fragment},
                                           {1, IXRHIBindingType::SampledTexture, IXRHIShaderStage::Fragment}});
    IXRHISamplerDesc desc;
    desc.minFilter = desc.magFilter = IXRHISamplerFilter::Nearest;
    m->sampler = rhi.CreateSampler(desc);
    m->ready = m->layout && m->sampler;
    if (m->ready)
        Tracen("[VIEW-IMPOSTOR] available: adaptive objects/terrain/characters, HDR+depth, 64 MiB, static 10 Hz / "
               "animated 60 Hz");
    return m->ready;
}

void ViewImpostorRenderer::BeginFrame(const IXRHIFrameInfo& frame)
{
    m->frame = frame;
    if (m->lastFrame != frame.frameNumber)
    {
        EndView();
        m->lastFrame = frame.frameNumber;
        m->usedCaptures = 0;
        m->stats = {};
        std::erase_if(m->retired, [&](const Impl::Retired& old) {
            if (frame.frameNumber <= old.frame + 4)
                return false;
            m->allocated -= old.entry->bytes;
            return true;
        });
        for (auto it = m->entries.begin(); it != m->entries.end();)
        {
            if (frame.frameNumber > it->second->lastSeen + 120)
            {
                auto old = it++;
                m->Retire(old);
            }
            else
                ++it;
        }
    }
    m->stats.bytes = m->allocated;
}
void ViewImpostorRenderer::BeginView(OffscreenSceneRenderer& target, IXRHICommandList& cmd, const IXRHIFrameInfo& frame,
                                     const WorldCamera& camera, double seconds, std::uint32_t view,
                                     std::uint64_t revision)
{
    EndView();
    BeginFrame(frame);
    if (!m->ready || !frame.frameActive || !target.IsReady() || view >= m->views.size() || !std::isfinite(seconds) ||
        seconds < 0)
        return;
    // A resize already drains the device and replaces all scene pipelines. Rebuild ours against
    // the current pass too. Entries retain the old dimensions but their camera revision changes.
    if (!m->pipeline || m->pipelinePass != target.GetTargetPass())
    {
        // Scene and Game have compatible formats/passes. Keep a single pipeline; only construct
        // on first use. Recreate on explicit Destroy/Create during a format change.
        if (!m->pipeline)
        {
            IXRHIGraphicsPipelineDesc desc;
            desc.vertexShader = m->vs;
            desc.fragmentShader = m->ps;
            desc.bindGroupLayouts = {m->layout.get()};
            desc.pushRanges = {{IXRHIShaderStage::Vertex | IXRHIShaderStage::Fragment, 0, 112}};
            desc.depthTestEnable = desc.depthWriteEnable = true;
            desc.depthCompareOp = IXRHICompareOp::LessOrEqual;
            desc.colorFormats = {target.ColorFormat()};
            desc.depthFormat = target.DepthFormat();
            desc.blendAttachments = {{}};
            desc.targetRenderPass = target.GetTargetPass();
            desc.debugName = "ViewImpostor.Main";
            m->pipeline = m->rhi->CreateGraphicsPipeline(desc);
            if (!m->pipeline)
                return;
        }
        m->pipelinePass = target.GetTargetPass();
    }
    auto& previous = m->views[view];
    const bool resourcesChanged = !previous.revision || previous.width != target.Width() ||
                                  previous.height != target.Height() ||
                                  previous.target != target.GetColorTexture().get();
    const bool cameraChanged =
        resourcesChanged ||
        std::memcmp(previous.camera.viewProjection.m, camera.viewProjection.m, sizeof(camera.viewProjection.m)) ||
        std::memcmp(&previous.camera.eye, &camera.eye, sizeof(camera.eye));
    if (cameraChanged)
    {
        // Small rotations are eligible immediately. Translation, discontinuous camera changes and
        // resizing still discard the cadence measurement and wait for a stable view.
        const auto rotation = resourcesChanged ? std::optional<WorldMat4>{} : RotationMapping(previous.camera, camera);
        const Rect full{0, 0, target.Width(), target.Height()};
        const Rect centre{target.Width() / 2, target.Height() / 2, 1, 1};
        if (!rotation || !ReprojectionCovered(*rotation, full, centre, target.Width(), target.Height()))
            previous.changed = seconds;
        if (resourcesChanged)
            ++previous.resourceRevision;
        previous.camera = camera;
        previous.width = target.Width();
        previous.height = target.Height();
        previous.target = target.GetColorTexture().get();
        ++previous.revision;
    }
    m->target = &target;
    m->cmd = &cmd;
    m->camera = camera;
    m->view = view;
    m->now = seconds;
    m->lightingRevision = revision;
    m->stats.bytes = m->allocated;
    if (previous.lightingRevision != revision)
    {
        previous.lightingRevision = revision;
        previous.lightingChanged = seconds;
    }
    const bool stable = seconds - previous.changed >= m->settings.settleSeconds &&
                        seconds - previous.lightingChanged >= m->settings.settleSeconds;
    // One cadence sample per actual frame, even when Scene and Game both record a view.
    if (m->policyFrame != frame.frameNumber)
    {
        m->policyFrame = frame.frameNumber;
        m->policyAllows = m->forced || m->policy.Observe(seconds, stable && m->policyView == view);
        m->policyView = view;
        const char* name = m->forced ? "forced" : m->policy.Name();
        if (m->lastPolicy != name)
        {
            Tracenf("[VIEW-IMPOSTOR] policy=%s (0=off, 1=adaptive, 2=forced)", name);
            m->lastPolicy = name;
        }
    }
    // Avoid per-instance preparation during translation or a discontinuous view change.
    if (stable && m->policyAllows)
        active = this;
}

void ViewImpostorRenderer::EndView()
{
    if (active == this)
        active = nullptr;
    m->target = nullptr;
    m->cmd = nullptr;
}
void ViewImpostorRenderer::Destroy()
{
    EndView();
    m = std::make_unique<Impl>();
}
const ViewImpostorRenderer::Stats& ViewImpostorRenderer::GetStats() const
{
    return m->stats;
}
ViewImpostorRenderer* ViewImpostorRenderer::Active()
{
    return active;
}
bool ViewImpostorRenderer::IsCapturing() const
{
    return m->capturing;
}

bool ViewImpostorRenderer::ApplyCaptureViewport(ixrhi::IXRHICommandList& cmd)
{
    if (!active || !active->m->capturing)
        return false;
    const auto& state = *active->m;
    cmd.SetViewport(-static_cast<float>(state.captureRect.x), -static_cast<float>(state.captureRect.y),
                    static_cast<float>(state.target->Width()), static_cast<float>(state.target->Height()));
    cmd.SetScissor(0, 0, state.captureRect.width, state.captureRect.height);
    return true;
}

bool ViewImpostorRenderer::Fresh(double now, double captured, double interval)
{
    return std::isfinite(now) && std::isfinite(captured) && interval > 0 && std::isfinite(interval) && captured >= 0 &&
           now >= captured && now - captured < interval;
}

std::optional<WorldMat4> ViewImpostorRenderer::RotationMapping(const WorldCamera& captured, const WorldCamera& current)
{
    if (captured.eye.x != current.eye.x || captured.eye.y != current.eye.y || captured.eye.z != current.eye.z ||
        captured.nearPlane != current.nearPlane || captured.farPlane != current.farPlane ||
        !std::isfinite(current.nearPlane) || !std::isfinite(current.farPlane) || current.nearPlane <= 0 ||
        current.farPlane <= current.nearPlane)
        return {};
    const auto a = WorldSub(captured.target, captured.eye), b = WorldSub(current.target, current.eye);
    const double aa = double(a.x) * a.x + double(a.y) * a.y + double(a.z) * a.z;
    const double bb = double(b.x) * b.x + double(b.y) * b.y + double(b.z) * b.z;
    const double ab = double(a.x) * b.x + double(a.y) * b.y + double(a.z) * b.z;
    if (!std::isfinite(aa) || !std::isfinite(bb) || !std::isfinite(ab) || aa <= 0 || bb <= 0 ||
        ab / std::sqrt(aa * bb) < 0.9993908270190958) // cos(2 degrees), bounds view-dependent colour error.
        return {};

    // Work relative to the declared common eye. World-space float matrices slightly round their
    // translation row; cancel that rounding in XY/W rather than amplify it through inversion.
    // Preserve the actual Z at the eye so that the depth remapping retains the rendered clip range.
    double relative[2][4][4]{};
    unsigned index = 0;
    for (const auto* camera : {&captured, &current})
    {
        const double point[] = {camera->eye.x, camera->eye.y, camera->eye.z, 1};
        for (unsigned column = 0; column < 4; ++column)
        {
            double value = 0, magnitude = 0;
            for (unsigned row = 0; row < 4; ++row)
            {
                if (!std::isfinite(camera->viewProjection.m[row * 4 + column]))
                    return {};
                const double term = point[row] * camera->viewProjection.m[row * 4 + column];
                value += term;
                magnitude += std::abs(term);
                if (row < 3)
                    relative[index][row][column] = camera->viewProjection.m[row * 4 + column];
            }
            if (!std::isfinite(value) || (column != 2 && std::abs(value) > 1e-6 * std::max(1.0, magnitude)))
                return {};
            relative[index][3][column] = column == 2 ? value : 0;
        }
        ++index;
    }
    // Double precision prevents cancellation of the camera translation in inverse(current) * old.
    double augmented[4][8]{};
    for (unsigned row = 0; row < 4; ++row)
    {
        for (unsigned column = 0; column < 4; ++column)
        {
            augmented[row][column] = relative[1][row][column];
        }
        augmented[row][row + 4] = 1;
    }
    for (unsigned column = 0; column < 4; ++column)
    {
        unsigned pivot = column;
        for (unsigned row = column + 1; row < 4; ++row)
            if (std::abs(augmented[row][column]) > std::abs(augmented[pivot][column]))
                pivot = row;
        if (std::abs(augmented[pivot][column]) < 1e-12)
            return {};
        for (unsigned i = 0; i < 8; ++i)
            std::swap(augmented[pivot][i], augmented[column][i]);
        const double divisor = augmented[column][column];
        for (double& value : augmented[column])
            value /= divisor;
        for (unsigned row = 0; row < 4; ++row)
            if (row != column)
            {
                const double factor = augmented[row][column];
                for (unsigned i = 0; i < 8; ++i)
                    augmented[row][i] -= factor * augmented[column][i];
            }
    }
    WorldMat4 mapping{};
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned column = 0; column < 4; ++column)
        {
            double value = 0;
            for (unsigned k = 0; k < 4; ++k)
                value += augmented[row][k + 4] * relative[0][k][column];
            if (!std::isfinite(value) || std::abs(value) > 1e6)
                return {};
            mapping.m[row * 4 + column] = static_cast<float>(value);
        }
    // A common origin makes source XY/W independent of destination depth. Refuse a matrix that
    // contradicts that property (e.g. a wrong eye or an oblique/projectively translated camera).
    for (unsigned column : {0u, 1u, 3u})
    {
        if (std::abs(mapping.m[8 + column]) > 2e-6f)
            return {};
        mapping.m[8 + column] = 0;
    }
    return mapping;
}

bool ViewImpostorRenderer::ReprojectionCovered(const WorldMat4& mapping, Rect capture, Rect output, std::uint32_t width,
                                               std::uint32_t height)
{
    if (!width || !height || !capture.width || !capture.height || !output.width || !output.height ||
        std::uint64_t(capture.x) + capture.width > width || std::uint64_t(capture.y) + capture.height > height ||
        std::uint64_t(output.x) + output.width > width || std::uint64_t(output.y) + output.height > height)
        return false;
    for (float value : mapping.m)
        if (!std::isfinite(value))
            return false;
    for (unsigned corner = 0; corner < 4; ++corner)
    {
        const double x = (double(output.x) + (corner & 1 ? output.width : 0)) / width * 2 - 1;
        const double y = (double(output.y) + (corner & 2 ? output.height : 0)) / height * 2 - 1;
        const double sx = x * mapping.m[0] + y * mapping.m[4] + mapping.m[12];
        const double sy = x * mapping.m[1] + y * mapping.m[5] + mapping.m[13];
        const double w = x * mapping.m[3] + y * mapping.m[7] + mapping.m[15];
        if (w <= 1e-6)
            return false;
        const double px = (sx / w + 1) * 0.5 * width, py = (sy / w + 1) * 0.5 * height;
        if (px < capture.x + 0.5 || py < capture.y + 0.5 || px > capture.x + capture.width - 0.5 ||
            py > capture.y + capture.height - 0.5)
            return false;
        // Bound local magnification/minification, including FOV and aspect changes. The native
        // source pixels are not a high-resolution texture that can be zoomed arbitrarily.
        const double dxX = (mapping.m[0] * w - sx * mapping.m[3]) / (w * w);
        const double dxY = (mapping.m[1] * w - sy * mapping.m[3]) / (w * w) * height / width;
        const double dyX = (mapping.m[4] * w - sx * mapping.m[7]) / (w * w) * width / height;
        const double dyY = (mapping.m[5] * w - sy * mapping.m[7]) / (w * w);
        const double scaleX = std::sqrt(dxX * dxX + dxY * dxY), scaleY = std::sqrt(dyX * dyX + dyY * dyY);
        if (scaleX < .95 || scaleX > 1.05 || scaleY < .95 || scaleY > 1.05)
            return false;
    }
    // A homography with positive W maps the rectangle into the convex hull of its corners.
    return true;
}

std::optional<ViewImpostorRenderer::Rect> ViewImpostorRenderer::ProjectBounds(const WorldCamera& camera,
                                                                              WorldVec3 minimum, WorldVec3 maximum,
                                                                              std::uint32_t width, std::uint32_t height,
                                                                              float distance,
                                                                              std::uint32_t maxDimension)
{
    if (!width || !height || !maxDimension || !(distance >= 0) || !std::isfinite(distance))
        return {};
    float nearestSq = 0;
    const float lo[] = {minimum.x, minimum.y, minimum.z}, hi[] = {maximum.x, maximum.y, maximum.z};
    const float eye[] = {camera.eye.x, camera.eye.y, camera.eye.z};
    for (int axis = 0; axis < 3; ++axis)
    {
        if (!std::isfinite(lo[axis]) || !std::isfinite(hi[axis]) || !std::isfinite(eye[axis]) || lo[axis] > hi[axis])
            return {};
        const float delta = std::max({lo[axis] - eye[axis], eye[axis] - hi[axis], 0.0f});
        nearestSq += delta * delta;
    }
    if (nearestSq < distance * distance)
        return {};
    float x0 = std::numeric_limits<float>::max(), y0 = x0, x1 = -x0, y1 = -x0;
    for (int corner = 0; corner < 8; ++corner)
    {
        const float v[] = {corner & 1 ? hi[0] : lo[0], corner & 2 ? hi[1] : lo[1], corner & 4 ? hi[2] : lo[2], 1};
        float projected[4]{};
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row)
                projected[column] += v[row] * camera.viewProjection.m[row * 4 + column];
        for (float component : projected)
            if (!std::isfinite(component))
                return {};
        if (projected[3] <= 0 || projected[2] <= 0 || projected[2] >= projected[3])
            return {};
        const float x = (projected[0] / projected[3] + 1) * 0.5f * width;
        const float y = (projected[1] / projected[3] + 1) * 0.5f * height;
        x0 = std::min(x0, x);
        x1 = std::max(x1, x);
        y0 = std::min(y0, y);
        y1 = std::max(y1, y);
    }
    // Integer crop at native screen resolution, with a two-pixel raster guard. Reject boxes that
    // cross the screen edge: there is no complete capture for a subsequently revealed surface.
    x0 = std::floor(x0) - 2;
    y0 = std::floor(y0) - 2;
    x1 = std::ceil(x1) + 2;
    y1 = std::ceil(y1) + 2;
    if (x0 < 0 || y0 < 0 || x1 > width || y1 > height || x1 <= x0 || y1 <= y0 || x1 - x0 > maxDimension ||
        y1 - y0 > maxDimension)
        return {};
    return Rect{static_cast<std::uint32_t>(x0), static_cast<std::uint32_t>(y0), static_cast<std::uint32_t>(x1 - x0),
                static_cast<std::uint32_t>(y1 - y0)};
}

bool ViewImpostorRenderer::TryDraw(const void* owner, std::uint64_t id, std::uint64_t sourceRevision, Kind kind,
                                   WorldVec3 minimum, WorldVec3 maximum, std::uint64_t triangles,
                                   const std::function<std::uint64_t()>& callback)
{
    if (!m->target || m->capturing || !owner || !id || kind >= Kind::Count || triangles < 128)
        return false;
    const auto& view = m->views[m->view];
    if (m->now - view.changed < m->settings.settleSeconds)
        return false;
    auto rectangle = ProjectBounds(m->camera, minimum, maximum, m->target->Width(), m->target->Height(),
                                   kind == Kind::Character ? m->settings.characterDistance : m->settings.distance,
                                   m->settings.maxDimension);
    if (!rectangle)
        return false;
    return TryDrawPixels(owner, id, sourceRevision, kind, *rectangle, 1, callback);
}

bool ViewImpostorRenderer::TryDrawTerrainRegion(const void* owner, std::uint64_t id, std::uint64_t sourceRevision,
                                                Rect rectangle, std::uint64_t triangles, std::uint32_t chunks,
                                                const std::function<std::uint64_t()>& callback)
{
    if (!m->target || m->capturing || !owner || !id || !chunks || triangles < 128 || !rectangle.width ||
        !rectangle.height || rectangle.height > m->settings.maxDimension ||
        static_cast<std::uint64_t>(rectangle.x) + rectangle.width > m->target->Width() ||
        static_cast<std::uint64_t>(rectangle.y) + rectangle.height > m->target->Height() ||
        m->now - m->views[m->view].changed < m->settings.settleSeconds)
        return false;
    return TryDrawPixels(owner, id, sourceRevision, Kind::Terrain, rectangle, chunks, callback);
}

bool ViewImpostorRenderer::TryDrawPixels(const void* owner, std::uint64_t id, std::uint64_t sourceRevision, Kind kind,
                                         Rect pixels, std::uint32_t items,
                                         const std::function<std::uint64_t()>& callback)
{
    const auto& view = m->views[m->view];
    const Impl::Key key{owner, id, m->view};
    auto it = m->entries.find(key);
    const auto width = m->target->Width(), height = m->target->Height();
    if (it != m->entries.end() && it->second->resourceRevision != view.resourceRevision)
    {
        m->Retire(it);
        it = m->entries.end();
    }
    if (it != m->entries.end())
        it->second->lastSeen = m->frame.frameNumber;
    WorldMat4 mapping = WorldIdentity();
    bool reprojected = false;
    bool fresh = it != m->entries.end() && it->second->sourceRevision == sourceRevision &&
                 it->second->lightingRevision == m->lightingRevision &&
                 Fresh(m->now, it->second->captured,
                       kind == Kind::Character ? m->settings.refreshSeconds : m->settings.staticRefreshSeconds);
    if (fresh)
    {
        const auto& entry = *it->second;
        if (entry.cameraRevision != view.revision)
        {
            const auto rotation = RotationMapping(entry.camera, m->camera);
            fresh = rotation && ReprojectionCovered(*rotation, entry.rectangle, pixels, width, height);
            if (fresh)
            {
                mapping = *rotation;
                reprojected = true;
            }
        }
        else
            fresh = pixels.x >= entry.rectangle.x && pixels.y >= entry.rectangle.y &&
                    pixels.x + pixels.width <= entry.rectangle.x + entry.rectangle.width &&
                    pixels.y + pixels.height <= entry.rectangle.y + entry.rectangle.height;
    }
    if (!fresh)
    {
        // Starvation is a geometry fallback, never an unbounded stale image. Round-robin is implicit:
        // refreshed entries are fresh on the next frame, leaving the budget for subsequent entries.
        if (m->usedCaptures >= m->settings.capturesPerFrame)
        {
            ++m->stats.fallbacks;
            return false;
        }
        // Reuse the allocation when recapturing a slightly changed rectangle. Its screen origin
        // can change; its descriptor/image dimensions must stay fixed while commands are in flight.
        auto capture = CaptureRectangle(pixels, width, height, it == m->entries.end() ? Rect{} : it->second->rectangle);
        if (it != m->entries.end() &&
            (capture.width != it->second->rectangle.width || capture.height != it->second->rectangle.height))
        {
            m->Retire(it);
            it = m->entries.end();
        }
        if (it == m->entries.end())
        {
            if (m->entries.size() >= m->settings.entries)
            {
                ++m->stats.fallbacks;
                return false;
            }
            auto entry = m->MakeEntry(capture);
            if (!entry)
            {
                ++m->stats.fallbacks;
                return false;
            }
            it = m->entries.emplace(key, std::move(entry)).first;
        }
        auto& entry = *it->second;
        entry.rectangle = capture;
        m->target->EndMainPass(*m->cmd);
        if (entry.captured >= 0)
            m->cmd->TransitionTexture(*entry.depth, IXRHIImageLayout::ShaderReadOnly,
                                      IXRHIImageLayout::DepthStencilAttachment);
        entry.target->Begin(*m->cmd);
        m->captureRect = capture;
        m->capturing = true;
        ApplyCaptureViewport(*m->cmd);
        entry.triangles = callback();
        m->capturing = false;
        entry.target->End(*m->cmd);
        m->cmd->TransitionTexture(*entry.depth, IXRHIImageLayout::DepthStencilAttachment,
                                  IXRHIImageLayout::ShaderReadOnly);
        m->target->BeginMainPass(*m->cmd, m->frame, false);
        entry.captured = m->now;
        entry.sourceRevision = sourceRevision;
        entry.lightingRevision = m->lightingRevision;
        entry.cameraRevision = view.revision;
        entry.resourceRevision = view.resourceRevision;
        entry.camera = m->camera;
        entry.lastSeen = m->frame.frameNumber;
        ++m->usedCaptures;
        ++m->stats.captures[static_cast<std::uint32_t>(kind)];
    }
    const auto& entry = *it->second;
    struct Parameters
    {
        float rectangle[4];
        float capture[4];
        float extent[4];
        float columns[4][4];
    } params{{float(pixels.x), float(pixels.y), float(pixels.width), float(pixels.height)},
             {float(entry.rectangle.x), float(entry.rectangle.y), float(entry.rectangle.width),
              float(entry.rectangle.height)},
             {float(width), float(height), reprojected ? 1.0f : 0.0f, 0}};
    static_assert(sizeof(Parameters) == 112);
    for (unsigned column = 0; column < 4; ++column)
        for (unsigned row = 0; row < 4; ++row)
            params.columns[column][row] = mapping.m[row * 4 + column];
    m->cmd->SetViewport(0, 0, float(width), float(height));
    m->cmd->SetScissor(pixels.x, pixels.y, pixels.width, pixels.height);
    m->cmd->SetGraphicsPipeline(*m->pipeline);
    m->cmd->BindGroup(0, *entry.group, 0);
    m->cmd->PushConstants(&params, sizeof(params));
    m->cmd->Draw(6);
    m->stats.draws[static_cast<std::uint32_t>(kind)] += items;
    if (reprojected)
        m->stats.reprojected += items;
    m->policy.RecordUse();
    if (fresh)
        m->stats.savedSourceTriangles += entry.triangles;
    m->stats.bytes = m->allocated;
    return true;
}
