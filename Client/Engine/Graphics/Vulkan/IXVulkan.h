#pragma once

// IXVulkan* — Vulkan backend implementation boundary.
//
// Vulkan REMAINS the graphics backend (no BGFX, no Metal backend in this phase).
// Future Apple support is Vulkan -> MoltenVK -> Metal, still behind this boundary.
//
// Boundary rule (Phases 2–3H, verified by grep + build graph): no Vk* type may
// appear outside Engine/Graphics/Vulkan and libs/platform/VulkanDevice.*
// (backend bootstrap). All generic renderers speak ixrhi::IXRHI* only.
// New code must forward-declare or use ixrhi::IXRHI* handles instead.
//
//   Renderers -> IXRHI (Engine/Graphics/IXRHI/)
//             -> IXVulkan (this directory)
//             -> native Vulkan (Win/Linux) / MoltenVK -> Metal (Apple, future)

namespace ixvulkan
{

// Forward declarations only. Phase 2 implements these against VulkanDevice.
class IXVulkanDevice;
class IXVulkanBuffer;
class IXVulkanTexture;
class IXVulkanPipeline;
class IXVulkanCommandList;
class IXVulkanSwapchain;

} // namespace ixvulkan
