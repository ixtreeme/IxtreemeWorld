#pragma once

// IXRHIFrame — canonical graphics-frame contract (Phase 3C, §3/§44).
//
// There is exactly ONE frame representation: IXRHIFrameInfo (extended here to
// the full context — frame/result indices, borrowed command list, borrowed
// backbuffer, swapchain generation). No parallel VulkanDevice-side frame info.
//
// Lifetime (§50): the info and its borrowed resources (commandList, backBuffer)
// are valid only for the active frame — from successful BeginFrame until the
// matching EndFrame (or Shutdown). No shared_ptr lifetime extension; renderers
// must not retain them.
//
// Threading (§48): render-thread only. Primary command list; no parallel
// recording in this phase (a future RenderGraph/parallel extension builds on
// this contract without breaking it).

#include "IXRHI.h"

#include <cstdint>

namespace ixrhi
{

class IXRHICommandList;
class IXRHITexture;

enum class IXRHIFrameResult : std::uint8_t
{
    Success = 0, // active frame; must be closed with EndFrame
    Skip, // no frame (minimized/zero-size/unavailable); do NOT call EndFrame
    SwapchainRecreated, // backend rebuilt swapchain; run resize orchestration, no EndFrame
    DeviceLost, // fatal; report, do not continue rendering
};

struct IXRHIFrameInfo
{
    IXRHIFrameResult result = IXRHIFrameResult::Skip;
    std::uint32_t frameIndex = 0; // frames-in-flight slot, authoritative
    std::uint32_t imageIndex = 0; // swapchain image; NEVER assumed == frameIndex
    std::uint32_t targetWidth = 0;
    std::uint32_t targetHeight = 0;
    bool frameActive = false; // == (result == Success)
    // Monotonic frame counter from the loop (diagnostics only, not an index).
    std::uint64_t frameNumber = 0;
    // Borrowed recording command list (valid while frameActive only).
    IXRHICommandList* commandList = nullptr;
    // Borrowed backbuffer view (valid while frameActive only, and only for the
    // current swapchainGeneration). Owned by the swapchain; never destroy.
    const IXRHITexture* backBuffer = nullptr;
    // Bumped on every swapchain (re)creation; detects stale references.
    std::uint64_t swapchainGeneration = 0;
};

struct IXRHIFrame
{
    IXRHIFrameResult result = IXRHIFrameResult::Skip;
    IXRHIFrameInfo info{};
    // Debug generation token pairing Begin/End (checked in Debug builds).
    std::uint64_t frameToken = 0;

    explicit operator bool() const noexcept { return result == IXRHIFrameResult::Success; }
};

} // namespace ixrhi
