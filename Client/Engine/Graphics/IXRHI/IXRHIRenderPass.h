#pragma once

// IXRHIRenderPass — borrowed render-pass token (Phase 3A, retained in 3C).
//
// Names a backend-owned pass for pipeline baking without exposing it: render
// targets expose theirs via GetPass(), the swapchain via the main pass. The
// backend unwraps the token at pipeline creation. Renderers hold a non-owning
// pointer valid for the owner's lifetime.

#include "IXRHI.h"

namespace ixrhi
{

class IXRHIRenderPass
{
public:
    virtual ~IXRHIRenderPass() = default;
};

} // namespace ixrhi
