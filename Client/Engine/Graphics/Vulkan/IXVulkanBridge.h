#pragma once

// IXVulkanBridge — backend-private native resolution for in-transition native
// consumers (Phase 3C Retired E3; remaining entries inventoried).
//
// NativeViewOf/NativeSamplerOf/NativePassOf resolve the native objects owned by
// IXRHI resources created by THIS backend, for still-native downstream users
// (TerrainRenderer water refraction inputs, terrain/skinned pass tokens) and
// the editor adapter's UI registration. They return null for foreign objects.
//
// Backend-scoped, transition-only — never on public IXRHI interfaces. The old
// E3 WrapFrameCommandList (borrowed frame command buffer) is DELETED: migrated
// renderers receive the IXRHI-owned list from the frame context, and legacy
// renderers read the active buffer through the synced legacy frame shim.
//
// Declared HERE (Vulkan module), never in IXRHI headers.

#include <memory>

#include <vulkan/vulkan.h>

namespace ixrhi
{
class IXRHITexture;
class IXRHISampler;
class IXRHIRenderPass;
} // namespace ixrhi

namespace ixvulkan
{

VkImageView NativeViewOf(const ixrhi::IXRHITexture& texture);
VkSampler NativeSamplerOf(const ixrhi::IXRHISampler& sampler);
VkRenderPass NativePassOf(const ixrhi::IXRHIRenderPass& pass);

} // namespace ixvulkan
