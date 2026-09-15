#pragma once

// IXRHICapabilities — portability baseline probe (Phase 2: populated from
// VkPhysicalDevice by IXVulkanDevice; renderers branch on this, never on #ifdef).
//
// Phase-1 fields are preserved (discreteGpu, supportsBindless, supportsRayQuery,
// maxAnisotropy). Added fields cover the core baseline + optional capabilities
// from the architecture brief. Exotic features (mesh shaders, ray tracing, VRS)
// are reported but MUST NOT be assumed: feature systems query before use.
//
// Future Apple path (IXRHI -> IXVulkan -> MoltenVK -> Metal) stays possible: the
// baseline below deliberately excludes anything MoltenVK cannot provide. See
// docs/architecture/ixrhi-portability.md.

#include <cstdint>
#include <string>

namespace ixrhi
{

struct IXRHICapabilities
{
    // Phase-1 fields (preserved).
    bool discreteGpu = false;
    bool supportsBindless = false;
    bool supportsRayQuery = false;
    std::uint32_t maxAnisotropy = 1;

    // Core portable baseline.
    std::uint32_t maxTextureSize = 1;
    bool supportsCompute = false;
    bool supportsIndirectDraw = false;
    bool supportsTimestampQueries = false;
    bool supportsAnisotropy = false;
    bool supportsDynamicRendering = false; // false on the classic render-pass backend
    bool supportsTimelineSemaphores = false;
    bool supportsMultiDrawIndirect = false;
    bool supportsAsyncCompute = false;

    // Explicitly non-baseline (never assumed).
    bool supportsMeshShaders = false;
    bool supportsRayTracing = false;
    bool supportsVariableRateShading = false;

    std::string deviceName;
};

} // namespace ixrhi
