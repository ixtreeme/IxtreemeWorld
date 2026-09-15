#pragma once

// IXRHIFence / IXRHISemaphore — minimal GPU synchronization for explicit
// advanced use only (async uploads use their own fences internally). Normal
// frame pacing needs no manual semaphore wiring: acquire/submit/present sync
// lives inside the backend frame authority. No Vulkan pipeline barriers here:
// explicit transitions go through IXRHICommandList::TransitionTexture.

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
