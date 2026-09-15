#pragma once

// IXVulkanSwapchain — main-window swapchain object (Phase 3C, §13/55).
//
// Owns everything swapchain-derived EXCEPT the VkSwapchainKHR handle + images
// themselves (legacy device infrastructure until a later phase, §39): the main
// VkRenderPass, per-image framebuffers (via render targets), per-image depth
// textures, and non-owning backbuffer wrappers. Generation bumps on every
// rebuild; stale references are detectable without Vulkan knowledge.
//
// Multi-window future (§90/91): one instance per window/swapchain; the device
// holds the main one today.

#include "IXRHISwapchain.h"
#include "IXRHITexture.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ixrhi
{
class IXRHIRenderTarget;
}

namespace ixvulkan
{

class IXVulkanDevice;

// Non-owning swapchain-image view for the frame context backBuffer channel.
// Owned by this swapchain (rebuilt per generation); renderers must never
// destroy it and must not use it past the frame/generation (§50).
class IXVulkanBackbuffer final : public ixrhi::IXRHITexture
{
public:
    IXVulkanBackbuffer(VkImage image,
                       VkImageView view,
                       std::uint32_t width,
                       std::uint32_t height,
                       ixrhi::IXRHIFormat format,
                       std::uint64_t generation,
                       std::string debugName);
    ~IXVulkanBackbuffer() override = default;

    std::uint32_t Width() const override { return m_width; }
    std::uint32_t Height() const override { return m_height; }
    ixrhi::IXRHIFormat Format() const override { return m_format; }
    const std::string& DebugName() const override { return m_debugName; }

    VkImage Native() const { return m_image; }
    VkImageView NativeView() const { return m_view; }
    std::uint64_t Generation() const { return m_generation; }

private:
    VkImage m_image = VK_NULL_HANDLE; // borrowed (legacy swapchain owns)
    VkImageView m_view = VK_NULL_HANDLE; // borrowed (legacy swapchain owns)
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    ixrhi::IXRHIFormat m_format = ixrhi::IXRHIFormat::Undefined;
    std::uint64_t m_generation = 0;
    std::string m_debugName;
};

class IXVulkanSwapchain final : public ixrhi::IXRHISwapchain
{
public:
    explicit IXVulkanSwapchain(IXVulkanDevice& device);
    ~IXVulkanSwapchain() override;

    IXVulkanSwapchain(const IXVulkanSwapchain&) = delete;
    IXVulkanSwapchain& operator=(const IXVulkanSwapchain&) = delete;

    // ---- ixrhi::IXRHISwapchain ----
    std::uint32_t Width() const override;
    std::uint32_t Height() const override;
    ixrhi::IXRHIFormat ColorFormat() const override;
    ixrhi::IXRHIFormat DepthFormat() const override;
    std::uint32_t ImageCount() const override;
    std::uint64_t Generation() const override { return m_generation; }
    bool RequestResize(std::uint32_t width, std::uint32_t height) override;

    // ---- backend frame authority ----
    // Rebuilds pass + per-image targets/wrappers for the CURRENT legacy
    // images. Bumps the generation. Called by the device after every
    // swapchain (re)creation and once at startup.
    void Rebuild();
    void Teardown();
    const ixrhi::IXRHIRenderPass* MainPass() const;
    ixrhi::IXRHIRenderTarget* MainTarget(std::uint32_t imageIndex);
    const ixrhi::IXRHITexture* Backbuffer(std::uint32_t imageIndex) const;

private:
    IXVulkanDevice* m_device = nullptr;
    std::uint64_t m_generation = 0;
    VkRenderPass m_mainPass = VK_NULL_HANDLE; // shared by all main targets; owned here
    std::vector<std::shared_ptr<IXVulkanBackbuffer>> m_backbuffers;
    std::vector<std::unique_ptr<ixrhi::IXRHIRenderTarget>> m_mainTargets;
};

} // namespace ixvulkan
