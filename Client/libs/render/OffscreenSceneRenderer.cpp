#include "OffscreenSceneRenderer.h"

#include "Debug.h"
#include "asset/IAssetReader.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
const char* VkResultName(VkResult result)
{
    switch (result)
    {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    default: return "VK_RESULT_UNKNOWN";
    }
}

void CheckVk(VkResult result, const char* call, const char* file, int line)
{
    if (result == VK_SUCCESS)
        return;

    Tracenf("%s:%d: Vulkan call failed: %s -> %s (%d)",
        file,
        line,
        call,
        VkResultName(result),
        result);
    std::abort();
}

#define VK_CHECK(call) CheckVk((call), #call, __FILE__, __LINE__)

std::vector<char> ReadBinaryFile(client::asset::IAssetReader& assets, const std::string& path)
{
    auto bytes = assets.ReadAll(path);
    if (!bytes)
    {
        Tracenf("[OFFSCREEN] failed to open shader: %s", path.c_str());
        std::abort();
    }

    return std::vector<char>(bytes->begin(), bytes->end());
}

VkShaderModule CreateShaderModule(VkDevice device, client::asset::IAssetReader& assets, const std::string& path)
{
    const std::vector<char> code = ReadBinaryFile(assets, path);
    VkShaderModuleCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    create.codeSize = code.size();
    create.pCode = reinterpret_cast<const std::uint32_t*>(code.data());

    VkShaderModule module = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(device, &create, nullptr, &module));
    return module;
}

bool HasStencil(VkFormat format)
{
    return format == VK_FORMAT_D16_UNORM_S8_UINT ||
        format == VK_FORMAT_D24_UNORM_S8_UINT ||
        format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

void TransitionImage(VkCommandBuffer cmd,
    VkImage image,
    VkImageAspectFlags aspect,
    VkImageLayout oldLayout,
    VkImageLayout newLayout,
    VkPipelineStageFlags srcStage,
    VkPipelineStageFlags dstStage,
    VkAccessFlags srcAccess,
    VkAccessFlags dstAccess)
{
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspect;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}
}

bool OffscreenSceneRenderer::Create(VulkanDevice& device, client::asset::IAssetReader& assets)
{
    Destroy();
    m_device = device.GetDevice();
    m_assets = &assets;
    m_extent = device.GetSwapchainExtent();
    m_colorFormat = device.GetSwapchainFormat();
    m_depthFormat = device.GetDepthStencilFormat();

    if (m_extent.width == 0 || m_extent.height == 0)
        return false;

    m_ready = CreateRenderPass(device) &&
        CreateImages(device) &&
        CreateFramebuffer(device) &&
        CreateSampler() &&
        CreateDescriptorResources() &&
        CreatePipeline(device);

    if (m_ready)
    {
        UpdateDescriptor();
        Tracenf("[OFFSCREEN] Targets created: %ux%u, format=swapchain-compatible",
            m_extent.width,
            m_extent.height);
        Tracen("[OFFSCREEN] Composite-pass active: swap-chain blit + central Reinhard tone-mapping");
    }
    else
    {
        Tracen("[OFFSCREEN] Failed to initialize, falling back to direct swap-chain rendering");
        Destroy();
    }
    return m_ready;
}

bool OffscreenSceneRenderer::Recreate(VulkanDevice& device)
{
    if (!m_assets)
        return false;
    device.WaitIdle();
    return Create(device, *m_assets);
}

void OffscreenSceneRenderer::BeginMainPass(VulkanDevice& device, bool clear)
{
    if (!m_ready || !device.IsFrameActive() || m_passActive)
        return;

    VkClearValue clearValues[2]{};
    clearValues[0].color.float32[0] = 0.04f;
    clearValues[0].color.float32[1] = 0.05f;
    clearValues[0].color.float32[2] = 0.09f;
    clearValues[0].color.float32[3] = 1.0f;
    clearValues[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = clear ? m_renderPass : m_loadRenderPass;
    begin.framebuffer = m_framebuffer;
    begin.renderArea.offset = {0, 0};
    begin.renderArea.extent = m_extent;
    begin.clearValueCount = static_cast<std::uint32_t>(std::size(clearValues));
    begin.pClearValues = clearValues;

    vkCmdBeginRenderPass(device.GetCommandBuffer(), &begin, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(m_extent.width);
    viewport.height = static_cast<float>(m_extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(device.GetCommandBuffer(), 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = m_extent;
    vkCmdSetScissor(device.GetCommandBuffer(), 0, 1, &scissor);

    m_passActive = true;
}

void OffscreenSceneRenderer::EndMainPass(VulkanDevice& device)
{
    if (!m_ready || !m_passActive)
        return;
    vkCmdEndRenderPass(device.GetCommandBuffer());
    m_passActive = false;
}

void OffscreenSceneRenderer::SnapshotScene(VulkanDevice& device)
{
    if (!m_ready || !device.IsFrameActive() || m_passActive ||
        !m_sceneColorSnapshot || !m_sceneDepthSnapshot)
    {
        return;
    }

    VkCommandBuffer cmd = device.GetCommandBuffer();
    const VkImageAspectFlags depthAspect = VK_IMAGE_ASPECT_DEPTH_BIT;

    TransitionImage(cmd,
        m_colorImage,
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_ACCESS_TRANSFER_READ_BIT);
    TransitionImage(cmd,
        m_sceneColorSnapshot,
        VK_IMAGE_ASPECT_COLOR_BIT,
        m_snapshotsReady ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        m_snapshotsReady ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        m_snapshotsReady ? VK_ACCESS_SHADER_READ_BIT : 0,
        VK_ACCESS_TRANSFER_WRITE_BIT);

    VkImageCopy colorCopy{};
    colorCopy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    colorCopy.srcSubresource.layerCount = 1;
    colorCopy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    colorCopy.dstSubresource.layerCount = 1;
    colorCopy.extent = {m_extent.width, m_extent.height, 1};
    vkCmdCopyImage(cmd,
        m_colorImage,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        m_sceneColorSnapshot,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &colorCopy);

    TransitionImage(cmd,
        m_colorImage,
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_TRANSFER_READ_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT);
    TransitionImage(cmd,
        m_sceneColorSnapshot,
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT);

    TransitionImage(cmd,
        m_depthImage,
        depthAspect,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_ACCESS_TRANSFER_READ_BIT);
    TransitionImage(cmd,
        m_sceneDepthSnapshot,
        depthAspect,
        m_snapshotsReady ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        m_snapshotsReady ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        m_snapshotsReady ? VK_ACCESS_SHADER_READ_BIT : 0,
        VK_ACCESS_TRANSFER_WRITE_BIT);

    VkImageCopy depthCopy{};
    depthCopy.srcSubresource.aspectMask = depthAspect;
    depthCopy.srcSubresource.layerCount = 1;
    depthCopy.dstSubresource.aspectMask = depthAspect;
    depthCopy.dstSubresource.layerCount = 1;
    depthCopy.extent = {m_extent.width, m_extent.height, 1};
    vkCmdCopyImage(cmd,
        m_depthImage,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        m_sceneDepthSnapshot,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &depthCopy);

    TransitionImage(cmd,
        m_depthImage,
        depthAspect,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_ACCESS_TRANSFER_READ_BIT,
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    TransitionImage(cmd,
        m_sceneDepthSnapshot,
        depthAspect,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT);

    m_snapshotsReady = true;
}

void OffscreenSceneRenderer::RenderComposite(VulkanDevice& device)
{
    if (!m_ready || !device.IsFrameActive())
        return;

    VkCommandBuffer cmd = device.GetCommandBuffer();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdBindDescriptorSets(cmd,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        m_pipelineLayout,
        0,
        1,
        &m_descriptorSet,
        0,
        nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void OffscreenSceneRenderer::Destroy()
{
    if (!m_device)
        return;

    if (m_pipeline)
        vkDestroyPipeline(m_device, m_pipeline, nullptr);
    if (m_pipelineLayout)
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    if (m_descriptorPool)
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
    if (m_descriptorSetLayout)
        vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);
    if (m_sampler)
        vkDestroySampler(m_device, m_sampler, nullptr);
    if (m_framebuffer)
        vkDestroyFramebuffer(m_device, m_framebuffer, nullptr);
    if (m_depthView)
        vkDestroyImageView(m_device, m_depthView, nullptr);
    if (m_depthImage)
        vkDestroyImage(m_device, m_depthImage, nullptr);
    if (m_depthMemory)
        vkFreeMemory(m_device, m_depthMemory, nullptr);
    if (m_sceneDepthSnapshotView)
        vkDestroyImageView(m_device, m_sceneDepthSnapshotView, nullptr);
    if (m_sceneDepthSnapshot)
        vkDestroyImage(m_device, m_sceneDepthSnapshot, nullptr);
    if (m_sceneDepthSnapshotMemory)
        vkFreeMemory(m_device, m_sceneDepthSnapshotMemory, nullptr);
    if (m_colorView)
        vkDestroyImageView(m_device, m_colorView, nullptr);
    if (m_colorImage)
        vkDestroyImage(m_device, m_colorImage, nullptr);
    if (m_colorMemory)
        vkFreeMemory(m_device, m_colorMemory, nullptr);
    if (m_sceneColorSnapshotView)
        vkDestroyImageView(m_device, m_sceneColorSnapshotView, nullptr);
    if (m_sceneColorSnapshot)
        vkDestroyImage(m_device, m_sceneColorSnapshot, nullptr);
    if (m_sceneColorSnapshotMemory)
        vkFreeMemory(m_device, m_sceneColorSnapshotMemory, nullptr);
    if (m_renderPass)
        vkDestroyRenderPass(m_device, m_renderPass, nullptr);
    if (m_loadRenderPass)
        vkDestroyRenderPass(m_device, m_loadRenderPass, nullptr);

    m_renderPass = VK_NULL_HANDLE;
    m_colorImage = VK_NULL_HANDLE;
    m_colorMemory = VK_NULL_HANDLE;
    m_colorView = VK_NULL_HANDLE;
    m_depthImage = VK_NULL_HANDLE;
    m_depthMemory = VK_NULL_HANDLE;
    m_depthView = VK_NULL_HANDLE;
    m_sceneColorSnapshot = VK_NULL_HANDLE;
    m_sceneColorSnapshotMemory = VK_NULL_HANDLE;
    m_sceneColorSnapshotView = VK_NULL_HANDLE;
    m_sceneDepthSnapshot = VK_NULL_HANDLE;
    m_sceneDepthSnapshotMemory = VK_NULL_HANDLE;
    m_sceneDepthSnapshotView = VK_NULL_HANDLE;
    m_framebuffer = VK_NULL_HANDLE;
    m_sampler = VK_NULL_HANDLE;
    m_descriptorSetLayout = VK_NULL_HANDLE;
    m_descriptorPool = VK_NULL_HANDLE;
    m_descriptorSet = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;
    m_pipeline = VK_NULL_HANDLE;
    m_loadRenderPass = VK_NULL_HANDLE;
    m_ready = false;
    m_passActive = false;
    m_snapshotsReady = false;
}

bool OffscreenSceneRenderer::CreateRenderPass(VulkanDevice&)
{
    auto createPass = [&](bool clear, VkRenderPass& outPass) {
    VkAttachmentDescription color{};
    color.format = m_colorFormat;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = clear ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    color.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentDescription depth{};
    depth.format = m_depthFormat;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.stencilLoadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = clear ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

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
    subpass.pDepthStencilAttachment = &depthRef;

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

    VkAttachmentDescription attachments[] = {color, depth};
    VkRenderPassCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    create.attachmentCount = static_cast<std::uint32_t>(std::size(attachments));
    create.pAttachments = attachments;
    create.subpassCount = 1;
    create.pSubpasses = &subpass;
    create.dependencyCount = static_cast<std::uint32_t>(dependencies.size());
    create.pDependencies = dependencies.data();
    VK_CHECK(vkCreateRenderPass(m_device, &create, nullptr, &outPass));
    };

    createPass(true, m_renderPass);
    createPass(false, m_loadRenderPass);
    return true;
}

bool OffscreenSceneRenderer::CreateImages(VulkanDevice& device)
{
    VkImageCreateInfo color{};
    color.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    color.imageType = VK_IMAGE_TYPE_2D;
    color.extent = {m_extent.width, m_extent.height, 1};
    color.mipLevels = 1;
    color.arrayLayers = 1;
    color.format = m_colorFormat;
    color.tiling = VK_IMAGE_TILING_OPTIMAL;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateImage(m_device, &color, nullptr, &m_colorImage));

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(m_device, m_colorImage, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = device.FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(m_device, &alloc, nullptr, &m_colorMemory));
    VK_CHECK(vkBindImageMemory(m_device, m_colorImage, m_colorMemory, 0));

    VkImageViewCreateInfo colorView{};
    colorView.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    colorView.image = m_colorImage;
    colorView.viewType = VK_IMAGE_VIEW_TYPE_2D;
    colorView.format = m_colorFormat;
    colorView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    colorView.subresourceRange.baseMipLevel = 0;
    colorView.subresourceRange.levelCount = 1;
    colorView.subresourceRange.baseArrayLayer = 0;
    colorView.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(m_device, &colorView, nullptr, &m_colorView));

    VkImageCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    depth.imageType = VK_IMAGE_TYPE_2D;
    depth.extent = {m_extent.width, m_extent.height, 1};
    depth.mipLevels = 1;
    depth.arrayLayers = 1;
    depth.format = m_depthFormat;
    depth.tiling = VK_IMAGE_TILING_OPTIMAL;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateImage(m_device, &depth, nullptr, &m_depthImage));

    vkGetImageMemoryRequirements(m_device, m_depthImage, &req);
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = device.FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(m_device, &alloc, nullptr, &m_depthMemory));
    VK_CHECK(vkBindImageMemory(m_device, m_depthImage, m_depthMemory, 0));

    VkImageViewCreateInfo depthView{};
    depthView.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    depthView.image = m_depthImage;
    depthView.viewType = VK_IMAGE_VIEW_TYPE_2D;
    depthView.format = m_depthFormat;
    depthView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (HasStencil(m_depthFormat))
        depthView.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
    depthView.subresourceRange.baseMipLevel = 0;
    depthView.subresourceRange.levelCount = 1;
    depthView.subresourceRange.baseArrayLayer = 0;
    depthView.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(m_device, &depthView, nullptr, &m_depthView));

    VkImageCreateInfo colorSnapshot = color;
    colorSnapshot.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VK_CHECK(vkCreateImage(m_device, &colorSnapshot, nullptr, &m_sceneColorSnapshot));
    vkGetImageMemoryRequirements(m_device, m_sceneColorSnapshot, &req);
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = device.FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(m_device, &alloc, nullptr, &m_sceneColorSnapshotMemory));
    VK_CHECK(vkBindImageMemory(m_device, m_sceneColorSnapshot, m_sceneColorSnapshotMemory, 0));

    colorView.image = m_sceneColorSnapshot;
    VK_CHECK(vkCreateImageView(m_device, &colorView, nullptr, &m_sceneColorSnapshotView));

    VkImageCreateInfo depthSnapshot = depth;
    depthSnapshot.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VK_CHECK(vkCreateImage(m_device, &depthSnapshot, nullptr, &m_sceneDepthSnapshot));
    vkGetImageMemoryRequirements(m_device, m_sceneDepthSnapshot, &req);
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = device.FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(m_device, &alloc, nullptr, &m_sceneDepthSnapshotMemory));
    VK_CHECK(vkBindImageMemory(m_device, m_sceneDepthSnapshot, m_sceneDepthSnapshotMemory, 0));

    depthView.image = m_sceneDepthSnapshot;
    VK_CHECK(vkCreateImageView(m_device, &depthView, nullptr, &m_sceneDepthSnapshotView));
    return true;
}

bool OffscreenSceneRenderer::CreateFramebuffer(VulkanDevice&)
{
    VkImageView attachments[] = {m_colorView, m_depthView};
    VkFramebufferCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    create.renderPass = m_renderPass;
    create.attachmentCount = static_cast<std::uint32_t>(std::size(attachments));
    create.pAttachments = attachments;
    create.width = m_extent.width;
    create.height = m_extent.height;
    create.layers = 1;
    VK_CHECK(vkCreateFramebuffer(m_device, &create, nullptr, &m_framebuffer));
    return true;
}

bool OffscreenSceneRenderer::CreateSampler()
{
    VkSamplerCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    create.magFilter = VK_FILTER_LINEAR;
    create.minFilter = VK_FILTER_LINEAR;
    create.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    create.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    create.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    create.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    create.maxLod = 1.0f;
    VK_CHECK(vkCreateSampler(m_device, &create, nullptr, &m_sampler));
    return true;
}

bool OffscreenSceneRenderer::CreateDescriptorResources()
{
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout.bindingCount = 1;
    layout.pBindings = &binding;
    VK_CHECK(vkCreateDescriptorSetLayout(m_device, &layout, nullptr, &m_descriptorSetLayout));

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 1;
    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = 1;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &poolSize;
    VK_CHECK(vkCreateDescriptorPool(m_device, &pool, nullptr, &m_descriptorPool));

    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = m_descriptorPool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &m_descriptorSetLayout;
    VK_CHECK(vkAllocateDescriptorSets(m_device, &alloc, &m_descriptorSet));
    return true;
}

bool OffscreenSceneRenderer::CreatePipeline(VulkanDevice& device)
{
    VkShaderModule vs = CreateShaderModule(m_device, *m_assets, "assets/shaders/composite_vs.spv");
    VkShaderModule ps = CreateShaderModule(m_device, *m_assets, "assets/shaders/composite_ps.spv");

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "VSMain";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps;
    stages[1].pName = "PSMain";

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = VK_FALSE;
    depth.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;

    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(std::size(dynamicStates));
    dynamic.pDynamicStates = dynamicStates;

    VkPipelineLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &m_descriptorSetLayout;
    VK_CHECK(vkCreatePipelineLayout(m_device, &layout, nullptr, &m_pipelineLayout));

    VkGraphicsPipelineCreateInfo pipeline{};
    pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline.stageCount = 2;
    pipeline.pStages = stages;
    pipeline.pVertexInputState = &vertexInput;
    pipeline.pInputAssemblyState = &assembly;
    pipeline.pViewportState = &viewport;
    pipeline.pRasterizationState = &raster;
    pipeline.pMultisampleState = &multisample;
    pipeline.pDepthStencilState = &depth;
    pipeline.pColorBlendState = &blend;
    pipeline.pDynamicState = &dynamic;
    pipeline.layout = m_pipelineLayout;
    pipeline.renderPass = device.GetRenderPass();
    pipeline.subpass = 0;
    VK_CHECK(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &m_pipeline));

    vkDestroyShaderModule(m_device, ps, nullptr);
    vkDestroyShaderModule(m_device, vs, nullptr);
    return true;
}

void OffscreenSceneRenderer::UpdateDescriptor()
{
    VkDescriptorImageInfo image{};
    image.sampler = m_sampler;
    image.imageView = m_colorView;
    image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_descriptorSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
}
