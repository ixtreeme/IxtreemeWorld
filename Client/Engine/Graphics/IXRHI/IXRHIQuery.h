#pragma once

// IXRHI GPU timestamp queries (Phase 3C, §23-25/63-65).
//
// Minimal abstraction over the backend's timestamp pool, preserving the
// existing 23-point profiler exactly: same indices, same boundaries, results
// in nanoseconds (IXVulkan converts raw ticks via timestampPeriod — generic
// code never sees it). No occlusion/statistics queries (not currently needed).
// Full IXRHIQueryPool is a later phase; this covers timestamps only.

#include "IXRHI.h"

#include <cstdint>

namespace ixrhi
{

// Engine profiler points. Values are stable indices into the results arrays
// (mirrors the legacy GpuTimestampPoint ordering 1:1 for log parity).
enum class IXRHITimestampPoint : std::uint32_t
{
    FrameBegin = 0,
    ShadowPassBegin,
    ShadowCascade0Begin,
    ShadowCascade0End,
    ShadowCascade1Begin,
    ShadowCascade1End,
    ShadowCascade2Begin,
    ShadowCascade2End,
    ShadowCascade3Begin,
    ShadowCascade3End,
    ShadowPassEnd,
    WaterReflectionBegin,
    WaterReflectionEnd,
    TerrainMainBegin,
    TerrainMainEnd,
    SceneOtherBegin,
    SceneOtherEnd,
    CompositeBegin,
    CompositeEnd,
    RmlUiBegin,
    RmlUiEnd,
    ImGuiBegin,
    ImGuiEnd,
    FrameEnd,
};

constexpr std::uint32_t IXRHITimestampPointCount =
    static_cast<std::uint32_t>(IXRHITimestampPoint::FrameEnd) + 1u;

constexpr std::uint32_t IXRHI_MAX_TIMESTAMP_POINTS = 32;

struct IXRHITimestampResults
{
    bool valid = false;
    std::uint64_t frameNumber = 0;
    bool pointValid[IXRHI_MAX_TIMESTAMP_POINTS]{};
    std::uint64_t pointNanoseconds[IXRHI_MAX_TIMESTAMP_POINTS]{};
};

// Occlusion queries: whether any sample of the draws between Begin and End passed the depth test
// (the count is approximate: zero or not is what it tells). Results come back frames later;
// TryGetResult never waits for them.
class IXRHIOcclusionQueries
{
public:
    virtual ~IXRHIOcclusionQueries() = default;
    virtual std::uint32_t Count() const = 0;
    // Outside any render pass, before the queries are begun again.
    virtual void Reset(IXRHICommandList& cmd, std::uint32_t first, std::uint32_t count) = 0;
    // Inside a render pass; one query is begun once between resets.
    virtual void Begin(IXRHICommandList& cmd, std::uint32_t index) = 0;
    virtual void End(IXRHICommandList& cmd, std::uint32_t index) = 0;
    // The samples counted by query `index` once the GPU has finished it; false until then.
    virtual bool TryGetResult(std::uint32_t index, std::uint64_t& samples) = 0;
};

struct IXRHICpuFrameTiming
{
    bool valid = false;
    std::uint64_t frameNumber = 0;
    double acquireImageMs = 0.0;
    double waitForFencesMs = 0.0;
    double renderLoopCpuWorkMs = 0.0;
    double submitMs = 0.0;
    double presentMs = 0.0;
    double totalCpuFrameMs = 0.0;
};

} // namespace ixrhi
