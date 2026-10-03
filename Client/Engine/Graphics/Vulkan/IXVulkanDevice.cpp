// IXVulkanDevice implementation: Vulkan backend OWNING the IXRHI graphics
// frame contract (Phase 3C authority). The legacy VulkanDevice keeps
// device/queue/swapchain-handle infrastructure plus synced migration shims.

#include "IXVulkanDevice.h"

#include "IXVulkanBinding.h"
#include "IXVulkanCommandList.h"
#include "IXVulkanConversions.h"
#include "IXVulkanPipeline.h"
#include "IXVulkanRenderPass.h"
#include "IXVulkanResources.h"
#include "IXVulkanSwapchain.h"
#include "IXVulkanSync.h"
#include "VulkanDevice.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

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

std::unique_ptr<ixrhi::IXRHIDevice> CreateDevice(VulkanDevice& loop)
{
    return std::make_unique<IXVulkanDevice>(loop);
}

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

    m_swapchain = std::make_unique<IXVulkanSwapchain>(*this);
    if (m_loop->GetSwapchain() != VK_NULL_HANDLE)
    {
        m_swapchain->Rebuild();
        EnsureSwapchainObjects();
    }
    CreateTimestampPool();
}

IXVulkanDevice::~IXVulkanDevice()
{
    Shutdown(); // idempotent backstop; explicit Shutdown precedes device teardown
}

VkDevice IXVulkanDevice::NativeDevice() const
{
    return m_loop->GetDevice();
}

VkRenderPass IXVulkanDevice::ResolveRenderPass(const ixrhi::IXRHIRenderPass* pass) const
{
    if (pass != nullptr)
    {
        if (auto* native = dynamic_cast<const IXVulkanRenderPass*>(pass))
            return native->Native();
        return VK_NULL_HANDLE; // foreign implementation: no compatible pass known
    }
    // Backend-owned main pass (null while torn down → caller defers).
    return m_swapchain ? m_swapchain->NativeMainPass() : VK_NULL_HANDLE;
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

void IXVulkanDevice::CopyBufferSync(VkBuffer src, VkBuffer dst, VkDeviceSize size) const
{
    if (src == VK_NULL_HANDLE || dst == VK_NULL_HANDLE || size == 0)
        return;
    std::lock_guard<std::mutex> lock(m_uploadMutex);
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = m_uploadPool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    CheckVk(vkAllocateCommandBuffers(NativeDevice(), &alloc, &cmd),
        "vkAllocateCommandBuffers(copy)",
        __FILE__,
        __LINE__);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CheckVk(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer(copy)", __FILE__, __LINE__);
    VkBufferCopy region{};
    region.size = size;
    vkCmdCopyBuffer(cmd, src, dst, 1, &region);
    CheckVk(vkEndCommandBuffer(cmd), "vkEndCommandBuffer(copy)", __FILE__, __LINE__);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    // Parity with pre-migration staging uploads: submit + queue wait-idle.
    CheckVk(vkQueueSubmit(m_loop->GetGraphicsQueue(), 1, &submit, VK_NULL_HANDLE),
        "vkQueueSubmit(copy)",
        __FILE__,
        __LINE__);
    CheckVk(vkQueueWaitIdle(m_loop->GetGraphicsQueue()), "vkQueueWaitIdle(copy)", __FILE__, __LINE__);
    vkFreeCommandBuffers(NativeDevice(), m_uploadPool, 1, &cmd);
}

bool IXVulkanDevice::IsTextureFormatSupported(ixrhi::IXRHIFormat format,
                                              ixrhi::IXRHITextureUsage usage) const
{
    const VkFormat vkFormat = ToVkFormat(format);
    if (vkFormat == VK_FORMAT_UNDEFINED)
        return false;
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(m_loop->GetPhysicalDevice(), vkFormat, &props);
    VkFormatFeatureFlags required = 0;
    const auto bits = static_cast<std::uint32_t>(usage);
    using U = ixrhi::IXRHITextureUsage;
    if (bits & static_cast<std::uint32_t>(U::Sampled))
        required |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if (bits & static_cast<std::uint32_t>(U::TransferDst))
        required |= VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if (bits & static_cast<std::uint32_t>(U::TransferSrc))
        required |= VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    if (bits & static_cast<std::uint32_t>(U::ColorAttachment))
        required |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    if (bits & static_cast<std::uint32_t>(U::DepthStencilAttachment))
        required |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    return (props.optimalTilingFeatures & required) == required;
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
    if (initialDataOrNull != nullptr && initialBytes > 0)
    {
        if (hostVisible)
        {
            result->Write(0, initialDataOrNull, initialBytes);
        }
        else
        {
            // Device-local with payload (static vertex/index data): stage through
            // a host buffer and copy synchronously (parity with the old
            // CreateDeviceLocalBuffer setup path).
            const std::size_t bytes = initialBytes < desc.sizeBytes
                ? initialBytes
                : static_cast<std::size_t>(desc.sizeBytes);
            VkBuffer staging = VK_NULL_HANDLE;
            VkBufferCreateInfo stagingInfo{};
            stagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            stagingInfo.size = static_cast<VkDeviceSize>(bytes);
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
            std::memcpy(mapped, initialDataOrNull, bytes);
            vkUnmapMemory(NativeDevice(), stagingMemory);
            CopyBufferSync(staging, buffer, static_cast<VkDeviceSize>(bytes));
            vkDestroyBuffer(NativeDevice(), staging, nullptr);
            vkFreeMemory(NativeDevice(), stagingMemory, nullptr);
        }
    }
    return result;
}

std::shared_ptr<ixrhi::IXRHITexture> IXVulkanDevice::CreateTexture(
    const ixrhi::IXRHITextureDesc& desc, const void* initialDataOrNull, std::size_t initialBytes)
{
    if (desc.width == 0 || desc.height == 0 || desc.format == ixrhi::IXRHIFormat::Undefined ||
        desc.mipLevels == 0 || desc.arrayLayers == 0)
        return nullptr;
    if (desc.cubeMap && (desc.arrayLayers != 6 || desc.width != desc.height))
        return nullptr;

    const VkFormat format = ToVkFormat(desc.format);
    const std::uint32_t pixelBytes = ixrhi::IXRHIFormatByteSize(desc.format);
    if (format == VK_FORMAT_UNDEFINED || pixelBytes == 0)
        return nullptr;

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
    if (desc.cubeMap)
        create.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

    VkImage image = VK_NULL_HANDLE;
    CheckVk(vkCreateImage(NativeDevice(), &create, nullptr, &image), "vkCreateImage", __FILE__, __LINE__);

    VkDeviceMemory memory = VK_NULL_HANDLE;
    AllocateAndBind(image, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, memory);
    SetDebugName(VK_OBJECT_TYPE_IMAGE,
        reinterpret_cast<std::uint64_t>(image),
        desc.debugName.c_str());

    if (initialDataOrNull != nullptr && initialBytes > 0)
    {
        // Tight layer-major/mip-minor packing (IXRHITexture.h contract).
        std::vector<TextureCopyRegion> regions;
        regions.reserve(static_cast<std::size_t>(desc.arrayLayers) * desc.mipLevels);
        std::uint64_t offset = 0;
        for (std::uint32_t layer = 0; layer < desc.arrayLayers; ++layer)
        {
            std::uint32_t mipWidth = desc.width;
            std::uint32_t mipHeight = desc.height;
            for (std::uint32_t mip = 0; mip < desc.mipLevels; ++mip)
            {
                TextureCopyRegion region{};
                region.mipLevel = mip;
                region.baseArrayLayer = layer;
                region.width = mipWidth;
                region.height = mipHeight;
                region.bufferOffsetBytes = offset;
                regions.push_back(region);
                offset += static_cast<std::uint64_t>(mipWidth) * mipHeight * pixelBytes;
                mipWidth = std::max(1u, mipWidth >> 1u);
                mipHeight = std::max(1u, mipHeight >> 1u);
            }
        }
        UploadTextureRegions(image,
            desc.format,
            desc.mipLevels,
            desc.arrayLayers,
            regions.data(),
            regions.size(),
            initialDataOrNull,
            initialBytes,
            VK_IMAGE_LAYOUT_UNDEFINED,
            SampledReadLayout(desc.format));
    }

    const bool isDepth = (ToVkAspectMask(desc.format) & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = image;
    view.viewType = desc.cubeMap ? VK_IMAGE_VIEW_TYPE_CUBE
        : desc.arrayLayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                               : VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    // A depth/stencil texture that is only sampled (never an attachment, e.g. a scene depth snapshot)
    // gets a depth-only view: a sampled view must name a single aspect, and one with both returned
    // zero depth (D24S8 scene depth read as 0 by the water and god rays).
    const bool depthAttachment = (static_cast<std::uint32_t>(desc.usage) &
        static_cast<std::uint32_t>(ixrhi::IXRHITextureUsage::DepthStencilAttachment)) != 0;
    view.subresourceRange.aspectMask = !isDepth ? VK_IMAGE_ASPECT_COLOR_BIT
        : depthAttachment ? ToVkAspectMask(desc.format)
                          : VK_IMAGE_ASPECT_DEPTH_BIT;
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
        desc.mipLevels,
        desc.arrayLayers,
        desc.format,
        desc.debugName);
}

bool IXVulkanDevice::UpdateTexture(ixrhi::IXRHITexture& texture, const void* data, std::size_t byteCount)
{
    auto* native = dynamic_cast<IXVulkanTexture*>(&texture);
    if (native == nullptr || data == nullptr || byteCount == 0)
        return false;

    const std::uint32_t pixelBytes = ixrhi::IXRHIFormatByteSize(native->Format());
    if (pixelBytes == 0 || native->Width() == 0 || native->Height() == 0 || native->ArrayLayers() == 0)
        return false;

    // Full base-level rewrite across all layers (splat paint parity: the old
    // path rewrote the whole base level; higher mips untouched).
    const std::uint64_t needBytes = static_cast<std::uint64_t>(native->Width()) * native->Height() *
        pixelBytes * native->ArrayLayers();
    if (byteCount < needBytes)
        return false;

    std::vector<TextureCopyRegion> regions;
    regions.reserve(native->ArrayLayers());
    for (std::uint32_t layer = 0; layer < native->ArrayLayers(); ++layer)
    {
        TextureCopyRegion region{};
        region.mipLevel = 0;
        region.baseArrayLayer = layer;
        region.width = native->Width();
        region.height = native->Height();
        region.bufferOffsetBytes =
            static_cast<std::uint64_t>(layer) * native->Width() * native->Height() * pixelBytes;
        regions.push_back(region);
    }

    // Creation upload leaves sampled textures in the read layout, so the
    // rewrite brackets from/to the same layout (parity with the old
    // SHADER_READ_ONLY round-trip).
    const VkImageLayout readLayout = SampledReadLayout(native->Format());
    UploadTextureRegions(native->Native(),
        native->Format(),
        native->MipLevels(),
        native->ArrayLayers(),
        regions.data(),
        regions.size(),
        data,
        static_cast<std::size_t>(needBytes),
        readLayout,
        readLayout);
    return true;
}

VkImageLayout IXVulkanDevice::SampledReadLayout(ixrhi::IXRHIFormat format)
{
    // Depth sampled as a texture must use the depth-read layout; the color
    // SHADER_READ_ONLY layout is invalid for depth aspects (and was a latent
    // validation issue wherever depth snapshots are sampled).
    if ((ToVkAspectMask(format) & VK_IMAGE_ASPECT_DEPTH_BIT) != 0)
        return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void IXVulkanDevice::UploadTextureRegions(VkImage image,
                                         ixrhi::IXRHIFormat format,
                                         std::uint32_t mipLevels,
                                         std::uint32_t arrayLayers,
                                         const TextureCopyRegion* regions,
                                         std::size_t regionCount,
                                         const void* bytes,
                                         std::size_t byteCount,
                                         VkImageLayout initialLayout,
                                         VkImageLayout finalLayout) const
{
    const std::uint32_t pixelBytes = ixrhi::IXRHIFormatByteSize(format);
    if (bytes == nullptr || regionCount == 0 || regions == nullptr || pixelBytes == 0 || mipLevels == 0 ||
        arrayLayers == 0)
        LogAbort("IXVulkanDevice::UploadTextureRegions: bad payload");
    (void)pixelBytes;

    std::uint64_t needBytes = 0;
    std::vector<VkBufferImageCopy> copies;
    copies.reserve(regionCount);
    for (std::size_t i = 0; i < regionCount; ++i)
    {
        const TextureCopyRegion& region = regions[i];
        if (region.width == 0 || region.height == 0 || region.layerCount == 0)
            LogAbort("IXVulkanDevice::UploadTextureRegions: bad region");
        VkBufferImageCopy copy{};
        copy.bufferOffset = static_cast<VkDeviceSize>(region.bufferOffsetBytes);
        copy.imageSubresource.aspectMask = ToVkAspectMask(format);
        copy.imageSubresource.mipLevel = region.mipLevel;
        copy.imageSubresource.baseArrayLayer = region.baseArrayLayer;
        copy.imageSubresource.layerCount = region.layerCount;
        copy.imageExtent = {region.width, region.height, 1};
        copies.push_back(copy);
        const std::uint64_t end =
            region.bufferOffsetBytes + static_cast<std::uint64_t>(region.width) * region.height *
            region.layerCount * ixrhi::IXRHIFormatByteSize(format);
        needBytes = std::max(needBytes, end);
    }
    if (byteCount < needBytes)
        LogAbort("IXVulkanDevice::UploadTextureRegions: bad payload");

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
    std::memcpy(mapped, bytes, static_cast<std::size_t>(needBytes));
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
    toDst.oldLayout = initialLayout;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = image;
    toDst.subresourceRange.aspectMask = ToVkAspectMask(format);
    toDst.subresourceRange.levelCount = mipLevels;
    toDst.subresourceRange.layerCount = arrayLayers;
    toDst.srcAccessMask = initialLayout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_SHADER_READ_BIT;
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
        initialLayout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                                   : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &toDst);

    vkCmdCopyBufferToImage(cmd,
        staging,
        image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        static_cast<std::uint32_t>(copies.size()),
        copies.data());

    VkImageMemoryBarrier toRead = toDst;
    toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toRead.newLayout = finalLayout;
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

std::unique_ptr<ixrhi::IXRHIBufferUpload> IXVulkanDevice::UploadBufferAsync(
    const ixrhi::IXRHIBufferDesc& desc, const void* src, std::size_t byteCount)
{
    if (desc.sizeBytes == 0 || src == nullptr || byteCount == 0)
        return nullptr;
    return std::make_unique<IXVulkanBufferUpload>(*this, desc, src, byteCount);
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
    if (desc.addressU == ixrhi::IXRHISamplerAddress::ClampToBorder ||
        desc.addressV == ixrhi::IXRHISamplerAddress::ClampToBorder ||
        desc.addressW == ixrhi::IXRHISamplerAddress::ClampToBorder)
        create.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE; // shadow-map convention
    create.compareEnable = desc.compareEnable ? VK_TRUE : VK_FALSE;
    create.compareOp = ToVkCompareOp(desc.compareOp);
    create.mipLodBias = desc.mipLodBias;
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
