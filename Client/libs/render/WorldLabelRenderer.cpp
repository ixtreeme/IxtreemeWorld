#include "WorldLabelRenderer.h"

#include "Debug.h"
#include "asset/IAssetReader.h"

#if defined(_WIN32)
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
constexpr uint32_t kMaxVertices = 131072;
constexpr uint32_t kFirstGlyph = 32;
constexpr uint32_t kLastGlyph = 126;
constexpr uint32_t kGlyphCount = kLastGlyph - kFirstGlyph + 1;
constexpr uint32_t kAtlasCell = 32;
constexpr uint32_t kAtlasColumns = 16;
constexpr uint32_t kAtlasRows = 6;
constexpr uint32_t kAtlasWidth = kAtlasColumns * kAtlasCell;
constexpr uint32_t kAtlasHeight = kAtlasRows * kAtlasCell;
constexpr float kHeadOffsetMeters = 2.0f;
constexpr float kNamePixelScale = 0.013f;
constexpr float kOutlinePixels = 1.6f;
constexpr float kOutlineAlpha = 0.85f;
constexpr float kFadeStartMeters = 25.0f;
constexpr float kFadeEndMeters = 45.0f;
constexpr bool kDepthTestLabels = true;

const char* VkResultName(VkResult result)
{
    switch (result)
    {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
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
        Tracenf("[WORLD-LABEL] failed to open shader: %s", path.c_str());
        std::abort();
    }

    return std::vector<char>(bytes->begin(), bytes->end());
}

VkShaderModule CreateShaderModule(VkDevice device, client::asset::IAssetReader& assets,
    const std::string& path)
{
    const std::vector<char> code = ReadBinaryFile(assets, path);
    VkShaderModuleCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    create.codeSize = code.size();
    create.pCode = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule module = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(device, &create, nullptr, &module));
    return module;
}

bool CreateHostVisibleBuffer(VulkanDevice& device, VkDevice vkDevice, VkDeviceSize size,
    VkBufferUsageFlags usage, const void* initialData, WorldLabelRenderer::Buffer& out)
{
    VkBufferCreateInfo buffer{};
    buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer.size = size;
    buffer.usage = usage;
    buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(vkDevice, &buffer, nullptr, &out.buffer));

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(vkDevice, out.buffer, &req);

    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = device.FindMemoryType(req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(vkDevice, &alloc, nullptr, &out.memory));
    VK_CHECK(vkBindBufferMemory(vkDevice, out.buffer, out.memory, 0));

    if (initialData)
    {
        void* mapped = nullptr;
        VK_CHECK(vkMapMemory(vkDevice, out.memory, 0, size, 0, &mapped));
        std::memcpy(mapped, initialData, static_cast<size_t>(size));
        vkUnmapMemory(vkDevice, out.memory);
    }

    return true;
}

bool CreateDeviceLocalImage(VulkanDevice& device, VkDevice vkDevice, uint32_t width, uint32_t height,
    VkFormat format, VkImage& image, VkDeviceMemory& memory)
{
    VkImageCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    create.imageType = VK_IMAGE_TYPE_2D;
    create.format = format;
    create.extent = {width, height, 1};
    create.mipLevels = 1;
    create.arrayLayers = 1;
    create.samples = VK_SAMPLE_COUNT_1_BIT;
    create.tiling = VK_IMAGE_TILING_OPTIMAL;
    create.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(vkDevice, &create, nullptr, &image));

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(vkDevice, image, &req);

    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = device.FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(vkDevice, &alloc, nullptr, &memory));
    VK_CHECK(vkBindImageMemory(vkDevice, image, memory, 0));
    return true;
}

VkCommandBuffer BeginOneTimeCommands(VkDevice vkDevice, uint32_t queueFamily, VkCommandPool& pool)
{
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = queueFamily;
    VK_CHECK(vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &pool));

    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(vkDevice, &alloc, &cmd));

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &begin));
    return cmd;
}

void EndOneTimeCommands(VkDevice vkDevice, VkQueue queue, VkCommandPool pool, VkCommandBuffer cmd)
{
    VK_CHECK(vkEndCommandBuffer(cmd));

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    VK_CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(queue));
    vkDestroyCommandPool(vkDevice, pool, nullptr);
}

void TransitionImageLayout(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout)
{
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
        newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }
    else
    {
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    }

    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

std::string ToPrintableAscii(const std::string& text)
{
    std::string result;
    result.reserve(text.size());
    for (unsigned char c : text)
        result.push_back(c >= kFirstGlyph && c <= kLastGlyph ? static_cast<char>(c) : '?');
    return result;
}

WorldVec3 AddScaled(WorldVec3 origin, WorldVec3 right, float x, WorldVec3 up, float y)
{
    return {
        origin.x + right.x * x + up.x * y,
        origin.y + right.y * x + up.y * y,
        origin.z + right.z * x + up.z * y};
}
}

bool WorldLabelRenderer::Create(VulkanDevice& device, client::asset::IAssetReader& assets)
{
    Destroy();
    m_device = device.GetDevice();
    m_assets = &assets;

    const bool atlas = CreateFontAtlas(device);
    const bool buffers = atlas ? CreateBuffers(device) : false;
    const bool descriptors = buffers ? CreateDescriptors() : false;
    const bool pipeline = descriptors ? CreatePipeline(device) : false;
    Tracenf("[WORLD-LABEL] Create: atlas=%d buffers=%d descriptors=%d pipeline=%d glyphs=%u",
        atlas ? 1 : 0,
        buffers ? 1 : 0,
        descriptors ? 1 : 0,
        pipeline ? 1 : 0,
        kGlyphCount);

    if (atlas && buffers && descriptors && pipeline)
        return true;

    Destroy();
    return false;
}

bool WorldLabelRenderer::RecreatePipeline(VulkanDevice& device)
{
    if (!m_device)
        return true;

    DestroyPipeline();
    if (device.GetRenderPass() == VK_NULL_HANDLE)
        return true;

    return CreatePipeline(device);
}

void WorldLabelRenderer::Render(VulkanDevice& device, const WorldCamera& camera, const std::vector<Label>& worldLabels)
{
    if (!m_pipeline || !device.IsFrameActive() || worldLabels.empty())
        return;

    const VkExtent2D extent = device.GetSwapchainExtent();
    if (extent.width == 0 || extent.height == 0)
        return;

    const uint32_t frameIndex = device.GetFrameIndex();
    std::vector<Vertex> vertices;
    BuildVertices(camera, worldLabels, vertices);
    if (vertices.empty())
        return;
    if (vertices.size() > kMaxVertices)
        vertices.resize(kMaxVertices - (kMaxVertices % 6u));

    UpdateUniform(frameIndex, camera);

    void* mapped = nullptr;
    const VkDeviceSize vertexBytes = sizeof(Vertex) * vertices.size();
    VK_CHECK(vkMapMemory(m_device, m_vertexBuffers[frameIndex].memory, 0, vertexBytes, 0, &mapped));
    std::memcpy(mapped, vertices.data(), static_cast<size_t>(vertexBytes));
    vkUnmapMemory(m_device, m_vertexBuffers[frameIndex].memory);

    VkCommandBuffer cmd = device.GetCommandBuffer();
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m_vertexBuffers[frameIndex].buffer, &offset);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
        0, 1, &m_descriptorSets[frameIndex], 0, nullptr);
    vkCmdDraw(cmd, static_cast<uint32_t>(vertices.size()), 1, 0, 0);

    static bool loggedRender = false;
    if (!loggedRender)
    {
        Tracenf("[WORLD-LABEL] Render: worldLabels=%zu vertices=%zu depthTest=%d",
            worldLabels.size(),
            vertices.size(),
            kDepthTestLabels ? 1 : 0);
        loggedRender = true;
    }
}

void WorldLabelRenderer::Destroy()
{
    if (!m_device)
        return;

    DestroyPipeline();
    if (m_descriptorPool)
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
    m_descriptorPool = VK_NULL_HANDLE;
    if (m_descriptorSetLayout)
        vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);
    m_descriptorSetLayout = VK_NULL_HANDLE;

    for (Buffer& buffer : m_vertexBuffers)
        DestroyBuffer(buffer);
    for (Buffer& buffer : m_uniformBuffers)
        DestroyBuffer(buffer);
    DestroyTexture(m_fontAtlas);
    m_device = VK_NULL_HANDLE;
    m_assets = nullptr;
}

bool WorldLabelRenderer::CreateBuffers(VulkanDevice& device)
{
    for (Buffer& buffer : m_vertexBuffers)
    {
        CreateHostVisibleBuffer(device, m_device, sizeof(Vertex) * kMaxVertices,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, nullptr, buffer);
    }

    for (Buffer& buffer : m_uniformBuffers)
    {
        CreateHostVisibleBuffer(device, m_device, sizeof(UniformBlock),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, nullptr, buffer);
    }
    return true;
}

bool WorldLabelRenderer::CreateFontAtlas(VulkanDevice& device)
{
    DestroyTexture(m_fontAtlas);
    m_glyphs = {};

    std::vector<uint8_t> rgba(static_cast<size_t>(kAtlasWidth) * kAtlasHeight * 4u, 0);

#if defined(_WIN32)
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(kAtlasWidth);
    info.bmiHeader.biHeight = -static_cast<LONG>(kAtlasHeight);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC screenDc = GetDC(nullptr);
    HDC memoryDc = CreateCompatibleDC(screenDc);
    HBITMAP bitmap = CreateDIBSection(memoryDc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits)
    {
        if (bitmap)
            DeleteObject(bitmap);
        DeleteDC(memoryDc);
        ReleaseDC(nullptr, screenDc);
        return false;
    }

    HGDIOBJ oldBitmap = SelectObject(memoryDc, bitmap);
    HBRUSH blackBrush = CreateSolidBrush(RGB(0, 0, 0));
    RECT fullRect{0, 0, static_cast<LONG>(kAtlasWidth), static_cast<LONG>(kAtlasHeight)};
    FillRect(memoryDc, &fullRect, blackBrush);
    DeleteObject(blackBrush);

    HFONT font = CreateFontA(-22, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        FF_DONTCARE, "Segoe UI");
    HGDIOBJ oldFont = SelectObject(memoryDc, font);
    SetBkMode(memoryDc, TRANSPARENT);
    SetTextColor(memoryDc, RGB(255, 255, 255));

    for (uint32_t glyphIndex = 0; glyphIndex < kGlyphCount; ++glyphIndex)
    {
        const char ch = static_cast<char>(kFirstGlyph + glyphIndex);
        const uint32_t col = glyphIndex % kAtlasColumns;
        const uint32_t row = glyphIndex / kAtlasColumns;
        const int x = static_cast<int>(col * kAtlasCell + 2);
        const int y = static_cast<int>(row * kAtlasCell + 4);
        TextOutA(memoryDc, x, y, &ch, 1);

        SIZE size{};
        GetTextExtentPoint32A(memoryDc, &ch, 1, &size);

        Glyph& glyph = m_glyphs[static_cast<size_t>(ch)];
        glyph.u0 = static_cast<float>(col * kAtlasCell) / static_cast<float>(kAtlasWidth);
        glyph.v0 = static_cast<float>(row * kAtlasCell) / static_cast<float>(kAtlasHeight);
        glyph.u1 = static_cast<float>((col + 1u) * kAtlasCell) / static_cast<float>(kAtlasWidth);
        glyph.v1 = static_cast<float>((row + 1u) * kAtlasCell) / static_cast<float>(kAtlasHeight);
        glyph.width = static_cast<float>(kAtlasCell);
        glyph.height = static_cast<float>(kAtlasCell);
        glyph.advance = static_cast<float>(std::max<LONG>(size.cx + 2, 8));
    }

    SelectObject(memoryDc, oldFont);
    DeleteObject(font);

    const uint8_t* bgra = static_cast<const uint8_t*>(bits);
    for (uint32_t i = 0; i < kAtlasWidth * kAtlasHeight; ++i)
    {
        const uint8_t alpha = std::max(std::max(bgra[i * 4u + 0u], bgra[i * 4u + 1u]), bgra[i * 4u + 2u]);
        rgba[i * 4u + 0u] = 255;
        rgba[i * 4u + 1u] = 255;
        rgba[i * 4u + 2u] = 255;
        rgba[i * 4u + 3u] = alpha;
    }
    rgba[0] = 255;
    rgba[1] = 255;
    rgba[2] = 255;
    rgba[3] = 255;

    SelectObject(memoryDc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memoryDc);
    ReleaseDC(nullptr, screenDc);
#else
    for (uint32_t glyphIndex = 0; glyphIndex < kGlyphCount; ++glyphIndex)
    {
        const char ch = static_cast<char>(kFirstGlyph + glyphIndex);
        const uint32_t col = glyphIndex % kAtlasColumns;
        const uint32_t row = glyphIndex / kAtlasColumns;
        Glyph& glyph = m_glyphs[static_cast<size_t>(ch)];
        glyph.u0 = static_cast<float>(col * kAtlasCell) / static_cast<float>(kAtlasWidth);
        glyph.v0 = static_cast<float>(row * kAtlasCell) / static_cast<float>(kAtlasHeight);
        glyph.u1 = static_cast<float>((col + 1u) * kAtlasCell) / static_cast<float>(kAtlasWidth);
        glyph.v1 = static_cast<float>((row + 1u) * kAtlasCell) / static_cast<float>(kAtlasHeight);
        glyph.width = static_cast<float>(kAtlasCell);
        glyph.height = static_cast<float>(kAtlasCell);
        glyph.advance = 16.0f;

        for (uint32_t y = row * kAtlasCell + 6; y < row * kAtlasCell + kAtlasCell - 6; ++y)
        {
            for (uint32_t x = col * kAtlasCell + 6; x < col * kAtlasCell + kAtlasCell - 6; ++x)
            {
                const size_t index = (static_cast<size_t>(y) * kAtlasWidth + x) * 4u;
                rgba[index + 0] = 255;
                rgba[index + 1] = 255;
                rgba[index + 2] = 255;
                rgba[index + 3] = 180;
            }
        }
    }
#endif

    VkQueue graphicsQueue = VK_NULL_HANDLE;
    vkGetDeviceQueue(m_device, device.GetGraphicsQueueFamily(), 0, &graphicsQueue);

    CreateDeviceLocalImage(device, m_device, kAtlasWidth, kAtlasHeight,
        VK_FORMAT_R8G8B8A8_UNORM, m_fontAtlas.image, m_fontAtlas.memory);

    Buffer staging{};
    CreateHostVisibleBuffer(device, m_device, rgba.size(),
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT, rgba.data(), staging);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {kAtlasWidth, kAtlasHeight, 1};

    VkCommandPool uploadPool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = BeginOneTimeCommands(m_device, device.GetGraphicsQueueFamily(), uploadPool);
    TransitionImageLayout(cmd, m_fontAtlas.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(cmd, staging.buffer, m_fontAtlas.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    TransitionImageLayout(cmd, m_fontAtlas.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EndOneTimeCommands(m_device, graphicsQueue, uploadPool, cmd);
    DestroyBuffer(staging);

    m_fontAtlas.width = kAtlasWidth;
    m_fontAtlas.height = kAtlasHeight;

    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = m_fontAtlas.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = VK_FORMAT_R8G8B8A8_UNORM;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(m_device, &view, nullptr, &m_fontAtlas.view));

    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.maxLod = 1.0f;
    VK_CHECK(vkCreateSampler(m_device, &sampler, nullptr, &m_fontAtlas.sampler));

    return true;
}

bool WorldLabelRenderer::CreateDescriptors()
{
    VkDescriptorSetLayoutBinding ubo{};
    ubo.binding = 0;
    ubo.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    ubo.descriptorCount = 1;
    ubo.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutBinding atlas{};
    atlas.binding = 1;
    atlas.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    atlas.descriptorCount = 1;
    atlas.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    std::array<VkDescriptorSetLayoutBinding, 2> bindings = {ubo, atlas};
    VkDescriptorSetLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout.bindingCount = static_cast<uint32_t>(bindings.size());
    layout.pBindings = bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(m_device, &layout, nullptr, &m_descriptorSetLayout));

    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = kFramesInFlight;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = kFramesInFlight;

    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = kFramesInFlight;
    pool.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    pool.pPoolSizes = poolSizes.data();
    VK_CHECK(vkCreateDescriptorPool(m_device, &pool, nullptr, &m_descriptorPool));

    std::array<VkDescriptorSetLayout, kFramesInFlight> layouts{};
    layouts.fill(m_descriptorSetLayout);

    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = m_descriptorPool;
    alloc.descriptorSetCount = static_cast<uint32_t>(layouts.size());
    alloc.pSetLayouts = layouts.data();
    VK_CHECK(vkAllocateDescriptorSets(m_device, &alloc, m_descriptorSets.data()));

    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = m_uniformBuffers[frame].buffer;
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(UniformBlock);

        VkDescriptorImageInfo imageInfo{};
        imageInfo.sampler = m_fontAtlas.sampler;
        imageInfo.imageView = m_fontAtlas.view;
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        std::array<VkWriteDescriptorSet, 2> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = m_descriptorSets[frame];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &bufferInfo;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = m_descriptorSets[frame];
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }

    return true;
}

bool WorldLabelRenderer::CreatePipeline(VulkanDevice& device)
{
    if (!m_assets)
        return false;

    VkShaderModule vs = CreateShaderModule(m_device, *m_assets, "assets/shaders/world_label_vs.spv");
    VkShaderModule ps = CreateShaderModule(m_device, *m_assets, "assets/shaders/world_label_ps.spv");

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "VSMain";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps;
    stages[1].pName = "PSMain";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attributes[3]{};
    attributes[0].location = 0;
    attributes[0].binding = 0;
    attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[0].offset = offsetof(Vertex, position);
    attributes[1].location = 1;
    attributes[1].binding = 0;
    attributes[1].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[1].offset = offsetof(Vertex, uv);
    attributes[2].location = 2;
    attributes[2].binding = 0;
    attributes[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attributes[2].offset = offsetof(Vertex, color);

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 3;
    vertexInput.pVertexAttributeDescriptions = attributes;

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
    depth.depthTestEnable = kDepthTestLabels ? VK_TRUE : VK_FALSE;
    depth.depthWriteEnable = VK_FALSE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;

    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
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

void WorldLabelRenderer::DestroyPipeline()
{
    if (m_pipeline)
        vkDestroyPipeline(m_device, m_pipeline, nullptr);
    m_pipeline = VK_NULL_HANDLE;

    if (m_pipelineLayout)
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    m_pipelineLayout = VK_NULL_HANDLE;
}

void WorldLabelRenderer::DestroyBuffer(Buffer& buffer)
{
    if (buffer.buffer)
        vkDestroyBuffer(m_device, buffer.buffer, nullptr);
    if (buffer.memory)
        vkFreeMemory(m_device, buffer.memory, nullptr);
    buffer = {};
}

void WorldLabelRenderer::DestroyTexture(Texture& texture)
{
    if (texture.sampler)
        vkDestroySampler(m_device, texture.sampler, nullptr);
    if (texture.view)
        vkDestroyImageView(m_device, texture.view, nullptr);
    if (texture.image)
        vkDestroyImage(m_device, texture.image, nullptr);
    if (texture.memory)
        vkFreeMemory(m_device, texture.memory, nullptr);
    texture = {};
}

void WorldLabelRenderer::UpdateUniform(uint32_t frameIndex, const WorldCamera& camera)
{
    const UniformBlock uniform{camera.viewProjection};
    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(m_device, m_uniformBuffers[frameIndex].memory, 0, sizeof(uniform), 0, &mapped));
    std::memcpy(mapped, &uniform, sizeof(uniform));
    vkUnmapMemory(m_device, m_uniformBuffers[frameIndex].memory);
}

void WorldLabelRenderer::BuildVertices(const WorldCamera& camera, const std::vector<Label>& worldLabels, std::vector<Vertex>& vertices) const
{
    const WorldVec3 forward = WorldNormalize(WorldSub(camera.target, camera.eye));
    WorldVec3 right = WorldNormalize(WorldCross({0.0f, 1.0f, 0.0f}, forward));
    if (WorldDot(right, right) <= 0.000001f)
        right = {1.0f, 0.0f, 0.0f};
    const WorldVec3 up = WorldNormalize(WorldCross(forward, right));

    vertices.reserve(std::min<size_t>(kMaxVertices, worldLabels.size() * 96u * 6u * 2u));
    for (const Label& label : worldLabels)
    {
        const WorldVec3 toCamera = WorldSub(label.position, camera.eye);
        const float distance = std::sqrt(WorldDot(toCamera, toCamera));
        const float fade = std::clamp(
            (kFadeEndMeters - distance) / (kFadeEndMeters - kFadeStartMeters),
            0.0f,
            1.0f);
        if (fade <= 0.0f)
            continue;

        const std::string text = ToPrintableAscii(label.text.empty() ? std::string("Label") : label.text);

        WorldVec3 origin = WorldAdd(label.position, {0.0f, kHeadOffsetMeters, 0.0f});
        const float nameLineHeight = kAtlasCell * kNamePixelScale;
        origin = WorldAdd(origin, WorldScale(up, nameLineHeight * 0.5f));

        float textColor[4] = {label.color[0], label.color[1], label.color[2], label.color[3]};
        if (label.selected)
        {
            const float selectColor[4] = {1.0f, 0.80f, 0.22f, 0.30f};
            AppendQuad(vertices, origin, right, up, 2.2f, 0.42f, selectColor, fade);
            textColor[0] = 1.0f;
            textColor[1] = 0.86f;
            textColor[2] = 0.32f;
            textColor[3] = 1.0f;
        }
        AppendLine(vertices, origin, right, up, text, kNamePixelScale, textColor, fade);
    }
}

void WorldLabelRenderer::AppendQuad(std::vector<Vertex>& vertices, WorldVec3 origin, WorldVec3 right, WorldVec3 up,
    float width, float height, const float color[4], float fade) const
{
    if (width <= 0.0f || height <= 0.0f || vertices.size() + 6u >= kMaxVertices)
        return;

    const float halfW = width * 0.5f;
    const float halfH = height * 0.5f;
    const WorldVec3 p0 = AddScaled(origin, right, -halfW, up, halfH);
    const WorldVec3 p1 = AddScaled(origin, right, halfW, up, halfH);
    const WorldVec3 p2 = AddScaled(origin, right, halfW, up, -halfH);
    const WorldVec3 p3 = AddScaled(origin, right, -halfW, up, -halfH);
    const float u = 0.5f / static_cast<float>(kAtlasWidth);
    const float v = 0.5f / static_cast<float>(kAtlasHeight);

    auto makeVertex = [&](WorldVec3 p)
    {
        Vertex vertex{};
        vertex.position[0] = p.x;
        vertex.position[1] = p.y;
        vertex.position[2] = p.z;
        vertex.uv[0] = u;
        vertex.uv[1] = v;
        vertex.color[0] = color[0];
        vertex.color[1] = color[1];
        vertex.color[2] = color[2];
        vertex.color[3] = color[3] * fade;
        return vertex;
    };

    vertices.push_back(makeVertex(p0));
    vertices.push_back(makeVertex(p1));
    vertices.push_back(makeVertex(p2));
    vertices.push_back(makeVertex(p0));
    vertices.push_back(makeVertex(p2));
    vertices.push_back(makeVertex(p3));
}

void WorldLabelRenderer::AppendLine(std::vector<Vertex>& vertices, WorldVec3 origin, WorldVec3 right, WorldVec3 up,
    const std::string& text, float pixelScale, const float color[4], float fade) const
{
    if (text.empty() || vertices.size() + 6u >= kMaxVertices)
        return;

    float textWidth = 0.0f;
    for (unsigned char c : text)
    {
        const char glyphChar = c >= kFirstGlyph && c <= kLastGlyph ? static_cast<char>(c) : '?';
        textWidth += m_glyphs[static_cast<size_t>(glyphChar)].advance * pixelScale;
    }

    auto appendPass = [&](float offsetX, float offsetY, const float passColor[4])
    {
        float penX = -textWidth * 0.5f + offsetX;
        for (unsigned char c : text)
        {
            const char glyphChar = c >= kFirstGlyph && c <= kLastGlyph ? static_cast<char>(c) : '?';
            const Glyph& glyph = m_glyphs[static_cast<size_t>(glyphChar)];
            const float x0 = penX;
            const float x1 = penX + glyph.width * pixelScale;
            const float y0 = offsetY;
            const float y1 = offsetY - glyph.height * pixelScale;

            const WorldVec3 p0 = AddScaled(origin, right, x0, up, y0);
            const WorldVec3 p1 = AddScaled(origin, right, x1, up, y0);
            const WorldVec3 p2 = AddScaled(origin, right, x1, up, y1);
            const WorldVec3 p3 = AddScaled(origin, right, x0, up, y1);

            auto makeVertex = [&](WorldVec3 p, float u, float v)
            {
                Vertex vertex{};
                vertex.position[0] = p.x;
                vertex.position[1] = p.y;
                vertex.position[2] = p.z;
                vertex.uv[0] = u;
                vertex.uv[1] = v;
                vertex.color[0] = passColor[0];
                vertex.color[1] = passColor[1];
                vertex.color[2] = passColor[2];
                vertex.color[3] = passColor[3];
                return vertex;
            };

            if (vertices.size() + 6u >= kMaxVertices)
                return;

            vertices.push_back(makeVertex(p0, glyph.u0, glyph.v0));
            vertices.push_back(makeVertex(p1, glyph.u1, glyph.v0));
            vertices.push_back(makeVertex(p2, glyph.u1, glyph.v1));
            vertices.push_back(makeVertex(p0, glyph.u0, glyph.v0));
            vertices.push_back(makeVertex(p2, glyph.u1, glyph.v1));
            vertices.push_back(makeVertex(p3, glyph.u0, glyph.v1));
            penX += glyph.advance * pixelScale;
        }
    };

    const float outlineColor[4] = {0.0f, 0.0f, 0.0f, kOutlineAlpha * fade};
    const float fillColor[4] = {color[0], color[1], color[2], color[3] * fade};
    const float diagonal = 0.70710678f;
    const float outlineOffset = kOutlinePixels * pixelScale;
    const float offsets[8][2] =
    {
        { 1.0f, 0.0f},
        {-1.0f, 0.0f},
        { 0.0f, 1.0f},
        { 0.0f,-1.0f},
        { diagonal, diagonal},
        {-diagonal, diagonal},
        { diagonal,-diagonal},
        {-diagonal,-diagonal},
    };

    for (const float (&offset)[2] : offsets)
        appendPass(offset[0] * outlineOffset, offset[1] * outlineOffset, outlineColor);
    appendPass(0.0f, 0.0f, fillColor);
}
