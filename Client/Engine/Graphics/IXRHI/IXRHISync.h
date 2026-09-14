#pragma once

// IXRHIFence / IXRHISemaphore — minimal GPU synchronization for the paths the
// current renderer actually needs (upload completion, frame pacing after the
// frame-loop migration). Per-frame acquire/submit/present fences stay inside
// VulkanDevice in Phase 2. No Vulkan pipeline barriers here: resource
// transitions are performed by the backend during texture upload; explicit
// barrier API arrives with the compute/offscreen migrations.

#include "IXRHI.h"

namespace ixrhi
{

class IXRHIFence
{
public:
    virtual ~IXRHIFence() = default;

    virtual void Wait() = 0;
    virtual void Reset() = 0;
};

class IXRHISemaphore
{
public:
    virtual ~IXRHISemaphore() = default;
};

} // namespace ixrhi
