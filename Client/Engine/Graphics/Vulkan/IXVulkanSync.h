#pragma once

// IXVulkan fence/semaphore + swapchain info adapter. The swapchain adapter
// answers queries from the live VulkanDevice; pacing (acquire/present) stays in
// the frame loop until Phase 3B.

#include "IXRHISwapchain.h"
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

class IXVulkanSwapchain final : public ixrhi::IXRHISwapchain
{
public:
    explicit IXVulkanSwapchain(IXVulkanDevice& device);

    std::uint32_t Width() const override;
    std::uint32_t Height() const override;
    ixrhi::IXRHIFormat ColorFormat() const override;
    ixrhi::IXRHIFormat DepthFormat() const override;
    std::uint32_t ImageCount() const override;
    bool RequestResize(std::uint32_t width, std::uint32_t height) override;

private:
    IXVulkanDevice* m_device = nullptr;
};

} // namespace ixvulkan
