#pragma once

// IXRHISwapchain — main-window swapchain abstraction (Phase 3C: real object,
// not just info).
//
// Acquire/present pacing lives in the IXRHIDevice frame lifecycle (BeginFrame/
// EndFrame), keeping one authoritative flow (§73). The swapchain object carries
// properties, the backbuffer format/extent contract, and the generation counter
// (§53) that detects stale references after recreation. It remains an object
// (not a global) so future detached windows stay possible (§90).

#include "IXRHI.h"
#include "IXRHITypes.h"

#include <cstdint>

namespace ixrhi
{

class IXRHISwapchain
{
public:
    virtual ~IXRHISwapchain() = default;

    virtual std::uint32_t Width() const = 0;
    virtual std::uint32_t Height() const = 0;
    virtual IXRHIFormat ColorFormat() const = 0;
    virtual IXRHIFormat DepthFormat() const = 0;
    virtual std::uint32_t ImageCount() const = 0;
    // Bumped on every swapchain (re)creation.
    virtual std::uint64_t Generation() const = 0;

    virtual bool RequestResize(std::uint32_t width, std::uint32_t height) = 0;
};

} // namespace ixrhi
