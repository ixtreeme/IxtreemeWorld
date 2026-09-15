// Frame-command-buffer bridge: the single inventoried native-access point.

#include "IXVulkanBridge.h"

#include "IXVulkanCommandList.h"
#include "IXVulkanDevice.h"
#include "IXVulkanRenderPass.h"
#include "IXVulkanResources.h"

namespace ixvulkan
{

std::unique_ptr<ixrhi::IXRHICommandList> WrapFrameCommandList(ixrhi::IXRHIDevice& device,
                                                              VkCommandBuffer frameCommandBuffer)
{
    auto* backend = dynamic_cast<IXVulkanDevice*>(&device);
    if (backend == nullptr || frameCommandBuffer == VK_NULL_HANDLE)
        return nullptr;
    return std::make_unique<IXVulkanCommandList>(*backend, frameCommandBuffer);
}

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
