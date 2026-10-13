#include "OcclusionCuller.h"
#include "Debug.h"
#include "IXRHIShader.h"
#include "asset/IAssetReader.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

OcclusionCuller* OcclusionCuller::s_active = nullptr;
namespace
{
std::string Environment(const char* name)
{
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t count = 0;
    _dupenv_s(&value, &count, name);
    std::string result = value ? value : "";
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value ? value : "";
#endif
}
}

bool OcclusionCuller::Create(ixrhi::IXRHIDevice& rhi, client::asset::IAssetReader& assets)
{
    Destroy();
    const auto& caps = rhi.GetCapabilities();
    if (Environment("IX_OCCLUSION_CULLING") == "0" || !caps.supportsCompute ||
        !caps.supportsIndirectDraw || !caps.supportsIndirectFirstInstance)
        return false;
    m_rhi = &rhi;
    m_diagnostics = Environment("IX_OCCLUSION_STATS") == "1";
    m_adaptive = Environment("IX_OCCLUSION_ADAPTIVE") != "0";
    using B = ixrhi::IXRHIBindingType;
    const auto stage = ixrhi::IXRHIShaderStage::Compute;
    m_layout = rhi.CreateBindGroupLayout({{0, B::SampledTexture, stage}, {1, B::StorageBuffer, stage},
        {2, B::StorageBuffer, stage}, {3, B::StorageBuffer, stage}, {4, B::StorageBuffer, stage},
        {5, B::StorageBuffer, stage}});
    if (m_layout) m_bindings = rhi.CreateBindGroup(*m_layout, 4);
    ixrhi::IXRHISamplerDesc sampler;
    sampler.debugName = "Occlusion:DepthSampler";
    m_sampler = rhi.CreateSampler(sampler);
    const char* files[] = {"base", "reduce", "test", "command"};
    const char* entries[] = {"BaseCS", "ReduceCS", "TestCS", "CommandCS"};
    bool ready = m_bindings && m_sampler;
    for (unsigned i = 0; i < 4 && ready; ++i)
    {
        const std::string path = std::string("assets/shaders/occlusion_") + files[i] + "_cs.spv";
        auto bytes = assets.ReadAll(path);
        if (!bytes || bytes->empty() || bytes->size() % 4) { ready = false; break; }
        ixrhi::IXRHIShaderDesc shader;
        shader.stage = stage; shader.entryPoint = entries[i]; shader.debugName = path;
        shader.spirv.resize(bytes->size() / 4);
        std::memcpy(shader.spirv.data(), bytes->data(), bytes->size());
        m_shaders[i] = rhi.CreateShader(shader);
        if (!m_shaders[i]) { ready = false; break; }
        ixrhi::IXRHIComputePipelineDesc pipeline;
        pipeline.computeShader = m_shaders[i]; pipeline.bindGroupLayouts = {m_layout.get()};
        pipeline.pushRanges = {{stage, 0, sizeof(Push)}}; pipeline.debugName = path;
        m_pipelines[i] = rhi.CreateComputePipeline(pipeline);
        ready = m_pipelines[i] != nullptr;
    }
    if (!ready) { Destroy(); Tracen("[OCCLUSION] unavailable; keeping direct draws"); return false; }
    m_bounds.reserve(kGroups); m_seeds.reserve(kCommands + 1);
    m_ready = true;
    Tracen("[OCCLUSION] same-frame GPU Hi-Z enabled (IX_OCCLUSION_CULLING=0 disables)");
    return true;
}

std::uint32_t OcclusionCuller::PyramidElements(std::uint32_t width, std::uint32_t height)
{
    if (!width || !height) return 0;
    std::uint64_t count = 0;
    do
    {
        count += static_cast<std::uint64_t>(width) * height;
        if (count > std::numeric_limits<std::uint32_t>::max()) return 0;
        if (width == 1 && height == 1) break;
        width = width / 2 + width % 2; height = height / 2 + height % 2;
    } while (true);
    return count <= std::numeric_limits<std::uint32_t>::max() ? static_cast<std::uint32_t>(count) : 0;
}

bool OcclusionCuller::Ensure(Slot& slot, std::uint32_t width, std::uint32_t height)
{
    if (slot.width == width && slot.height == height && slot.indirect) return true;
    // BeginFrame already fenced this exact frame slot. No resource here is used
    // by the other slot/view, so resize/release needs no additional device wait.
    slot = {};
    const auto elements = PyramidElements((width + 15) / 16, (height + 15) / 16);
    if (!elements) return false;
    using U = ixrhi::IXRHIBufferUsage;
    const auto make = [&](std::uint64_t bytes, U usage, bool host, const char* name) {
        ixrhi::IXRHIBufferDesc desc;
        desc.sizeBytes = bytes; desc.usage = usage;
        desc.cpuAccess = host ? ixrhi::IXRHICpuAccess::Write : ixrhi::IXRHICpuAccess::None;
        desc.debugName = name;
        return m_rhi->CreateBuffer(desc, nullptr, 0);
    };
    slot.pyramid = make(elements * 4ull, U::Storage, false, "Occlusion:Pyramid");
    slot.bounds = make(kGroups * sizeof(GpuBounds), U::Storage, true, "Occlusion:Bounds");
    slot.seeds = make((kCommands + 1ull) * sizeof(Seed), U::Storage, true, "Occlusion:Seeds");
    slot.visible = make(kGroups * 4ull, U::Storage | U::TransferSrc, false, "Occlusion:Visible");
    slot.indirect = make(kCommands * 20ull, U::Storage | U::Indirect | U::TransferSrc, false, "Occlusion:Commands");
    slot.readback = make(kGroups * 4ull, U::TransferDst, true, "Occlusion:Stats");
    if (!slot.pyramid || !slot.bounds || !slot.seeds || !slot.visible || !slot.indirect ||
        !slot.readback) { slot = {}; return false; }
    slot.width = width; slot.height = height;
    return true;
}

void OcclusionCuller::BeginFrame(const ixrhi::IXRHIFrameInfo& frame)
{
    if (!m_ready || !frame.frameActive || frame.frameIndex >= 2) return;
    for (unsigned view = 0; view < 2; ++view)
    {
        Slot& slot = m_slots[view * 2 + frame.frameIndex];
        if (slot.pending && slot.readback)
        {
            std::vector<std::uint32_t> flags(slot.instanceCounts.size());
            slot.readback->Read(0, flags.data(), flags.size() * sizeof(std::uint32_t));
            Stats stats{};
            stats.groups = static_cast<std::uint32_t>(flags.size());
            for (std::size_t i = 0; i < flags.size(); ++i)
            {
                stats.instances += slot.instanceCounts[i];
                stats.indexWork += slot.indexWork[i];
                if (!flags[i])
                {
                    ++stats.culledGroups; stats.culledInstances += slot.instanceCounts[i];
                    stats.culledIndexWork += slot.indexWork[i];
                }
            }
            m_stats[view] = stats;
            // Feedback changes only whether the optimizer is worth running. It NEVER
            // hides geometry using old visibility. Low-benefit views periodically probe.
            m_useful[view] = stats.culledIndexWork >= 50000 && stats.culledIndexWork >= stats.indexWork / 4;
            if (m_diagnostics && frame.frameNumber % 256 < 2)
                Tracenf("[OCCLUSION] view=%u groups=%u culled=%u instances=%u culledInstances=%u",
                    view, stats.groups, stats.culledGroups, stats.instances, stats.culledInstances);
            slot.pending = false;
        }
    }
}

bool OcclusionCuller::WantsView(std::uint32_t view, std::uint64_t frameNumber)
{
    if (!m_ready || view >= 2) return false;
    if (!m_adaptive || m_useful[view]) return true;
    if (frameNumber < m_nextProbe[view]) return false;
    m_nextProbe[view] = frameNumber + 120;
    return true;
}

bool OcclusionCuller::BeginView(ixrhi::IXRHICommandList& cmd, const ixrhi::IXRHIFrameInfo& frame,
    const WorldCamera& camera, std::shared_ptr<ixrhi::IXRHITexture> depth, std::uint32_t view)
{
    if (!m_ready || !frame.frameActive || frame.frameIndex >= 2 || view >= 2 || !depth || s_active) return false;
    const auto set = view * 2 + frame.frameIndex;
    Slot& slot = m_slots[set];
    if (!Ensure(slot, depth->Width(), depth->Height())) return false;
    m_bindings->UpdateTexture(set, 0, depth, m_sampler);
    m_bindings->UpdateBuffer(set, 1, slot.pyramid, 0, slot.pyramid->SizeBytes());
    m_bindings->UpdateBuffer(set, 2, slot.bounds, 0, slot.bounds->SizeBytes());
    m_bindings->UpdateBuffer(set, 3, slot.seeds, 0, slot.seeds->SizeBytes());
    m_bindings->UpdateBuffer(set, 4, slot.visible, 0, slot.visible->SizeBytes());
    m_bindings->UpdateBuffer(set, 5, slot.indirect, 0, slot.indirect->SizeBytes());
    using S = ixrhi::IXRHIBufferState;
    cmd.TransitionBuffer(*slot.pyramid, slot.used ? S::ShaderRead : S::Undefined, S::ShaderWrite);
    cmd.TransitionBuffer(*slot.visible, slot.used ? S::TransferSrc : S::Undefined, S::ShaderWrite);
    cmd.TransitionBuffer(*slot.indirect, slot.used ? S::IndirectRead : S::Undefined, S::ShaderWrite);
    Push push{};
    std::memcpy(push.vp, camera.viewProjection.m, sizeof(push.vp));
    push.width = slot.width; push.height = slot.height;
    push.baseWidth = (slot.width + 15) / 16; push.baseHeight = (slot.height + 15) / 16;
    const auto dispatch = [&](unsigned pipeline, std::uint32_t x, std::uint32_t y) {
        cmd.SetComputePipeline(*m_pipelines[pipeline]); cmd.BindGroup(0, *m_bindings, set);
        cmd.PushConstants(&push, sizeof(push)); cmd.Dispatch(x, y, 1);
    };
    dispatch(0, push.baseWidth, push.baseHeight);
    push.inWidth = push.baseWidth; push.inHeight = push.baseHeight;
    while (push.inWidth > 1 || push.inHeight > 1)
    {
        cmd.TransitionBuffer(*slot.pyramid, S::ShaderWrite, S::ShaderWrite);
        push.outOffset = push.inOffset + push.inWidth * push.inHeight;
        dispatch(1, ((push.inWidth + 1) / 2 + 7) / 8, ((push.inHeight + 1) / 2 + 7) / 8);
        push.inOffset = push.outOffset;
        push.inWidth = (push.inWidth + 1) / 2; push.inHeight = (push.inHeight + 1) / 2;
    }
    cmd.TransitionBuffer(*slot.pyramid, S::ShaderWrite, S::ShaderRead);
    // Counts/seeds are host written later during command recording, before queue
    // submission. Fixed dispatch capacities + the uploaded header bound GPU work.
    dispatch(2, (kGroups + 63) / 64, 1);
    cmd.TransitionBuffer(*slot.visible, S::ShaderWrite, S::ShaderRead);
    dispatch(3, (kCommands + 63) / 64, 1);
    cmd.TransitionBuffer(*slot.indirect, S::ShaderWrite, S::IndirectRead);
    slot.used = true; slot.instanceCounts.clear(); slot.indexWork.clear();
    m_bounds.clear(); m_seeds.clear(); m_seeds.push_back({});
    m_slot = &slot; m_group = kInvalidGroup; s_active = this;
    return true;
}

std::uint32_t OcclusionCuller::AddGroup(Bounds box, std::uint32_t instances)
{
    if (!m_slot || m_bounds.size() >= kGroups) return kInvalidGroup;
    const auto id = static_cast<std::uint32_t>(m_bounds.size());
    m_bounds.push_back({{box.min.x, box.min.y, box.min.z, 0}, {box.max.x, box.max.y, box.max.z, 0}});
    m_slot->instanceCounts.push_back(instances);
    m_slot->indexWork.push_back(0);
    return id;
}

bool OcclusionCuller::Draw(ixrhi::IXRHICommandList& cmd, std::uint32_t indices, std::uint32_t instances,
    std::uint32_t firstIndex, std::int32_t vertexOffset, std::uint32_t firstInstance)
{
    if (!m_slot || m_group >= m_bounds.size() || m_seeds.size() > kCommands) return false;
    const auto command = m_seeds.size() - 1;
    m_seeds.push_back({indices, instances, firstIndex, vertexOffset, firstInstance, m_group});
    m_slot->indexWork[m_group] += static_cast<std::uint64_t>(indices) * instances;
    cmd.DrawIndexedIndirect(*m_slot->indirect, command * 20ull);
    return true;
}

void OcclusionCuller::EndView(ixrhi::IXRHICommandList& cmd)
{
    if (s_active != this || !m_slot) return;
    m_seeds[0].indices = static_cast<std::uint32_t>(m_bounds.size());
    m_seeds[0].instances = static_cast<std::uint32_t>(m_seeds.size() - 1);
    m_slot->bounds->Write(0, m_bounds.data(), m_bounds.size() * sizeof(GpuBounds));
    m_slot->seeds->Write(0, m_seeds.data(), m_seeds.size() * sizeof(Seed));
    if (!m_bounds.empty())
    {
        using S = ixrhi::IXRHIBufferState;
        cmd.TransitionBuffer(*m_slot->visible, S::ShaderRead, S::TransferSrc);
        cmd.TransitionBuffer(*m_slot->readback, S::HostRead, S::TransferDst);
        cmd.CopyBuffer(*m_slot->visible, *m_slot->readback, m_bounds.size() * 4ull);
        cmd.TransitionBuffer(*m_slot->readback, S::TransferDst, S::HostRead);
        m_slot->pending = true;
    }
    else
    {
        cmd.TransitionBuffer(*m_slot->visible, ixrhi::IXRHIBufferState::ShaderRead, ixrhi::IXRHIBufferState::TransferSrc);
        // Nothing was tested, so nothing reports back. Without this an earlier "useful" verdict
        // would keep the depth pyramid running every frame for a view that has no groups left.
        const auto view = static_cast<std::size_t>(m_slot - m_slots.data()) / 2;
        m_stats[view] = {};
        m_useful[view] = false;
        m_slot->pending = false;
    }
    m_slot = nullptr; m_group = kInvalidGroup; s_active = nullptr;
}

bool OcclusionCuller::Project(const WorldCamera& camera, Bounds box, std::uint32_t width,
    std::uint32_t height, Projection& out)
{
    if (!width || !height) return false;
    out = {static_cast<float>(width), static_cast<float>(height), 0, 0, 1};
    const float* m = camera.viewProjection.m;
    for (unsigned i = 0; i < 8; ++i)
    {
        const WorldVec3 p{i & 1 ? box.max.x : box.min.x, i & 2 ? box.max.y : box.min.y, i & 4 ? box.max.z : box.min.z};
        const float w = p.x * m[3] + p.y * m[7] + p.z * m[11] + m[15];
        if (!(w > 0.0001f)) return false;
        const float x = (p.x * m[0] + p.y * m[4] + p.z * m[8] + m[12]) / w;
        const float y = (p.x * m[1] + p.y * m[5] + p.z * m[9] + m[13]) / w;
        const float z = (p.x * m[2] + p.y * m[6] + p.z * m[10] + m[14]) / w;
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || std::abs(x) >= 1 || std::abs(y) >= 1 || z <= 0 || z >= 1)
            return false;
        out.minX = std::min(out.minX, (x * 0.5f + 0.5f) * width);
        out.minY = std::min(out.minY, (y * 0.5f + 0.5f) * height);
        out.maxX = std::max(out.maxX, (x * 0.5f + 0.5f) * width);
        out.maxY = std::max(out.maxY, (y * 0.5f + 0.5f) * height);
        out.nearestDepth = std::min(out.nearestDepth, z);
    }
    return box.min.x <= box.max.x && box.min.y <= box.max.y && box.min.z <= box.max.z;
}

void OcclusionCuller::Destroy()
{
    if (s_active == this) s_active = nullptr;
    m_slot = nullptr; m_ready = false;
    m_bindings.reset();
    for (auto& slot : m_slots) slot = {};
    for (auto& pipeline : m_pipelines) pipeline.reset();
    for (auto& shader : m_shaders) shader.reset();
    m_layout.reset(); m_sampler.reset(); m_rhi = nullptr;
    m_stats = {}; m_useful = {}; m_nextProbe = {}; m_bounds.clear(); m_seeds.clear();
}
