#pragma once

#include "IXRHIDevice.h"
#include "IXRHIFrame.h"
#include "WorldCamera.h"
#include <array>
#include <limits>
#include <vector>

namespace client::asset { class IAssetReader; }

// Same-frame Hi-Z culling. The caller draws occluders, ends the storing pass, then
// BeginView records compute BEFORE the rest of the scene. Groups/draw seeds are
// collected while recording that scene, and uploaded by EndView before submission.
// No visibility readback controls rendering: compute writes the indirect commands.
class OcclusionCuller
{
public:
    static constexpr std::uint32_t kGroups = 4096, kCommands = 32768;
    static constexpr std::uint32_t kInvalidGroup = std::numeric_limits<std::uint32_t>::max();
    struct Bounds { WorldVec3 min, max; };
    struct Projection { float minX, minY, maxX, maxY, nearestDepth; };
    struct Stats
    {
        std::uint32_t groups = 0, culledGroups = 0, instances = 0, culledInstances = 0;
        std::uint64_t indexWork = 0, culledIndexWork = 0;
    };

    bool Create(ixrhi::IXRHIDevice&, client::asset::IAssetReader&);
    void Destroy(); // only after device drain
    bool IsEnabled() const { return m_ready; }
    void BeginFrame(const ixrhi::IXRHIFrameInfo&); // consume only the slot already fenced by BeginFrame
    bool WantsView(std::uint32_t view, std::uint64_t frameNumber);
    bool BeginView(ixrhi::IXRHICommandList&, const ixrhi::IXRHIFrameInfo&, const WorldCamera&,
                   std::shared_ptr<ixrhi::IXRHITexture> depth, std::uint32_t view);
    void EndView(ixrhi::IXRHICommandList&); // outside render pass, before submission
    static OcclusionCuller* Active() { return s_active; }
    std::uint32_t AddGroup(Bounds, std::uint32_t instances);
    std::uint32_t Group() const { return m_group; }
    void SetGroup(std::uint32_t group) { m_group = group; }
    // false = caller issues its original direct draw (capacity/selection/capture fallback).
    bool Draw(ixrhi::IXRHICommandList&, std::uint32_t indices, std::uint32_t instances,
              std::uint32_t firstIndex, std::int32_t vertexOffset, std::uint32_t firstInstance);
    const Stats& GetStats(std::uint32_t view) const { return m_stats.at(view); }

    // Conservative viewport projection, also used to choose large opaque occluders.
    // Bounds touching/crossing a clipping plane are kept visible.
    static bool Project(const WorldCamera&, Bounds, std::uint32_t width, std::uint32_t height, Projection&);
    static std::uint32_t PyramidElements(std::uint32_t width, std::uint32_t height);

private:
    friend struct OcclusionCullerTestAccess;
    struct GpuBounds { float min[4], max[4]; };
    struct Seed { std::uint32_t indices, instances, firstIndex; std::int32_t vertexOffset;
                  std::uint32_t firstInstance, group, padding[2]{}; };
    struct Push { float vp[16]; std::uint32_t width, height, baseWidth, baseHeight;
                  std::uint32_t inWidth, inHeight, inOffset, outOffset; };
    struct Slot
    {
        std::uint32_t width = 0, height = 0;
        std::shared_ptr<ixrhi::IXRHIBuffer> pyramid, bounds, seeds, visible, indirect, readback;
        bool used = false, pending = false;
        std::vector<std::uint32_t> instanceCounts;
        std::vector<std::uint64_t> indexWork;
    };
    bool Ensure(Slot&, std::uint32_t width, std::uint32_t height);
    static OcclusionCuller* s_active;
    ixrhi::IXRHIDevice* m_rhi = nullptr;
    bool m_ready = false, m_diagnostics = false, m_adaptive = true;
    std::unique_ptr<ixrhi::IXRHIBindGroupLayout> m_layout;
    std::unique_ptr<ixrhi::IXRHIBindGroup> m_bindings;
    std::array<std::shared_ptr<ixrhi::IXRHIShader>, 4> m_shaders;
    std::array<std::unique_ptr<ixrhi::IXRHIComputePipeline>, 4> m_pipelines;
    std::shared_ptr<ixrhi::IXRHISampler> m_sampler;
    std::array<Slot, 4> m_slots; // two independent views, two fenced frame slots
    std::array<Stats, 2> m_stats{};
    std::array<bool, 2> m_useful{};
    std::array<std::uint64_t, 2> m_nextProbe{};
    Slot* m_slot = nullptr;
    std::uint32_t m_group = kInvalidGroup;
    std::vector<GpuBounds> m_bounds;
    std::vector<Seed> m_seeds;
};
