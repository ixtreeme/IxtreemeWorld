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
                                           VkFramebuffer framebuffer,
                                           std::unique_ptr<IXVulkanRenderPass> passToken,
                                           float clearColor[4],
                                           float clearDepth,
                                           std::uint32_t clearStencil,
                                           std::string debugName)
    : m_device(&device)
    , m_color(std::move(color))
    , m_depth(std::move(depth))
    , m_pass(pass)
    , m_framebuffer(framebuffer)
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
}

IXVulkanRenderTarget::~IXVulkanRenderTarget()
{
    if (m_device == nullptr)
        return;
    const VkDevice native = m_device->NativeDevice();
    if (m_framebuffer != VK_NULL_HANDLE)
        vkDestroyFramebuffer(native, m_framebuffer, nullptr);
    if (m_pass != VK_NULL_HANDLE)
        vkDestroyRenderPass(native, m_pass, nullptr);
}

void IXVulkanRenderTarget::Begin(ixrhi::IXRHICommandList& cmd) const
{
    auto* native = dynamic_cast<IXVulkanCommandList*>(&cmd);
    if (native == nullptr)
        return;
    VkClearValue clearValues[2]{};
    clearValues[0].color.float32[0] = m_clearColor[0];
    clearValues[0].color.float32[1] = m_clearColor[1];
    clearValues[0].color.float32[2] = m_clearColor[2];
    clearValues[0].color.float32[3] = m_clearColor[3];
    clearValues[1].depthStencil = {m_clearDepth, m_clearStencil};

    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = m_pass;
    begin.framebuffer = m_framebuffer;
    begin.renderArea.offset = {0, 0};
    begin.renderArea.extent = {m_width, m_height};
    begin.clearValueCount = m_depth ? 2u : 1u;
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
    if (!desc.color || desc.color->Width() == 0 || desc.color->Height() == 0)
        return nullptr;
    auto* color = dynamic_cast<IXVulkanTexture*>(desc.color.get());
    if (color == nullptr)
        return nullptr;
    auto* depth = dynamic_cast<IXVulkanTexture*>(desc.depth.get());
    if (desc.depth && depth == nullptr)
        return nullptr;

    const VkFormat colorFormat = ToVkFormat(desc.color->Format());
    const bool hasDepth = depth != nullptr;
    const VkFormat depthFormat = hasDepth ? ToVkFormat(desc.depth->Format()) : VK_FORMAT_UNDEFINED;

    // Pass mirrors the old offscreen clear/load passes: color final READ_ONLY
    // (sampled by composite/editor), depth final ATTACHMENT; initial layouts
    // follow the load ops. Subpass dependencies preserve the old ordering
    // (fragment-shader reads before attachment writes and vice versa).
    VkAttachmentDescription attachments[2]{};
    attachments[0].format = colorFormat;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = ToVkLoadOp(desc.colorLoad);
    attachments[0].storeOp = ToVkStoreOp(desc.colorStore);
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = desc.colorLoad == ixrhi::IXRHILoadOp::Clear
        ? VK_IMAGE_LAYOUT_UNDEFINED
        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthRef{};
    depthRef.attachment = 1;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    std::array<VkSubpassDependency, 2> dependencies{};
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

    uint32_t attachmentCount = 1;
    if (hasDepth)
    {
        attachments[1].format = depthFormat;
        attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[1].loadOp = ToVkLoadOp(desc.depthLoad);
        attachments[1].storeOp = ToVkStoreOp(desc.depthStore);
        attachments[1].stencilLoadOp = ToVkLoadOp(desc.depthLoad);
        attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].initialLayout = desc.depthLoad == ixrhi::IXRHILoadOp::Clear
            ? VK_IMAGE_LAYOUT_UNDEFINED
            : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        subpass.pDepthStencilAttachment = &depthRef;
        attachmentCount = 2;
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
    SetDebugName(VK_OBJECT_TYPE_RENDER_PASS,
        reinterpret_cast<std::uint64_t>(pass),
        desc.debugName.c_str());

    VkImageView views[2] = {color->NativeView(), hasDepth ? depth->NativeView() : VK_NULL_HANDLE};
    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = pass;
    fbInfo.attachmentCount = attachmentCount;
    fbInfo.pAttachments = views;
    fbInfo.width = desc.color->Width();
    fbInfo.height = desc.color->Height();
    fbInfo.layers = 1;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    CheckVk(vkCreateFramebuffer(NativeDevice(), &fbInfo, nullptr, &framebuffer),
        "vkCreateFramebuffer(target)",
        __FILE__,
        __LINE__);

    auto token = IXVulkanRenderPass::Borrow(*this, pass);
    float clearColor[4] = {
        desc.clearColor[0], desc.clearColor[1], desc.clearColor[2], desc.clearColor[3]};
    return std::make_unique<IXVulkanRenderTarget>(*this,
        desc.color,
        desc.depth,
        pass,
        framebuffer,
        std::move(token),
        clearColor,
        desc.clearDepth,
        desc.clearStencil,
        desc.debugName);
}

void IXVulkanDevice::WaitIdle()
{
    CheckVk(vkDeviceWaitIdle(NativeDevice()), "vkDeviceWaitIdle", __FILE__, __LINE__);
}

} // namespace ixvulkan
