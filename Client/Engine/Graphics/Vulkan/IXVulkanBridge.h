#pragma once

// IXVulkanBridge — the ONE inventoried native-access point for strangler
// migration (Phase 2, §24 escape hatch).
//
// WrapFrameCommandList borrows the loop's already-recording in-flight command
// buffer so migrated renderers can record through IXRHICommandList without the
// frame loop itself migrating yet. The returned list is non-owning: the loop
// still begins/ends/submits the buffer. Must be used and destroyed within the
// frame that provided the buffer.
//
// Declared HERE (Vulkan module), never in IXRHI headers. Callers: the frame
// owner only (apps/client EngineApplication). Renderer code takes
// ixrhi::IXRHICommandList& and never includes this.

#include <memory>

#include <vulkan/vulkan.h>

namespace ixrhi
{
class IXRHICommandList;
class IXRHIDevice;
} // namespace ixrhi

namespace ixvulkan
{

std::unique_ptr<ixrhi::IXRHICommandList> WrapFrameCommandList(ixrhi::IXRHIDevice& device,
                                                              VkCommandBuffer frameCommandBuffer);

} // namespace ixvulkan
