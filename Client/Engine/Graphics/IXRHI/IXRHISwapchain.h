#pragma once

// IXRHISwapchain — Phase-2 scope: info queries over the live swapchain plus a
// resize request. Acquire/present stay inside VulkanDevice's frame loop until the
// frame-loop migration (Phase 3B, with offscreen decoupling); they are therefore
// deliberately ABSENT from this interface rather than faked.

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

    virtual bool RequestResize(std::uint32_t width, std::uint32_t height) = 0;
};

} // namespace ixrhi
