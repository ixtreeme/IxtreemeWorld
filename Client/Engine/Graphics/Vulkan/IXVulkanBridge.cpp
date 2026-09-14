// Frame-command-buffer bridge: the single inventoried native-access point.

#include "IXVulkanBridge.h"

#include "IXVulkanCommandList.h"
#include "IXVulkanDevice.h"

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

} // namespace ixvulkan
