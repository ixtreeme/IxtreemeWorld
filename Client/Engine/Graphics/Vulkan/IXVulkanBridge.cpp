// Backend-private native resolution (see header inventory).

#include "IXVulkanBridge.h"

#include "IXVulkanCommandList.h"
#include "IXVulkanDevice.h"
#include "IXVulkanRenderPass.h"
#include "IXVulkanResources.h"

namespace ixvulkan
{

VkImageView NativeViewOf(const ixrhi::IXRHITexture& texture)
{
    auto* native = dynamic_cast<const IXVulkanTexture*>(&texture);
    return native != nullptr ? native->NativeView() : VK_NULL_HANDLE;
}

VkSampler NativeSamplerOf(const ixrhi::IXRHISampler& sampler)
{
    auto* native = dynamic_cast<const IXVulkanSampler*>(&sampler);
    return native != nullptr ? native->Native() : VK_NULL_HANDLE;
}

VkRenderPass NativePassOf(const ixrhi::IXRHIRenderPass& pass)
{
    auto* native = dynamic_cast<const IXVulkanRenderPass*>(&pass);
    return native != nullptr ? native->Native() : VK_NULL_HANDLE;
}

} // namespace ixvulkan
