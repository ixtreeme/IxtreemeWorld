#pragma once

// IXVulkanRenderPass — borrowed-pass unwrapping (Vulkan-module only).
// Created by the frame owner from a live VkRenderPass; renderers only ever see
// the ixrhi::IXRHIRenderPass base. Non-owning: the pass owner outlives it.

#include "IXRHIRenderPass.h"

#include <vulkan/vulkan.h>

#include <memory>

namespace ixvulkan
{

class IXVulkanDevice;

class IXVulkanRenderPass final : public ixrhi::IXRHIRenderPass
{
public:
    // Borrows pass (may be null = backend default). Backend device reference
    // kept for symmetry/debugging; not dereferenced for lifetime.
    IXVulkanRenderPass(IXVulkanDevice& device, VkRenderPass pass);

    VkRenderPass Native() const { return m_pass; }

    static std::unique_ptr<IXVulkanRenderPass> Borrow(IXVulkanDevice& device, VkRenderPass pass);

private:
    IXVulkanDevice* m_device = nullptr;
    VkRenderPass m_pass = VK_NULL_HANDLE;
};

} // namespace ixvulkan
