#pragma once

// IXRHI — Ixtreeme Render Hardware Interface (Phase 2: production core).
//
// The Ixtreeme renderer depends on this engine graphics contract; Vulkan is one
// implementation behind it (Engine/Graphics/Vulkan/):
//
//   Renderer -> IXRHI -> IXVulkan -> native Vulkan (Win/Linux)
//                                     -> MoltenVK -> Metal (Apple, future)
//
// RULES (binding):
// - No header under Engine/Graphics/IXRHI/ may include <vulkan/vulkan.h> or name
//   any Vk* type. Verified by the IXRHISmoke compile check (it includes every
//   IXRHI header in a TU without Vulkan headers on the include path).
// - Public types keep the IXRHI prefix; backend types use IXVulkan (never RHI*,
//   IRHI*, VulkanRHI).
// - No void* native-device casts and no GetNativeVk*() on these interfaces. The
//   single narrow escape hatch is ixvulkan::WrapFrameCommandList (Vulkan-module
//   header only) for the in-flight frame command buffer during strangler
//   migration. Every native access is inventoried in docs/architecture/.
//
// OWNERSHIP MODEL (binding, see IXRHIDevice.h for the full contract):
// - GPU resources (buffer/texture/sampler/shader): std::shared_ptr — real shared
//   lifetime (e.g. one shader feeding several pipeline variants; bind groups
//   keeping frame resources alive). Deterministic RAII destruction, no manual
//   vkDestroy in renderer code, no custom pools.
// - Pipelines, bind-group layouts/groups, command lists: std::unique_ptr —
//   unique ownership by the creating renderer.
// - Descriptors passed to Create* are consumed (copied) at creation; shaders and
//   layouts need not outlive the pipeline they built.

namespace ixrhi
{

class IXRHIBuffer;
class IXRHITexture;
class IXRHISampler;
class IXRHIShader;
class IXRHIGraphicsPipeline;
class IXRHIComputePipeline;
class IXRHIBindGroupLayout;
class IXRHIBindGroup;
class IXRHICommandList;
class IXRHISwapchain;
class IXRHIDevice;

// Engine-level status. VkResult NEVER crosses this boundary; the backend logs the
// underlying VkResult for diagnostics and translates it here. Matches current
// engine policy: creation entry points return bool/status (no exceptions), while
// unexpected backend failures abort via the backend's CheckVk equivalent, exactly
// like the renderers did before migration.
enum class IXRHIResult : int
{
    Ok = 0,
    OutOfMemory,
    DeviceLost,
    Unsupported,
    InvalidArgument,
    IoError,
};

inline const char* IXRHIResultName(IXRHIResult result)
{
    switch (result)
    {
    case IXRHIResult::Ok: return "Ok";
    case IXRHIResult::OutOfMemory: return "OutOfMemory";
    case IXRHIResult::DeviceLost: return "DeviceLost";
    case IXRHIResult::Unsupported: return "Unsupported";
    case IXRHIResult::InvalidArgument: return "InvalidArgument";
    case IXRHIResult::IoError: return "IoError";
    }
    return "Unknown";
}

} // namespace ixrhi
