#pragma once

// IXVulkan fence/semaphore objects (explicit advanced use only; the normal
// frame flow needs no manual semaphore wiring — see IXRHIDevice::BeginFrame).

#include "IXRHISync.h"

#include <vulkan/vulkan.h>

namespace ixvulkan
{

class IXVulkanDevice;

class IXVulkanFence final : public ixrhi::IXRHIFence
{
public:
    IXVulkanFence(IXVulkanDevice& device, VkFence fence);
    ~IXVulkanFence() override;

    void Wait() override;
    void Reset() override;
    VkFence Native() const { return m_fence; }

private:
    IXVulkanDevice* m_device = nullptr;
    VkFence m_fence = VK_NULL_HANDLE;
};

class IXVulkanSemaphore final : public ixrhi::IXRHISemaphore
{
public:
    IXVulkanSemaphore(IXVulkanDevice& device, VkSemaphore semaphore);
    ~IXVulkanSemaphore() override;

    VkSemaphore Native() const { return m_semaphore; }

private:
    IXVulkanDevice* m_device = nullptr;
    VkSemaphore m_semaphore = VK_NULL_HANDLE;
};

} // namespace ixvulkan
