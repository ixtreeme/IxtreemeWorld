// IXVulkanSwapchain: main-window swapchain object. Owns the main pass, the
// per-image main targets (framebuffer + backend depth), and the non-owning
// backbuffer wrappers. The VkSwapchainKHR handle + images + views stay legacy
// infrastructure (§39); everything derived lives here.

#include "IXVulkanSwapchain.h"

#include "IXVulkanConversions.h"
#include "IXVulkanDevice.h"
#include "IXVulkanRenderTarget.h"
#include "IXVulkanResources.h"
#include "VulkanDevice.h"

namespace ixvulkan
{

IXVulkanBackbuffer::IXVulkanBackbuffer(VkImage image,
                                       VkImageView view,
                                       std::uint32_t width,
                                       std::uint32_t height,
                                       ixrhi::IXRHIFormat format,
                                       std::uint64_t generation,
                                       std::string debugName)
    : m_image(image)
    , m_view(view)
    , m_width(width)
    , m_height(height)
    , m_format(format)
    , m_generation(generation)
    , m_debugName(std::move(debugName))
{
}

IXVulkanSwapchain::IXVulkanSwapchain(IXVulkanDevice& device) : m_device(&device)
{
}

IXVulkanSwapchain::~IXVulkanSwapchain()
{
    Teardown();
}

std::uint32_t IXVulkanSwapchain::Width() const
{
    return m_device->Loop().GetSwapchainExtent().width;
}

std::uint32_t IXVulkanSwapchain::Height() const
{
    return m_device->Loop().GetSwapchainExtent().height;
}

ixrhi::IXRHIFormat IXVulkanSwapchain::ColorFormat() const
{
    return FromVkFormat(m_device->Loop().GetSwapchainFormat());
}

ixrhi::IXRHIFormat IXVulkanSwapchain::DepthFormat() const
{
    return FromVkFormat(m_device->Loop().GetDepthStencilFormat());
}

std::uint32_t IXVulkanSwapchain::ImageCount() const
{
    return m_device->Loop().GetSwapchainImageCount();
}

bool IXVulkanSwapchain::RequestResize(std::uint32_t width, std::uint32_t height)
{
    return m_device->RequestResize(width, height);
}

void IXVulkanSwapchain::Rebuild()
{
    Teardown();

    VulkanDevice& loop = m_device->Loop();
    const std::uint32_t imageCount = loop.GetSwapchainImageCount();
    const VkExtent2D extent = loop.GetSwapchainExtent();
    if (imageCount == 0 || extent.width == 0 || extent.height == 0)
        return;
    const ixrhi::IXRHIFormat colorFormat = ColorFormat();
    const ixrhi::IXRHIFormat depthFormat = DepthFormat();
    if (colorFormat == ixrhi::IXRHIFormat::Undefined ||
        depthFormat == ixrhi::IXRHIFormat::Undefined)
        return;

    ++m_generation;
    if (m_generation == 0)
        m_generation = 1; // 0 stays invalid (§53)

    VkRenderPass mainPass = m_device->CreateCompatRenderPass(colorFormat,
        depthFormat,
        ixrhi::IXRHILoadOp::Clear,
        ixrhi::IXRHIStoreOp::Store,
        ixrhi::IXRHILoadOp::Clear,
        ixrhi::IXRHIStoreOp::DontCare,
        "MainWindow.Pass",
        /*forPresent=*/true);
    m_device->SetDebugName(VK_OBJECT_TYPE_RENDER_PASS,
        reinterpret_cast<std::uint64_t>(mainPass),
        "MainWindow.Pass");
    m_mainPass = mainPass;

    m_backbuffers.reserve(imageCount);
    m_mainTargets.reserve(imageCount);
    for (std::uint32_t i = 0; i < imageCount; ++i)
    {
        auto backbuffer = std::make_shared<IXVulkanBackbuffer>(loop.GetSwapchainImage(i),
            loop.GetSwapchainImageView(i),
            extent.width,
            extent.height,
            colorFormat,
            m_generation,
            "MainWindow.Backbuffer");

        ixrhi::IXRHITextureDesc depthDesc;
        depthDesc.width = extent.width;
        depthDesc.height = extent.height;
        depthDesc.format = depthFormat;
        depthDesc.usage = ixrhi::IXRHITextureUsage::DepthStencilAttachment;
        depthDesc.debugName = "MainWindow.Depth";
        auto depth = m_device->CreateTexture(depthDesc, nullptr, 0);
        if (!depth)
        {
            m_device->CheckVk(VK_ERROR_OUT_OF_DEVICE_MEMORY,
                "MainWindow depth texture",
                __FILE__,
                __LINE__);
        }

        auto* depthNative = dynamic_cast<IXVulkanTexture*>(depth.get());
        VkFramebuffer framebuffer = m_device->CreateFramebufferFor(mainPass,
            backbuffer->NativeView(),
            depthNative != nullptr ? depthNative->NativeView() : VK_NULL_HANDLE,
            extent.width,
            extent.height);

        float clearColor[4] = {0.04f, 0.05f, 0.09f, 1.0f};
        auto target = std::make_unique<IXVulkanRenderTarget>(*m_device,
            backbuffer,
            std::move(depth),
            mainPass,
            /*ownPass=*/false,
            framebuffer,
            IXVulkanRenderPass::Borrow(*m_device, mainPass),
            clearColor,
            /*clearDepth=*/1.0f,
            /*clearStencil=*/0u,
            "MainWindow.Target");

        m_backbuffers.push_back(std::move(backbuffer));
        m_mainTargets.push_back(std::move(target));
    }

    // Mirror the main pass into the legacy object for still-native pipelines
    // (RmlUi, terrain/skinned direct paths). Owned here; legacy must not free.
    loop.SetMigrationMainPass(mainPass);
}

void IXVulkanSwapchain::Teardown()
{
    // Targets (and their framebuffers) die first; the shared pass dies with
    // the last reference below. Backbuffer wrappers are non-owning.
    m_mainTargets.clear();
    m_backbuffers.clear();
    if (m_mainPass != VK_NULL_HANDLE && m_device != nullptr)
    {
        vkDestroyRenderPass(m_device->NativeDevice(), m_mainPass, nullptr);
        m_mainPass = VK_NULL_HANDLE;
    }
    if (m_device != nullptr)
        m_device->Loop().SetMigrationMainPass(VK_NULL_HANDLE);
}

const ixrhi::IXRHIRenderPass* IXVulkanSwapchain::MainPass() const
{
    // All main targets share m_mainPass; any live target exposes its token.
    if (m_mainTargets.empty() || !m_mainTargets.front())
        return nullptr;
    return m_mainTargets.front()->GetPass();
}

ixrhi::IXRHIRenderTarget* IXVulkanSwapchain::MainTarget(std::uint32_t imageIndex)
{
    return imageIndex < m_mainTargets.size() ? m_mainTargets[imageIndex].get() : nullptr;
}

const ixrhi::IXRHITexture* IXVulkanSwapchain::Backbuffer(std::uint32_t imageIndex) const
{
    return imageIndex < m_backbuffers.size() ? m_backbuffers[imageIndex].get() : nullptr;
}

} // namespace ixvulkan
