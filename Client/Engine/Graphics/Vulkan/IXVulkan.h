#pragma once

// IXVulkan* — Vulkan backend implementation boundary (Phase 1: reserved).
//
// Vulkan REMAINS the graphics backend (no BGFX, no Metal backend in this phase).
// Future Apple support is Vulkan -> MoltenVK -> Metal, still behind this boundary.
//
// Phase 1 rule: no Vk* type may be introduced outside Engine/Graphics/Vulkan,
// libs/platform/VulkanDevice.*, and the existing libs/render/*Renderer.* files.
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
