// IXVulkanRenderTarget: pass + framebuffer owned by the backend, created from
// the facade's IXRHI textures (views resolved internally, never exposed).

#include "IXVulkanRenderTarget.h"

#include "IXVulkanCommandList.h"
#include "IXVulkanConversions.h"
#include "IXVulkanDevice.h"
#include "IXVulkanResources.h"

#include <array>
#include <cstring>

namespace ixvulkan
{

IXVulkanRenderTarget::IXVulkanRenderTarget(IXVulkanDevice& device,
                                           std::shared_ptr<ixrhi::IXRHITexture> color,
                                           std::shared_ptr<ixrhi::IXRHITexture> depth,
                                           VkRenderPass pass,
                                           bool ownPass,
                                           VkFramebuffer framebuffer,
                                           VkImageView ownedDepthLayerView,
                                           std::unique_ptr<IXVulkanRenderPass> passToken,
                                           float clearColor[4],
                                           float clearDepth,
                                           std::uint32_t clearStencil,
                                           std::string debugName)
    : m_device(&device)
    , m_color(std::move(color))
    , m_depth(std::move(depth))
    , m_pass(pass)
    , m_ownPass(ownPass)
    , m_framebuffer(framebuffer)
    , m_ownedDepthLayerView(ownedDepthLayerView)
    , m_passToken(std::move(passToken))
    , m_clearDepth(clearDepth)
    , m_clearStencil(clearStencil)
    , m_debugName(std::move(debugName))
{
    std::memcpy(m_clearColor, clearColor, sizeof(m_clearColor));
    if (m_color)
    {
        m_width = m_color->Width();
        m_height = m_color->Height();
    }
    else if (m_depth)
    {
        m_width = m_depth->Width();
        m_height = m_depth->Height();
    }
}

IXVulkanRenderTarget::~IXVulkanRenderTarget()
{
    if (m_device == nullptr)
        return;
    const VkDevice native = m_device->NativeDevice();
    if (m_framebuffer != VK_NULL_HANDLE)
        vkDestroyFramebuffer(native, m_framebuffer, nullptr);
    if (m_ownedDepthLayerView != VK_NULL_HANDLE)
        vkDestroyImageView(native, m_ownedDepthLayerView, nullptr);
    if (m_ownPass && m_pass != VK_NULL_HANDLE)
        vkDestroyRenderPass(native, m_pass, nullptr);
}

void IXVulkanRenderTarget::Begin(ixrhi::IXRHICommandList& cmd) const
{
    auto* native = dynamic_cast<IXVulkanCommandList*>(&cmd);
    if (native == nullptr)
        return;
    // Clear-value order mirrors the framebuffer attachments (color first
    // when present, then depth) — depth-only targets clear values[0] as depth.
    VkClearValue clearValues[2]{};
    std::uint32_t clearValueCount = 0;
    if (m_color)
    {
        clearValues[clearValueCount].color.float32[0] = m_clearColor[0];
        clearValues[clearValueCount].color.float32[1] = m_clearColor[1];
        clearValues[clearValueCount].color.float32[2] = m_clearColor[2];
        clearValues[clearValueCount].color.float32[3] = m_clearColor[3];
        ++clearValueCount;
    }
    if (m_depth)
    {
        clearValues[clearValueCount].depthStencil = {m_clearDepth, m_clearStencil};
        ++clearValueCount;
    }

    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = m_pass;
    begin.framebuffer = m_framebuffer;
    begin.renderArea.offset = {0, 0};
    begin.renderArea.extent = {m_width, m_height};
    // Attachment order mirrors CreateRenderTarget: color first when present.
    begin.clearValueCount = clearValueCount;
    begin.pClearValues = clearValues;
    vkCmdBeginRenderPass(native->Native(), &begin, VK_SUBPASS_CONTENTS_INLINE);
    native->SetViewport(0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height));
    native->SetScissor(0, 0, m_width, m_height);
}

void IXVulkanRenderTarget::End(ixrhi::IXRHICommandList& cmd) const
{
    auto* native = dynamic_cast<IXVulkanCommandList*>(&cmd);
    if (native == nullptr)
        return;
    vkCmdEndRenderPass(native->Native());
}

std::unique_ptr<ixrhi::IXRHIRenderTarget> IXVulkanDevice::CreateRenderTarget(
    const ixrhi::IXRHIRenderTargetDesc& desc)
{
    auto* color = desc.color ? dynamic_cast<IXVulkanTexture*>(desc.color.get()) : nullptr;
    if (desc.color && color == nullptr)
        return nullptr;
    auto* depth = desc.depth ? dynamic_cast<IXVulkanTexture*>(desc.depth.get()) : nullptr;
    if (desc.depth && depth == nullptr)
        return nullptr;
    if (color && (color->Width() == 0 || color->Height() == 0))
        return nullptr;
    if (depth && (depth->Width() == 0 || depth->Height() == 0))
        return nullptr;
    if (color == nullptr && depth == nullptr)
        return nullptr;
    if (depth && depth->ArrayLayers() > 1 && desc.depthLayer >= depth->ArrayLayers())
        return nullptr;

    const bool hasColor = color != nullptr;
    const bool hasDepth = depth != nullptr;
    VkRenderPass pass = CreateCompatRenderPass(hasColor ? desc.color->Format() : ixrhi::IXRHIFormat::Undefined,
        hasDepth ? desc.depth->Format() : ixrhi::IXRHIFormat::Undefined,
        desc.colorLoad,
        desc.colorStore,
        desc.depthLoad,
        desc.depthStore,
        desc.debugName.c_str());
    if (pass == VK_NULL_HANDLE)
        return nullptr;

    // Layered depth (shadow cascades) needs a per-slice 2D view owned by the
    // target; the texture keeps its full-array sampling view.
    VkImageView ownedDepthLayerView = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;
    if (depth)
    {
        if (depth->ArrayLayers() > 1)
        {
            VkImageViewCreateInfo layerView{};
            layerView.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            layerView.image = depth->Native();
            layerView.viewType = VK_IMAGE_VIEW_TYPE_2D;
            layerView.format = ToVkFormat(depth->Format());
            layerView.subresourceRange.aspectMask = ToVkAspectMask(depth->Format());
            layerView.subresourceRange.baseArrayLayer = desc.depthLayer;
            layerView.subresourceRange.levelCount = 1;
            layerView.subresourceRange.layerCount = 1;
            CheckVk(vkCreateImageView(NativeDevice(), &layerView, nullptr, &ownedDepthLayerView),
                "vkCreateImageView(depthLayer)",
                __FILE__,
                __LINE__);
            depthView = ownedDepthLayerView;
        }
        else
        {
            depthView = depth->NativeView();
        }
    }

    const std::uint32_t width = hasColor ? desc.color->Width() : desc.depth->Width();
    const std::uint32_t height = hasColor ? desc.color->Height() : desc.depth->Height();
    // Depth-only targets bind the depth view in attachment slot 0 (the pass
    // has no color attachment); color targets keep the color/depth ordering.
    VkFramebuffer framebuffer = CreateFramebufferFor(pass,
        hasColor ? color->NativeView() : depthView,
        hasColor ? depthView : VK_NULL_HANDLE,
        width,
        height);

    auto token = IXVulkanRenderPass::Borrow(*this, pass);
    float clearColor[4] = {
        desc.clearColor[0], desc.clearColor[1], desc.clearColor[2], desc.clearColor[3]};
    return std::make_unique<IXVulkanRenderTarget>(*this,
        desc.color,
        desc.depth,
        pass,
        /*ownPass=*/true,
        framebuffer,
        ownedDepthLayerView,
        std::move(token),
        clearColor,
        desc.clearDepth,
        desc.clearStencil,
        desc.debugName);
}

VkRenderPass IXVulkanDevice::CreateCompatRenderPass(ixrhi::IXRHIFormat colorFormat,
                                                    ixrhi::IXRHIFormat depthFormat,
                                                    ixrhi::IXRHILoadOp colorLoad,
                                                    ixrhi::IXRHIStoreOp colorStore,
                                                    ixrhi::IXRHILoadOp depthLoad,
                                                    ixrhi::IXRHIStoreOp depthStore,
                                                    const char* debugName,
                                                    bool forPresent) const
{
    // Pass mirrors the old offscreen clear/load passes: color final READ_ONLY
    // (sampled by composite/editor), depth final ATTACHMENT; initial layouts
    // follow the load ops. Subpass dependencies preserve the old ordering
    // (fragment-shader reads before attachment writes and vice versa).
    // Depth-only shape (shadow maps): single depth attachment, no color
    // reference; the same dependency pattern with depth stages.
    const bool hasColor = colorFormat != ixrhi::IXRHIFormat::Undefined;
    const bool hasDepth = depthFormat != ixrhi::IXRHIFormat::Undefined;
    if (!hasColor && !hasDepth)
        return VK_NULL_HANDLE;
    VkAttachmentDescription attachments[2]{};
    uint32_t attachmentCount = 0;    VkAttachmentReference colorRef{};
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference depthRef{};
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    if (hasColor)
    {
        attachments[attachmentCount].format = ToVkFormat(colorFormat);
        attachments[attachmentCount].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[attachmentCount].loadOp = ToVkLoadOp(colorLoad);
        attachments[attachmentCount].storeOp = ToVkStoreOp(colorStore);
        attachments[attachmentCount].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[attachmentCount].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[attachmentCount].initialLayout = colorLoad == ixrhi::IXRHILoadOp::Clear
            ? VK_IMAGE_LAYOUT_UNDEFINED
            : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        attachments[attachmentCount].finalLayout = forPresent ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                                                              : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        colorRef.attachment = attachmentCount;
        ++attachmentCount;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;
    }

    std::array<VkSubpassDependency, 2> dependencies{};
    if (hasColor)
    {
        dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass = 0;
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }
    else
    {
        dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass = 0;
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        dependencies[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }

    if (hasDepth)
    {
        attachments[attachmentCount].format = ToVkFormat(depthFormat);
        attachments[attachmentCount].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[attachmentCount].loadOp = ToVkLoadOp(depthLoad);
        attachments[attachmentCount].storeOp = forPresent ? VK_ATTACHMENT_STORE_OP_DONT_CARE : ToVkStoreOp(depthStore);
        attachments[attachmentCount].stencilLoadOp = ToVkLoadOp(depthLoad);
        attachments[attachmentCount].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[attachmentCount].initialLayout = depthLoad == ixrhi::IXRHILoadOp::Clear
            ? VK_IMAGE_LAYOUT_UNDEFINED
            : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachments[attachmentCount].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthRef.attachment = attachmentCount;
        ++attachmentCount;
        subpass.pDepthStencilAttachment = &depthRef;
    }

    VkRenderPassCreateInfo passInfo{};
    passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    passInfo.attachmentCount = attachmentCount;
    passInfo.pAttachments = attachments;
    passInfo.subpassCount = 1;
    passInfo.pSubpasses = &subpass;
    passInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
    passInfo.pDependencies = dependencies.data();
    VkRenderPass pass = VK_NULL_HANDLE;
    CheckVk(vkCreateRenderPass(NativeDevice(), &passInfo, nullptr, &pass),
        "vkCreateRenderPass(target)",
        __FILE__,
        __LINE__);
    SetDebugName(VK_OBJECT_TYPE_RENDER_PASS, reinterpret_cast<std::uint64_t>(pass), debugName);
    return pass;
}

VkFramebuffer IXVulkanDevice::CreateFramebufferFor(VkRenderPass pass,
                                                   VkImageView colorView,
                                                   VkImageView depthViewOrNull,
                                                   std::uint32_t width,
                                                   std::uint32_t height) const
{
    VkImageView views[2] = {colorView, depthViewOrNull};
    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = pass;
    fbInfo.attachmentCount = depthViewOrNull != VK_NULL_HANDLE ? 2u : 1u;
    fbInfo.pAttachments = views;
    fbInfo.width = width;
    fbInfo.height = height;
    fbInfo.layers = 1;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    CheckVk(vkCreateFramebuffer(NativeDevice(), &fbInfo, nullptr, &framebuffer),
        "vkCreateFramebuffer(target)",
        __FILE__,
        __LINE__);
    return framebuffer;
}

void IXVulkanDevice::WaitIdle()
{
    WaitForAsyncPresentIdle();
    CheckVk(vkDeviceWaitIdle(NativeDevice()), "vkDeviceWaitIdle", __FILE__, __LINE__);
}

} // namespace ixvulkan
