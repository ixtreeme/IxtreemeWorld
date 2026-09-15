#pragma once

// IXVulkanBridge — inventoried native-access points for strangler migration.
//
// WrapFrameCommandList borrows the loop's already-recording in-flight command
// buffer so migrated renderers can record through IXRHICommandList without the
// frame loop itself migrating yet. The returned list is non-owning: the loop
// still begins/ends/submits the buffer. Must be used and destroyed within the
// frame that provided the buffer.
//
// NativeViewOf/NativeSamplerOf resolve the native view/sampler owned by an
// IXRHI texture created by THIS backend, for still-native consumers in
// transition (TerrainRenderer water refraction inputs, Phase 3B). They return
// null for foreign objects. Backend-scoped, editor/renderer transition only —
// never on public IXRHI interfaces, never a new E-hatch.
//
// Declared HERE (Vulkan module), never in IXRHI headers. Callers: the frame
// owner and in-transition native renderers only. Renderer code that is already
// IXRHI-native takes ixrhi:: types and never includes this.

#include <memory>

#include <vulkan/vulkan.h>

namespace ixrhi
{
class IXRHICommandList;
class IXRHIDevice;
class IXRHITexture;
class IXRHISampler;
class IXRHIRenderPass;
} // namespace ixrhi

namespace ixvulkan
{

std::unique_ptr<ixrhi::IXRHICommandList> WrapFrameCommandList(ixrhi::IXRHIDevice& device,
                                                              VkCommandBuffer frameCommandBuffer);

VkImageView NativeViewOf(const ixrhi::IXRHITexture& texture);
VkSampler NativeSamplerOf(const ixrhi::IXRHISampler& sampler);
VkRenderPass NativePassOf(const ixrhi::IXRHIRenderPass& pass);

} // namespace ixvulkan
