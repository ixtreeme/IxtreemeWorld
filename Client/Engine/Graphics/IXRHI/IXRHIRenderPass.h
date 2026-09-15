#pragma once

// IXRHIRenderPass — borrowed render-pass token (Phase 3A).
//
// While the frame loop still owns VkRenderPass objects (VulkanDevice /
// OffscreenSceneRenderer), a migrated renderer that must bake pipelines against
// a NON-default pass (offscreen scene pass) names it through this engine-level
// token instead of a VkRenderPass. The backend unwraps it at pipeline creation.
//
// Lifetime: borrowed — the pass owner (offscreen renderer / swapchain loop)
// outlives every pipeline baked against it, exactly like the pre-migration
// SetMainRenderPass(VkRenderPass) member. Renderers hold a non-owning pointer.
// Phase 3B (offscreen decoupling) will let the backend CREATE passes from
// attachment descs; this token then becomes the creation result.

#include "IXRHI.h"

namespace ixrhi
{

class IXRHIRenderPass
{
public:
    virtual ~IXRHIRenderPass() = default;
};

} // namespace ixrhi
