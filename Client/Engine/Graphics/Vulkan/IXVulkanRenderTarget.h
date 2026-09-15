#pragma once

// IXVulkanRenderTarget — backend render-target (pass + framebuffer) created
// from IXRHI textures. The generic facade (OffscreenSceneRenderer) owns this
// through the ixrhi::IXRHIRenderTarget interface and never sees Vulkan.

#include "IXRHIRenderTarget.h"
#include "IXVulkanRenderPass.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>

namespace ixvulkan
{

class IXVulkanDevice;

class IXVulkanRenderTarget final : public ixrhi::IXRHIRenderTarget
{
public:
    IXVulkanRenderTarget(IXVulkanDevice& device,
                         std::shared_ptr<ixrhi::IXRHITexture> color,
                         std::shared_ptr<ixrhi::IXRHITexture> depth,
                         VkRenderPass pass,
                         VkFramebuffer framebuffer,
                         std::unique_ptr<IXVulkanRenderPass> passToken,
                         float clearColor[4],
                         float clearDepth,
                         std::uint32_t clearStencil,
                         std::string debugName);
    ~IXVulkanRenderTarget() override;

    void Begin(ixrhi::IXRHICommandList& cmd) const override;
    void End(ixrhi::IXRHICommandList& cmd) const override;
    const ixrhi::IXRHIRenderPass* GetPass() const override { return m_passToken.get(); }
    std::uint32_t Width() const override { return m_width; }
    std::uint32_t Height() const override { return m_height; }

private:
    IXVulkanDevice* m_device = nullptr;
    // Shared lifetime with the target: views stay valid while the framebuffer
    // exists, even if the facade drops its own references.
    std::shared_ptr<ixrhi::IXRHITexture> m_color;
    std::shared_ptr<ixrhi::IXRHITexture> m_depth;
    VkRenderPass m_pass = VK_NULL_HANDLE;
    VkFramebuffer m_framebuffer = VK_NULL_HANDLE;
    std::unique_ptr<IXVulkanRenderPass> m_passToken;
    float m_clearColor[4]{};
    float m_clearDepth = 1.0f;
    std::uint32_t m_clearStencil = 0;
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    std::string m_debugName;
};

} // namespace ixvulkan
