// Borrowed render-pass token implementation.

#include "IXVulkanRenderPass.h"

namespace ixvulkan
{

IXVulkanRenderPass::IXVulkanRenderPass(IXVulkanDevice& device, VkRenderPass pass)
    : m_device(&device)
    , m_pass(pass)
{
}

std::unique_ptr<IXVulkanRenderPass> IXVulkanRenderPass::Borrow(IXVulkanDevice& device,
                                                               VkRenderPass pass)
{
    return std::make_unique<IXVulkanRenderPass>(device, pass);
}

} // namespace ixvulkan
