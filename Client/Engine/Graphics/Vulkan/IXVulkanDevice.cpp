// IXVulkanDevice implementation: strangler adapter over the live VulkanDevice.

#include "IXVulkanDevice.h"

#include "IXVulkanBinding.h"
#include "IXVulkanCommandList.h"
#include "IXVulkanConversions.h"
#include "IXVulkanPipeline.h"
#include "IXVulkanResources.h"
#include "IXVulkanSync.h"
#include "VulkanDevice.h"

#include <cstdlib>
#include <cstring>

namespace ixvulkan
{
namespace
{

void LogAbort(const char* message)
{
    // Same policy as the pre-migration renderers: log + abort. Engine logging
    // (Debug.h) is linked via IXEnginePlatform's IXEngineDebug dependency.
    (void)message;
    std::abort();
}

} // namespace

IXVulkanDevice::IXVulkanDevice(VulkanDevice& loop) : m_loop(&loop)
{
    auto proc = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
        vkGetDeviceProcAddr(NativeDevice(), "vkSetDebugUtilsObjectNameEXT"));
    m_setDebugName = proc;

    VkCommandPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool.queueFamilyIndex = m_loop->GetGraphicsQueueFamily();
    CheckVk(vkCreateCommandPool(NativeDevice(), &pool, nullptr, &m_uploadPool),
        "vkCreateCommandPool(upload)",
        __FILE__,
        __LINE__);

    QueryCapabilities();
}

IXVulkanDevice::~IXVulkanDevice()
{
    if (m_uploadPool != VK_NULL_HANDLE)
        vkDestroyCommandPool(NativeDevice(), m_uploadPool, nullptr);
}

VkDevice IXVulkanDevice::NativeDevice() const
{
    return m_loop->GetDevice();
}

VkRenderPass IXVulkanDevice::ResolveRenderPass() const
{
    if (m_pipelineRenderPassOverride != VK_NULL_HANDLE)
        return m_pipelineRenderPassOverride;
    return m_loop->GetRenderPass();
}

void IXVulkanDevice::SetPipelineRenderPass(VkRenderPass pass)
{
    m_pipelineRenderPassOverride = pass;
}

void IXVulkanDevice::SetDebugName(VkObjectType type, std::uint64_t handle, const char* name) const
{
    if (m_setDebugName == nullptr || handle == 0 || name == nullptr || name[0] == '\0')
        return;
    VkDebugUtilsObjectNameInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.objectType = type;
    info.objectHandle = handle;
    info.pObjectName = name;
    m_setDebugName(NativeDevice(), &info);
}

void IXVulkanDevice::CheckVk(VkResult result, const char* call, const char* file, int line) const
{
    if (result == VK_SUCCESS)
        return;
    (void)file;
    (void)line;
    (void)call;
    LogAbort("Vulkan call failed inside IXVulkan backend");
}

std::uint32_t IXVulkanDevice::FindMemoryType(std::uint32_t typeBits,
                                             VkMemoryPropertyFlags props) const
{
    return m_loop->FindMemoryType(typeBits, props);
}

void IXVulkanDevice::AllocateAndBind(VkBuffer buffer,
                                     VkMemoryPropertyFlags props,
                                     VkDeviceMemory& out) const
{
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(NativeDevice(), buffer, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, props);
    CheckVk(vkAllocateMemory(NativeDevice(), &alloc, nullptr, &out),
        "vkAllocateMemory(buffer)",
        __FILE__,
        __LINE__);
    CheckVk(vkBindBufferMemory(NativeDevice(), buffer, out, 0), "vkBindBufferMemory", __FILE__, __LINE__);
}

void IXVulkanDevice::AllocateAndBind(VkImage image,
                                     VkMemoryPropertyFlags props,
                                     VkDeviceMemory& out) const
{
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(NativeDevice(), image, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, props);
    CheckVk(vkAllocateMemory(NativeDevice(), &alloc, nullptr, &out),
        "vkAllocateMemory(image)",
        __FILE__,
        __LINE__);
    CheckVk(vkBindImageMemory(NativeDevice(), image, out, 0), "vkBindImageMemory", __FILE__, __LINE__);
}

void IXVulkanDevice::QueryCapabilities()
{
    const VkPhysicalDevice physical = m_loop->GetPhysicalDevice();
    m_capabilities = ixrhi::IXRHICapabilities{};
    if (physical == VK_NULL_HANDLE)
        return;

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical, &props);
    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(physical, &features);

    m_capabilities.deviceName = props.deviceName;
    m_capabilities.discreteGpu = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    m_capabilities.maxTextureSize = props.limits.maxImageDimension2D;
    m_capabilities.maxAnisotropy =
        m_loop->SupportsSamplerAnisotropy()
            ? static_cast<std::uint32_t>(m_loop->GetMaxSamplerAnisotropy())
            : 1;
    m_capabilities.supportsAnisotropy = m_loop->SupportsSamplerAnisotropy();
    m_capabilities.supportsCompute = true; // single universal queue in VulkanDevice
    m_capabilities.supportsIndirectDraw = true; // core drawIndirect
    m_capabilities.supportsTimestampQueries = props.limits.timestampComputeAndGraphics != 0;
    m_capabilities.supportsDynamicRendering = false; // VkRenderPass infra (Phase 2)
    m_capabilities.supportsTimelineSemaphores = false; // unused by renderer; enable with use
    m_capabilities.supportsMultiDrawIndirect =
        features.multiDrawIndirect != 0; // queried, never assumed
    m_capabilities.supportsAsyncCompute = false; // single universal queue in VulkanDevice
    m_capabilities.supportsBindless = false; // descriptor indexing unused; enable with use
    m_capabilities.supportsRayQuery = false;
    m_capabilities.supportsMeshShaders = false;
    m_capabilities.supportsRayTracing = false;
    m_capabilities.supportsVariableRateShading = false;
    (void)features;
}

std::shared_ptr<ixrhi::IXRHIBuffer> IXVulkanDevice::CreateBuffer(
    const ixrhi::IXRHIBufferDesc& desc, const void* initialDataOrNull, std::size_t initialBytes)
{
    if (desc.sizeBytes == 0)
        return nullptr;

    VkBufferCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    create.size = static_cast<VkDeviceSize>(desc.sizeBytes);
    create.usage = ToVkBufferUsage(desc.usage);
    if (initialDataOrNull != nullptr && initialBytes > 0)
        create.usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer buffer = VK_NULL_HANDLE;
    CheckVk(vkCreateBuffer(NativeDevice(), &create, nullptr, &buffer),
        "vkCreateBuffer",
        __FILE__,
        __LINE__);

    VkDeviceMemory memory = VK_NULL_HANDLE;
    const bool hostVisible = desc.cpuAccess == ixrhi::IXRHICpuAccess::Write;
    AllocateAndBind(buffer,
        hostVisible ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                    : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        memory);
    SetDebugName(VK_OBJECT_TYPE_BUFFER,
        reinterpret_cast<std::uint64_t>(buffer),
        desc.debugName.c_str());

    auto result = std::make_shared<IXVulkanBuffer>(*this,
        buffer,
        memory,
        desc.sizeBytes,
        desc.usage,
        desc.debugName);
    if (initialDataOrNull != nullptr && initialBytes > 0 && hostVisible)
        result->Write(0, initialDataOrNull, initialBytes);
    return result;
}

std::shared_ptr<ixrhi::IXRHITexture> IXVulkanDevice::CreateTexture(
    const ixrhi::IXRHITextureDesc& desc, const void* initialDataOrNull, std::size_t initialBytes)
{
    if (desc.width == 0 || desc.height == 0 || desc.format == ixrhi::IXRHIFormat::Undefined)
        return nullptr;

    const VkFormat format = ToVkFormat(desc.format);
    VkImageCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    create.imageType = VK_IMAGE_TYPE_2D;
    create.format = format;
    create.extent = {desc.width, desc.height, 1};
    create.mipLevels = desc.mipLevels;
    create.arrayLayers = desc.arrayLayers;
    create.samples = ToVkSampleCount(desc.sampleCount);
    create.tiling = VK_IMAGE_TILING_OPTIMAL;
    create.usage = ToVkImageUsage(desc.usage);
    if (initialDataOrNull != nullptr && initialBytes > 0)
        create.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage image = VK_NULL_HANDLE;
    CheckVk(vkCreateImage(NativeDevice(), &create, nullptr, &image), "vkCreateImage", __FILE__, __LINE__);

    VkDeviceMemory memory = VK_NULL_HANDLE;
    AllocateAndBind(image, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, memory);
    SetDebugName(VK_OBJECT_TYPE_IMAGE,
        reinterpret_cast<std::uint64_t>(image),
        desc.debugName.c_str());

    if (initialDataOrNull != nullptr && initialBytes > 0)
        UploadTextureBytes(image, desc.width, desc.height, desc.format, initialDataOrNull, initialBytes);
    else
    {
        // Render-target/depth textures reach their layout via the render pass;
        // sampled textures without data are invalid use — flag early in debug.
        (void)0;
    }

    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange.aspectMask =
        ixrhi::HasUsage(desc.usage, ixrhi::IXRHITextureUsage::DepthStencilAttachment)
            ? VK_IMAGE_ASPECT_DEPTH_BIT
            : VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = desc.mipLevels;
    view.subresourceRange.layerCount = desc.arrayLayers;
    VkImageView imageView = VK_NULL_HANDLE;
    CheckVk(vkCreateImageView(NativeDevice(), &view, nullptr, &imageView),
        "vkCreateImageView",
        __FILE__,
        __LINE__);

    return std::make_shared<IXVulkanTexture>(*this,
        image,
        memory,
        imageView,
        desc.width,
        desc.height,
        desc.format,
        desc.debugName);
}

void IXVulkanDevice::UploadTextureBytes(VkImage image,
                                        std::uint32_t width,
                                        std::uint32_t height,
                                        ixrhi::IXRHIFormat format,
                                        const void* bytes,
                                        std::size_t byteCount) const
{
    const std::uint32_t pixelBytes = ixrhi::IXRHIFormatByteSize(format);
    const std::size_t needBytes = static_cast<std::size_t>(width) * height * pixelBytes;
    if (bytes == nullptr || byteCount < needBytes || pixelBytes == 0)
        LogAbort("IXVulkanDevice::UploadTextureBytes: bad payload");

    // Staging buffer (same host-visible pattern as pre-migration upload code).
    VkBuffer staging = VK_NULL_HANDLE;
    VkBufferCreateInfo stagingInfo{};
    stagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    stagingInfo.size = static_cast<VkDeviceSize>(needBytes);
    stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    CheckVk(vkCreateBuffer(NativeDevice(), &stagingInfo, nullptr, &staging),
        "vkCreateBuffer(staging)",
        __FILE__,
        __LINE__);
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    AllocateAndBind(staging,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        stagingMemory);
    void* mapped = nullptr;
    CheckVk(vkMapMemory(NativeDevice(), stagingMemory, 0, stagingInfo.size, 0, &mapped),
        "vkMapMemory(staging)",
        __FILE__,
        __LINE__);
    std::memcpy(mapped, bytes, needBytes);
    vkUnmapMemory(NativeDevice(), stagingMemory);

    std::lock_guard<std::mutex> lock(m_uploadMutex);
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = m_uploadPool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    CheckVk(vkAllocateCommandBuffers(NativeDevice(), &alloc, &cmd),
        "vkAllocateCommandBuffers(upload)",
        __FILE__,
        __LINE__);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CheckVk(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer(upload)", __FILE__, __LINE__);

    VkImageMemoryBarrier toDst{};
    toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = image;
    toDst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toDst.subresourceRange.levelCount = 1;
    toDst.subresourceRange.layerCount = 1;
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &toDst);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier toRead = toDst;
    toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &toRead);

    CheckVk(vkEndCommandBuffer(cmd), "vkEndCommandBuffer(upload)", __FILE__, __LINE__);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    // Parity with pre-migration upload: submit + queue wait-idle (setup-time only).
    CheckVk(vkQueueSubmit(m_loop->GetGraphicsQueue(), 1, &submit, VK_NULL_HANDLE),
        "vkQueueSubmit(upload)",
        __FILE__,
        __LINE__);
    CheckVk(vkQueueWaitIdle(m_loop->GetGraphicsQueue()), "vkQueueWaitIdle(upload)", __FILE__, __LINE__);
    vkFreeCommandBuffers(NativeDevice(), m_uploadPool, 1, &cmd);

    vkDestroyBuffer(NativeDevice(), staging, nullptr);
    vkFreeMemory(NativeDevice(), stagingMemory, nullptr);
}

std::shared_ptr<ixrhi::IXRHISampler> IXVulkanDevice::CreateSampler(
    const ixrhi::IXRHISamplerDesc& desc)
{
    VkSamplerCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    create.magFilter = ToVkFilter(desc.magFilter);
    create.minFilter = ToVkFilter(desc.minFilter);
    create.mipmapMode = desc.mipmapFilter == ixrhi::IXRHISamplerFilter::Nearest
        ? VK_SAMPLER_MIPMAP_MODE_NEAREST
        : VK_SAMPLER_MIPMAP_MODE_LINEAR;
    create.addressModeU = ToVkAddressMode(desc.addressU);
    create.addressModeV = ToVkAddressMode(desc.addressV);
    create.addressModeW = ToVkAddressMode(desc.addressW);
    create.maxAnisotropy = desc.maxAnisotropy > 1 && m_capabilities.supportsAnisotropy
        ? static_cast<float>(desc.maxAnisotropy)
        : 1.0f;
    create.anisotropyEnable = (create.maxAnisotropy > 1.0f) ? VK_TRUE : VK_FALSE;
    create.maxLod = desc.maxLod;

    VkSampler sampler = VK_NULL_HANDLE;
    CheckVk(vkCreateSampler(NativeDevice(), &create, nullptr, &sampler),
        "vkCreateSampler",
        __FILE__,
        __LINE__);
    SetDebugName(VK_OBJECT_TYPE_SAMPLER,
        reinterpret_cast<std::uint64_t>(sampler),
        desc.debugName.c_str());
    return std::make_shared<IXVulkanSampler>(*this, sampler, desc.debugName);
}

std::shared_ptr<ixrhi::IXRHIShader> IXVulkanDevice::CreateShader(
    const ixrhi::IXRHIShaderDesc& desc)
{
    if (desc.spirv.empty() || desc.entryPoint.empty())
        return nullptr;
    VkShaderModuleCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    create.codeSize = desc.spirv.size() * sizeof(std::uint32_t);
    create.pCode = desc.spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    CheckVk(vkCreateShaderModule(NativeDevice(), &create, nullptr, &module),
        "vkCreateShaderModule",
        __FILE__,
        __LINE__);
    SetDebugName(VK_OBJECT_TYPE_SHADER_MODULE,
        reinterpret_cast<std::uint64_t>(module),
        desc.debugName.c_str());
    return std::make_shared<IXVulkanShader>(*this, module, desc.stage, desc.entryPoint, desc.debugName);
}

} // namespace ixvulkan
