#include "TerrainRenderer.h"

#include "Debug.h"
#include "asset/IAssetReader.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
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
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    default: return "VK_RESULT_UNKNOWN";
    }
}

void CheckVk(VkResult result, const char* call, const char* file, int line)
{
    if (result == VK_SUCCESS)
        return;

    char buffer[512];
    std::snprintf(buffer, sizeof(buffer), "%s:%d: Vulkan call failed: %s -> %s (%d)",
        file, line, call, VkResultName(result), result);
    Tracen(buffer);
    std::abort();
}

#define VK_CHECK(call) CheckVk((call), #call, __FILE__, __LINE__)

std::vector<char> ReadBinaryFile(client::asset::IAssetReader& assets, const std::string& path)
{
    auto bytes = assets.ReadAll(path);
    if (!bytes)
    {
        const std::string message = "Failed to open shader: " + path;
        Tracen(message.c_str());
        std::abort();
    }

    return std::vector<char>(bytes->begin(), bytes->end());
}

uint32_t MakeFourCC(char a, char b, char c, char d)
{
    return static_cast<uint32_t>(a) |
        (static_cast<uint32_t>(b) << 8) |
        (static_cast<uint32_t>(c) << 16) |
        (static_cast<uint32_t>(d) << 24);
}

const char* VkFormatName(VkFormat format)
{
    switch (format)
    {
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return "VK_FORMAT_BC1_RGBA_SRGB_BLOCK";
    case VK_FORMAT_BC2_SRGB_BLOCK: return "VK_FORMAT_BC2_SRGB_BLOCK";
    case VK_FORMAT_BC3_SRGB_BLOCK: return "VK_FORMAT_BC3_SRGB_BLOCK";
    case VK_FORMAT_R8G8B8A8_SRGB: return "VK_FORMAT_R8G8B8A8_SRGB";
    case VK_FORMAT_R8_UNORM: return "VK_FORMAT_R8_UNORM";
    default: return "VK_FORMAT_UNDEFINED";
    }
}

#pragma pack(push, 1)
struct DdsPixelFormat
{
    uint32_t size;
    uint32_t flags;
    uint32_t fourCC;
    uint32_t rgbBitCount;
    uint32_t rBitMask;
    uint32_t gBitMask;
    uint32_t bBitMask;
    uint32_t aBitMask;
};

struct DdsHeader
{
    uint32_t size;
    uint32_t flags;
    uint32_t height;
    uint32_t width;
    uint32_t pitchOrLinearSize;
    uint32_t depth;
    uint32_t mipMapCount;
    uint32_t reserved1[11];
    DdsPixelFormat pixelFormat;
    uint32_t caps;
    uint32_t caps2;
    uint32_t caps3;
    uint32_t caps4;
    uint32_t reserved2;
};
#pragma pack(pop)

struct DdsImage
{
    std::string filename;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 0;
    uint32_t blockBytes = 0;
    uint32_t bytesPerPixel = 0;
    bool compressed = false;
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::vector<uint8_t> pixels;
    std::vector<VkBufferImageCopy> regions;
};

struct TextureSetEntry
{
    std::string path;
    float scaleU = 1.0f;
    float scaleV = 1.0f;
};

bool LoadDdsImage(client::asset::IAssetReader& assets, const std::string& path, DdsImage& out)
{
    auto maybeBytes = assets.ReadAll(path);
    if (!maybeBytes)
    {
        Tracenf("[TERRAIN-TEX] failed to open DDS: %s", path.c_str());
        return false;
    }

    std::vector<uint8_t> bytes = std::move(*maybeBytes);

    if (bytes.size() < sizeof(uint32_t) + sizeof(DdsHeader))
        return false;

    const uint32_t magic = static_cast<uint32_t>(bytes[0]) |
        (static_cast<uint32_t>(bytes[1]) << 8) |
        (static_cast<uint32_t>(bytes[2]) << 16) |
        (static_cast<uint32_t>(bytes[3]) << 24);
    if (magic != MakeFourCC('D', 'D', 'S', ' '))
        return false;

    const DdsHeader* header = reinterpret_cast<const DdsHeader*>(bytes.data() + sizeof(uint32_t));
    if (header->size != 124 || header->pixelFormat.size != 32)
        return false;

    out = {};
    out.filename = path.substr(path.find_last_of("\\/") + 1);
    out.width = header->width;
    out.height = header->height;
    out.mipLevels = std::max<uint32_t>(1, header->mipMapCount);

    const uint32_t ddpfFourCC = 0x00000004;
    const uint32_t ddpfRGB = 0x00000040;
    if (header->pixelFormat.flags & ddpfFourCC)
    {
        if (header->pixelFormat.fourCC == MakeFourCC('D', 'X', 'T', '1'))
        {
            out.format = VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
            out.blockBytes = 8;
            out.compressed = true;
        }
        else if (header->pixelFormat.fourCC == MakeFourCC('D', 'X', 'T', '3'))
        {
            out.format = VK_FORMAT_BC2_SRGB_BLOCK;
            out.blockBytes = 16;
            out.compressed = true;
        }
        else if (header->pixelFormat.fourCC == MakeFourCC('D', 'X', 'T', '5'))
        {
            out.format = VK_FORMAT_BC3_SRGB_BLOCK;
            out.blockBytes = 16;
            out.compressed = true;
        }
        else
        {
            Tracenf("[TERRAIN-TEX] unsupported DDS fourCC in %s", path.c_str());
            return false;
        }
    }
    else if ((header->pixelFormat.flags & ddpfRGB) && header->pixelFormat.rgbBitCount == 32)
    {
        out.format = VK_FORMAT_R8G8B8A8_SRGB;
        out.bytesPerPixel = 4;
        out.compressed = false;
    }
    else
    {
        return false;
    }

    size_t offset = sizeof(uint32_t) + sizeof(DdsHeader);
    for (uint32_t mip = 0; mip < out.mipLevels; ++mip)
    {
        const uint32_t mipWidth = std::max(1u, out.width >> mip);
        const uint32_t mipHeight = std::max(1u, out.height >> mip);
        const size_t mipSize = out.compressed
            ? static_cast<size_t>(std::max(1u, (mipWidth + 3u) / 4u)) * std::max(1u, (mipHeight + 3u) / 4u) * out.blockBytes
            : static_cast<size_t>(mipWidth) * mipHeight * out.bytesPerPixel;
        if (offset + mipSize > bytes.size())
            return false;

        VkBufferImageCopy region{};
        region.bufferOffset = static_cast<VkDeviceSize>(out.pixels.size());
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = mip;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {mipWidth, mipHeight, 1};
        out.regions.push_back(region);
        out.pixels.insert(out.pixels.end(), bytes.begin() + offset, bytes.begin() + offset + mipSize);
        offset += mipSize;
    }

    return true;
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
    VkBufferUsageFlags usage, const void* initialData, TerrainRenderer::Buffer& out)
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

void CopyBuffer(VkCommandBuffer cmd, VkBuffer src, VkBuffer dst, VkDeviceSize size)
{
    VkBufferCopy copy{};
    copy.size = size;
    vkCmdCopyBuffer(cmd, src, dst, 1, &copy);
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

bool CreateDeviceLocalImage(VulkanDevice& device, VkDevice vkDevice, uint32_t width, uint32_t height,
    uint32_t mipLevels, VkFormat format, VkImage& image, VkDeviceMemory& memory)
{
    VkImageCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    create.imageType = VK_IMAGE_TYPE_2D;
    create.format = format;
    create.extent = {width, height, 1};
    create.mipLevels = mipLevels;
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

void TransitionImageLayout(VkCommandBuffer cmd, VkImage image, uint32_t mipLevels,
    VkImageLayout oldLayout, VkImageLayout newLayout)
{
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = mipLevels;
    barrier.subresourceRange.baseArrayLayer = 0;
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

std::string LowerCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool ReadMapSetting(client::asset::IAssetReader& assets, const std::string& path, uint32_t& mapSizeX, uint32_t& mapSizeY,
    uint32_t& baseX, uint32_t& baseY, uint32_t& cellScale, float& heightScale)
{
    auto text = assets.ReadText(path);
    if (!text)
        return false;
    std::istringstream file(*text);

    std::string key;
    while (file >> key)
    {
        key = LowerCopy(key);
        if (key == "mapsize")
            file >> mapSizeX >> mapSizeY;
        else if (key == "baseposition")
            file >> baseX >> baseY;
        else if (key == "cellscale")
            file >> cellScale;
        else if (key == "heightscale")
            file >> heightScale;
        else
        {
            std::string rest;
            std::getline(file, rest);
        }
    }

    return mapSizeX > 0 && mapSizeY > 0 && cellScale > 0 && heightScale > 0.0f;
}

uint16_t ReadU16LE(const uint8_t* data)
{
    return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

bool ReadHeightRaw(client::asset::IAssetReader& assets, const std::string& path, std::vector<uint16_t>& heights)
{
    auto bytes = assets.ReadAll(path);
    if (!bytes)
        return false;

    if (bytes->size() != 131u * 131u * sizeof(uint16_t))
        return false;

    heights.resize(131u * 131u);
    for (size_t i = 0; i < heights.size(); ++i)
        heights[i] = ReadU16LE(bytes->data() + i * sizeof(uint16_t));
    return true;
}

float BilinearHeightCm(const std::vector<float>& grid, uint32_t width, uint32_t height,
    float localXcm, float localYcm, float cellScaleCm)
{
    if (grid.empty() || width < 2 || height < 2)
        return 0.0f;

    const float maxX = static_cast<float>(width - 1) * cellScaleCm;
    const float maxY = static_cast<float>(height - 1) * cellScaleCm;
    localXcm = std::clamp(localXcm, 0.0f, maxX);
    localYcm = std::clamp(localYcm, 0.0f, maxY);

    const float gx = localXcm / cellScaleCm;
    const float gy = localYcm / cellScaleCm;
    const uint32_t x0 = std::min(static_cast<uint32_t>(gx), width - 2);
    const uint32_t y0 = std::min(static_cast<uint32_t>(gy), height - 2);
    const uint32_t x1 = x0 + 1;
    const uint32_t y1 = y0 + 1;
    const float tx = gx - static_cast<float>(x0);
    const float ty = gy - static_cast<float>(y0);

    const float h00 = grid[y0 * width + x0];
    const float h10 = grid[y0 * width + x1];
    const float h01 = grid[y1 * width + x0];
    const float h11 = grid[y1 * width + x1];
    const float h0 = h00 + (h10 - h00) * tx;
    const float h1 = h01 + (h11 - h01) * tx;
    return h0 + (h1 - h0) * ty;
}

std::string Trim(std::string value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.pop_back();
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
        value = value.substr(1, value.size() - 2);
    return value;
}

std::string ResolveTerrainTexturePath(const std::string& textureSetPath)
{
    const std::string lower = LowerCopy(textureSetPath);
    const std::string marker = "terrainmaps\\";
    size_t pos = lower.find(marker);
    if (pos == std::string::npos)
        pos = lower.find("terrainmaps/");

    if (pos != std::string::npos)
    {
        std::string relative = textureSetPath.substr(pos + marker.size());
        std::replace(relative.begin(), relative.end(), '\\', '/');
        return "assets/ymi work/terrainmaps/" + relative;
    }

    return textureSetPath;
}

float ParseFloatOrDefault(const std::string& value, float fallback)
{
    const std::string trimmed = Trim(value);
    char* end = nullptr;
    const char* begin = trimmed.c_str();
    const float parsed = std::strtof(begin, &end);
    return end != begin ? parsed : fallback;
}

std::string ReadTextureSetPathFromSetting(client::asset::IAssetReader& assets,
    const std::string& settingPath)
{
    auto text = assets.ReadText(settingPath);
    if (!text)
        return "assets/textureset/metin2_c1.txt";
    std::istringstream file(*text);

    std::string key;
    while (file >> key)
    {
        key = LowerCopy(key);
        if (key == "textureset")
        {
            std::string value;
            file >> value;
            value = Trim(value);
            std::replace(value.begin(), value.end(), '\\', '/');
            if (value.find(':') != std::string::npos || value.rfind("\\\\", 0) == 0)
                return value;
            return value.rfind("assets/", 0) == 0 ? value : "assets/" + value;
        }

        std::string rest;
        std::getline(file, rest);
    }

    return "assets/textureset/metin2_c1.txt";
}

std::vector<TextureSetEntry> LoadTextureSetEntries(client::asset::IAssetReader& assets,
    const std::string& path)
{
    std::vector<TextureSetEntry> entries(1);
    auto text = assets.ReadText(path);
    if (!text)
        return entries;
    std::istringstream file(*text);

    std::string line;
    int currentIndex = 0;
    while (std::getline(file, line))
    {
        const std::string lower = LowerCopy(Trim(line));
        if (lower.rfind("start texture", 0) == 0)
        {
            currentIndex = std::atoi(lower.c_str() + std::strlen("start texture"));
            if (currentIndex >= static_cast<int>(entries.size()))
                entries.resize(static_cast<size_t>(currentIndex) + 1);
            continue;
        }

        if (currentIndex > 0 && lower.find(".dds") != std::string::npos)
        {
            TextureSetEntry& entry = entries[static_cast<size_t>(currentIndex)];
            entry.path = ResolveTerrainTexturePath(Trim(line));

            std::string scaleULine;
            std::string scaleVLine;
            if (std::getline(file, scaleULine))
                entry.scaleU = ParseFloatOrDefault(scaleULine, 1.0f);
            if (std::getline(file, scaleVLine))
                entry.scaleV = ParseFloatOrDefault(scaleVLine, 1.0f);
            currentIndex = 0;
        }
    }

    return entries;
}

std::vector<std::string> LoadTextureSetPaths(client::asset::IAssetReader& assets,
    const std::string& path)
{
    const std::vector<TextureSetEntry> entries = LoadTextureSetEntries(assets, path);
    std::vector<std::string> paths(entries.size());
    for (size_t i = 1; i < entries.size(); ++i)
        paths[i] = entries[i].path;
    return paths;
}

bool ReadTileRaw(client::asset::IAssetReader& assets, const std::string& path, std::vector<uint8_t>& outInterior)
{
    auto bytes = assets.ReadAll(path);
    if (!bytes)
        return false;

    if (bytes->size() != 258u * 258u)
        return false;

    outInterior.resize(256u * 256u);
    for (uint32_t y = 0; y < 256u; ++y)
    {
        const uint8_t* src = bytes->data() + (static_cast<size_t>(y) + 1u) * 258u + 1u;
        std::memcpy(outInterior.data() + static_cast<size_t>(y) * 256u, src, 256u);
    }
    return true;
}

uint8_t DominantTileIndex(client::asset::IAssetReader& assets, const std::string& mapDirectory,
    uint32_t mapSizeX, uint32_t mapSizeY)
{
    std::array<uint32_t, 256> counts{};
    for (uint32_t cellX = 0; cellX < mapSizeX; ++cellX)
    {
        for (uint32_t cellY = 0; cellY < mapSizeY; ++cellY)
        {
            const uint32_t cellId = cellX * 1000u + cellY;
            char folder[16]{};
            std::snprintf(folder, sizeof(folder), "%06u", cellId);
            auto bytes = assets.ReadAll(mapDirectory + "/" + folder + "/tile.raw");
            if (!bytes)
                continue;

            for (std::size_t i = 0; i < bytes->size(); ++i)
            {
                if ((*bytes)[i] != 0)
                    ++counts[(*bytes)[i]];
            }
        }
    }

    uint8_t best = 0;
    for (uint32_t i = 1; i < counts.size(); ++i)
    {
        if (counts[i] > counts[best])
            best = static_cast<uint8_t>(i);
    }
    return best;
}
}

bool TerrainRenderer::Create(VulkanDevice& device, client::asset::IAssetReader& assets)
{
    Destroy();
    m_device = device.GetDevice();
    m_assets = &assets;

    const bool texture = CreateFallbackTexture(device);
    const bool mask = texture ? CreateFallbackMask(device) : false;
    const bool buffers = mask ? CreateBuffers(device) : false;
    const bool descriptors = buffers ? CreateDescriptors() : false;
    const bool pipeline = descriptors ? CreatePipeline(device) : false;
    Tracenf("[TERRAIN] Create: texture=%d mask=%d buffers=%d descriptors=%d pipeline=%d",
        texture ? 1 : 0,
        mask ? 1 : 0,
        buffers ? 1 : 0,
        descriptors ? 1 : 0,
        pipeline ? 1 : 0);

    if (texture && mask && buffers && descriptors && pipeline)
        return true;

    Destroy();
    return false;
}

bool TerrainRenderer::LoadMap(VulkanDevice& device, const std::string& mapDirectory, int32_t serverX, int32_t serverY)
{
    if (!m_device)
        return false;

    device.WaitIdle();
    DestroyBuffer(m_vertexBuffer);
    DestroyBuffer(m_indexBuffer);
    DestroyTerrainLayers();
    m_tileIndices.clear();
    m_tileGridWidth = 0;
    m_tileGridHeight = 0;
    m_indexCount = 0;

    if (!CreateMapBuffers(device, mapDirectory, serverX, serverY))
    {
        Tracen("[TERRAIN-MAP] LoadMap failed; restoring flat fallback terrain");
        m_mapLoaded = false;
        m_heightCmGrid.clear();
        const bool flat = CreateFlatBuffers(device);
        CreateDescriptors();
        return flat;
    }

    const bool tilesLoaded = LoadTileIndices(mapDirectory);
    const bool layersBuilt = tilesLoaded ? BuildTerrainLayers(device, mapDirectory) : false;
    if (layersBuilt)
    {
        CreateDescriptors();
        Tracenf("[TERRAIN-SPLAT] active layers=%zu tileGrid=%ux%u",
            m_layers.size(),
            m_tileGridWidth,
            m_tileGridHeight);
    }
    else
    {
        Tracen("[TERRAIN-SPLAT] no terrain layers built; using dominant texture fallback");
        LoadDominantTerrainTexture(device, mapDirectory);
        CreateDescriptors();
    }
    return true;
}

bool TerrainRenderer::RecreatePipeline(VulkanDevice& device)
{
    if (!m_device)
        return true;

    DestroyPipeline();
    if (device.GetRenderPass() == VK_NULL_HANDLE)
        return true;

    return CreatePipeline(device);
}

void TerrainRenderer::Render(VulkanDevice& device, const WorldCamera& camera)
{
    static bool loggedDraw = false;
    static bool loggedSkip = false;

    if (!m_pipeline || m_indexCount == 0 || !device.IsFrameActive())
    {
        if (!loggedSkip)
        {
            Tracen("[TERRAIN] Render skip: inactive pipeline/frame");
            loggedSkip = true;
        }
        return;
    }

    const VkExtent2D extent = device.GetSwapchainExtent();
    if (extent.width == 0 || extent.height == 0)
        return;

    const uint32_t frameIndex = device.GetFrameIndex();
    UpdateUniform(frameIndex, camera);

    VkCommandBuffer cmd = device.GetCommandBuffer();

    VkClearAttachment depthClear{};
    depthClear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depthClear.clearValue.depthStencil.depth = 1.0f;

    VkClearRect depthRect{};
    depthRect.rect = {{0, 0}, extent};
    depthRect.baseArrayLayer = 0;
    depthRect.layerCount = 1;
    vkCmdClearAttachments(cmd, 1, &depthClear, 1, &depthRect);

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
    vkCmdBindVertexBuffers(cmd, 0, 1, &m_vertexBuffer.buffer, &offset);
    vkCmdBindIndexBuffer(cmd, m_indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
    struct TerrainPushConstants
    {
        float layerParams[4];
    };

    if (!m_layers.empty() && m_layerDescriptorSets.size() == m_layers.size() * kFramesInFlight)
    {
        for (size_t layerIndex = 0; layerIndex < m_layers.size(); ++layerIndex)
        {
            const TerrainLayer& layer = m_layers[layerIndex];
            TerrainPushConstants push{{layer.tilingU, layer.tilingV, 0.0f, 0.0f}};
            vkCmdPushConstants(cmd, m_pipelineLayout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(push), &push);

            const VkDescriptorSet descriptorSet =
                m_layerDescriptorSets[layerIndex * kFramesInFlight + frameIndex];
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
                0, 1, &descriptorSet, 0, nullptr);
            vkCmdDrawIndexed(cmd, m_indexCount, 1, 0, 0, 0);
        }
    }
    else
    {
        TerrainPushConstants push{{1.0f, 1.0f, 0.0f, 0.0f}};
        vkCmdPushConstants(cmd, m_pipelineLayout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(push), &push);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
            0, 1, &m_descriptorSets[frameIndex], 0, nullptr);
        vkCmdDrawIndexed(cmd, m_indexCount, 1, 0, 0, 0);
    }

    if (!loggedDraw)
    {
        Tracenf("[TERRAIN] Render: %s indexCount=%u layers=%zu camera eye=(%.2f,%.2f,%.2f) target=(%.2f,%.2f,%.2f)",
            m_mapLoaded ? "metin heightmap" : "flat 100m ground",
            m_indexCount,
            m_layers.size(),
            camera.eye.x,
            camera.eye.y,
            camera.eye.z,
            camera.target.x,
            camera.target.y,
            camera.target.z);
        loggedDraw = true;
    }
}

float TerrainRenderer::SampleHeightAt(float localX, float localZ) const
{
    if (!m_mapLoaded || m_heightCmGrid.empty())
        return 0.0f;

    const float localXcm = m_spawnLocalXcm + localX * 100.0f;
    const float localYcm = m_spawnLocalYcm - localZ * 100.0f;
    const float heightCm = BilinearHeightCm(m_heightCmGrid, m_heightGridWidth, m_heightGridHeight,
        localXcm, localYcm, m_cellScaleMeters * 100.0f);
    return heightCm * 0.01f;
}

TerrainRenderer::MovementBounds TerrainRenderer::GetMovementBounds() const
{
    MovementBounds bounds{};
    if (!m_mapLoaded || m_heightGridWidth < 2 || m_heightGridHeight < 2)
        return bounds;

    const float cellScaleCm = m_cellScaleMeters * 100.0f;
    const float maxXcm = static_cast<float>(m_heightGridWidth - 1u) * cellScaleCm;
    const float maxYcm = static_cast<float>(m_heightGridHeight - 1u) * cellScaleCm;

    bounds.valid = true;
    bounds.minX = (0.0f - m_spawnLocalXcm) * 0.01f;
    bounds.maxX = (maxXcm - m_spawnLocalXcm) * 0.01f;
    bounds.minZ = (m_spawnLocalYcm - maxYcm) * 0.01f;
    bounds.maxZ = (m_spawnLocalYcm - 0.0f) * 0.01f;
    return bounds;
}

float TerrainRenderer::SampleHeight(WorldVec3 position) const
{
    return SampleHeightAt(position.x, position.z);
}

void TerrainRenderer::Destroy()
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

    DestroyBuffer(m_vertexBuffer);
    DestroyBuffer(m_indexBuffer);
    for (Buffer& buffer : m_uniformBuffers)
        DestroyBuffer(buffer);
    DestroyTerrainLayers();
    DestroyTexture(m_baseTexture);
    DestroyTexture(m_fallbackMask);

    m_layerDescriptorSets.clear();
    m_tileIndices.clear();
    m_tileGridWidth = 0;
    m_tileGridHeight = 0;
    m_indexCount = 0;
    m_heightGridWidth = 0;
    m_heightGridHeight = 0;
    m_mapSizeX = 0;
    m_mapSizeY = 0;
    m_spawnLocalXcm = 0.0f;
    m_spawnLocalYcm = 0.0f;
    m_spawnHeightCm = 0.0f;
    m_mapLoaded = false;
    m_heightCmGrid.clear();
    m_device = VK_NULL_HANDLE;
    m_assets = nullptr;
}

bool TerrainRenderer::CreateBuffers(VulkanDevice& device)
{
    return CreateFlatBuffers(device);
}

bool TerrainRenderer::CreateFlatBuffers(VulkanDevice& device)
{
    constexpr float halfSize = 50.0f;
    const std::array<Vertex, 4> vertices =
    {{
        {{-halfSize, 0.0f, -halfSize}, {0.0f, 0.0f}, {0.0f, 0.0f}},
        {{ halfSize, 0.0f, -halfSize}, {10.0f, 0.0f}, {1.0f, 0.0f}},
        {{ halfSize, 0.0f,  halfSize}, {10.0f, 10.0f}, {1.0f, 1.0f}},
        {{-halfSize, 0.0f,  halfSize}, {0.0f, 10.0f}, {0.0f, 1.0f}},
    }};

    const std::array<uint32_t, 6> indices = {0, 1, 2, 0, 2, 3};
    m_indexCount = static_cast<uint32_t>(indices.size());

    CreateHostVisibleBuffer(device, m_device, sizeof(Vertex) * vertices.size(),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertices.data(), m_vertexBuffer);
    CreateHostVisibleBuffer(device, m_device, sizeof(uint32_t) * indices.size(),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indices.data(), m_indexBuffer);

    for (Buffer& buffer : m_uniformBuffers)
    {
        CreateHostVisibleBuffer(device, m_device, sizeof(UniformBlock),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, nullptr, buffer);
    }

    return true;
}

bool TerrainRenderer::CreateMapBuffers(VulkanDevice& device, const std::string& mapDirectory, int32_t serverX, int32_t serverY)
{
    uint32_t baseX = 0;
    uint32_t baseY = 0;
    uint32_t cellScaleCm = 200;
    float heightScale = 0.5f;
    if (!m_assets || !ReadMapSetting(*m_assets, mapDirectory + "/setting.txt", m_mapSizeX, m_mapSizeY,
                         baseX, baseY, cellScaleCm, heightScale))
    {
        Tracenf("[TERRAIN-MAP] invalid setting.txt: %s", (mapDirectory + "/setting.txt").c_str());
        return false;
    }

    constexpr uint32_t kTerrainSize = 128;
    constexpr uint32_t kRawStride = 131;
    m_heightGridWidth = m_mapSizeX * kTerrainSize + 1;
    m_heightGridHeight = m_mapSizeY * kTerrainSize + 1;
    m_cellScaleMeters = static_cast<float>(cellScaleCm) * 0.01f;
    m_spawnLocalXcm = static_cast<float>(serverX) - static_cast<float>(baseX);
    m_spawnLocalYcm = static_cast<float>(serverY) - static_cast<float>(baseY);

    m_heightCmGrid.assign(static_cast<size_t>(m_heightGridWidth) * m_heightGridHeight, 0.0f);
    uint32_t loadedCells = 0;
    float minHeightCm = std::numeric_limits<float>::max();
    float maxHeightCm = -std::numeric_limits<float>::max();

    for (uint32_t cellX = 0; cellX < m_mapSizeX; ++cellX)
    {
        for (uint32_t cellY = 0; cellY < m_mapSizeY; ++cellY)
        {
            const uint32_t cellId = cellX * 1000u + cellY;
            char folder[16]{};
            std::snprintf(folder, sizeof(folder), "%06u", cellId);
            const std::string heightPath = mapDirectory + "/" + folder + "/height.raw";

            std::vector<uint16_t> raw;
            if (!m_assets || !ReadHeightRaw(*m_assets, heightPath, raw))
            {
                Tracenf("[TERRAIN-MAP] failed height.raw: %s", heightPath.c_str());
                return false;
            }

            ++loadedCells;
            for (uint32_t y = 0; y <= kTerrainSize; ++y)
            {
                for (uint32_t x = 0; x <= kTerrainSize; ++x)
                {
                    const uint16_t rawHeight = raw[(y + 1u) * kRawStride + (x + 1u)];
                    const float heightCm = static_cast<float>(rawHeight) * heightScale;
                    const uint32_t gx = cellX * kTerrainSize + x;
                    const uint32_t gy = cellY * kTerrainSize + y;
                    m_heightCmGrid[static_cast<size_t>(gy) * m_heightGridWidth + gx] = heightCm;
                    minHeightCm = std::min(minHeightCm, heightCm);
                    maxHeightCm = std::max(maxHeightCm, heightCm);
                }
            }
        }
    }

    m_spawnHeightCm = BilinearHeightCm(m_heightCmGrid, m_heightGridWidth, m_heightGridHeight,
        m_spawnLocalXcm, m_spawnLocalYcm, static_cast<float>(cellScaleCm));

    std::vector<Vertex> vertices;
    vertices.reserve(m_heightCmGrid.size());
    for (uint32_t gy = 0; gy < m_heightGridHeight; ++gy)
    {
        for (uint32_t gx = 0; gx < m_heightGridWidth; ++gx)
        {
            const float localXcm = static_cast<float>(gx * cellScaleCm);
            const float localYcm = static_cast<float>(gy * cellScaleCm);
            const float heightCm = m_heightCmGrid[static_cast<size_t>(gy) * m_heightGridWidth + gx];

            Vertex vertex{};
            vertex.position[0] = (localXcm - m_spawnLocalXcm) * 0.01f;
            vertex.position[1] = heightCm * 0.01f;
            vertex.position[2] = -(localYcm - m_spawnLocalYcm) * 0.01f;
            vertex.texUv[0] = static_cast<float>(gx) / 16.0f;
            vertex.texUv[1] = static_cast<float>(gy) / 16.0f;
            vertex.maskUv[0] = m_heightGridWidth > 1
                ? static_cast<float>(gx) / static_cast<float>(m_heightGridWidth - 1u)
                : 0.0f;
            vertex.maskUv[1] = m_heightGridHeight > 1
                ? static_cast<float>(gy) / static_cast<float>(m_heightGridHeight - 1u)
                : 0.0f;
            vertices.push_back(vertex);
        }
    }

    std::vector<uint32_t> indices;
    indices.reserve(static_cast<size_t>(m_heightGridWidth - 1) * (m_heightGridHeight - 1) * 6u);
    for (uint32_t y = 0; y < m_heightGridHeight - 1; ++y)
    {
        for (uint32_t x = 0; x < m_heightGridWidth - 1; ++x)
        {
            const uint32_t i0 = y * m_heightGridWidth + x;
            const uint32_t i1 = i0 + 1;
            const uint32_t i2 = i0 + m_heightGridWidth;
            const uint32_t i3 = i2 + 1;
            indices.push_back(i0);
            indices.push_back(i1);
            indices.push_back(i3);
            indices.push_back(i0);
            indices.push_back(i3);
            indices.push_back(i2);
        }
    }

    m_indexCount = static_cast<uint32_t>(indices.size());
    CreateHostVisibleBuffer(device, m_device, sizeof(Vertex) * vertices.size(),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertices.data(), m_vertexBuffer);
    CreateHostVisibleBuffer(device, m_device, sizeof(uint32_t) * indices.size(),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indices.data(), m_indexBuffer);

    m_mapLoaded = true;
    Tracenf("[TERRAIN-MAP] loaded dir=%s cells=%u mapSize=%ux%u base=(%u,%u) spawnServer=(%d,%d) spawnLocalCm=(%.0f,%.0f) spawnHeightCm=%.1f heightCm=%.1f..%.1f vertices=%zu indices=%zu",
        mapDirectory.c_str(),
        loadedCells,
        m_mapSizeX,
        m_mapSizeY,
        baseX,
        baseY,
        serverX,
        serverY,
        m_spawnLocalXcm,
        m_spawnLocalYcm,
        m_spawnHeightCm,
        minHeightCm,
        maxHeightCm,
        vertices.size(),
        indices.size());
    return true;
}

bool TerrainRenderer::CreateFallbackTexture(VulkanDevice& device)
{
    DestroyTexture(m_baseTexture);

    const uint32_t pixels[] =
    {
        0xff49643d, 0xff607d4f,
        0xff607d4f, 0xff49643d,
    };

    DdsImage image{};
    image.filename = "terrain_fallback";
    image.width = 2;
    image.height = 2;
    image.mipLevels = 1;
    image.format = VK_FORMAT_R8G8B8A8_SRGB;
    image.bytesPerPixel = 4;
    image.pixels.resize(sizeof(pixels));
    std::memcpy(image.pixels.data(), pixels, sizeof(pixels));

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {2, 2, 1};
    image.regions.push_back(region);

    VkQueue graphicsQueue = VK_NULL_HANDLE;
    vkGetDeviceQueue(m_device, device.GetGraphicsQueueFamily(), 0, &graphicsQueue);

    CreateDeviceLocalImage(device, m_device, image.width, image.height, image.mipLevels,
        image.format, m_baseTexture.image, m_baseTexture.memory);

    Buffer staging{};
    CreateHostVisibleBuffer(device, m_device, image.pixels.size(),
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT, image.pixels.data(), staging);

    VkCommandPool uploadPool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = BeginOneTimeCommands(m_device, device.GetGraphicsQueueFamily(), uploadPool);
    TransitionImageLayout(cmd, m_baseTexture.image, image.mipLevels,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(cmd, staging.buffer, m_baseTexture.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        static_cast<uint32_t>(image.regions.size()),
        image.regions.data());
    TransitionImageLayout(cmd, m_baseTexture.image, image.mipLevels,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EndOneTimeCommands(m_device, graphicsQueue, uploadPool, cmd);
    DestroyBuffer(staging);

    m_baseTexture.format = image.format;
    m_baseTexture.width = image.width;
    m_baseTexture.height = image.height;
    m_baseTexture.mipLevels = image.mipLevels;
    m_baseTexture.name = image.filename;

    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = m_baseTexture.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = m_baseTexture.format;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = m_baseTexture.mipLevels;
    view.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(m_device, &view, nullptr, &m_baseTexture.view));

    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.maxLod = 1.0f;
    VK_CHECK(vkCreateSampler(m_device, &sampler, nullptr, &m_baseTexture.sampler));
    return true;
}

bool TerrainRenderer::CreateFallbackMask(VulkanDevice& device)
{
    DestroyTexture(m_fallbackMask);

    const uint8_t pixel = 255;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    vkGetDeviceQueue(m_device, device.GetGraphicsQueueFamily(), 0, &graphicsQueue);

    CreateDeviceLocalImage(device, m_device, 1, 1, 1,
        VK_FORMAT_R8_UNORM, m_fallbackMask.image, m_fallbackMask.memory);

    Buffer staging{};
    CreateHostVisibleBuffer(device, m_device, sizeof(pixel),
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &pixel, staging);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {1, 1, 1};

    VkCommandPool uploadPool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = BeginOneTimeCommands(m_device, device.GetGraphicsQueueFamily(), uploadPool);
    TransitionImageLayout(cmd, m_fallbackMask.image, 1,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(cmd, staging.buffer, m_fallbackMask.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    TransitionImageLayout(cmd, m_fallbackMask.image, 1,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EndOneTimeCommands(m_device, graphicsQueue, uploadPool, cmd);
    DestroyBuffer(staging);

    m_fallbackMask.format = VK_FORMAT_R8_UNORM;
    m_fallbackMask.width = 1;
    m_fallbackMask.height = 1;
    m_fallbackMask.mipLevels = 1;
    m_fallbackMask.name = "terrain_full_mask";

    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = m_fallbackMask.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = m_fallbackMask.format;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(m_device, &view, nullptr, &m_fallbackMask.view));

    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.maxLod = 1.0f;
    VK_CHECK(vkCreateSampler(m_device, &sampler, nullptr, &m_fallbackMask.sampler));
    return true;
}

bool TerrainRenderer::LoadDominantTerrainTexture(VulkanDevice& device, const std::string& mapDirectory)
{
    const std::vector<std::string> texturePaths =
        m_assets ? LoadTextureSetPaths(*m_assets, ReadTextureSetPathFromSetting(*m_assets, mapDirectory + "/setting.txt"))
                 : std::vector<std::string>{};
    uint8_t index = m_assets ? DominantTileIndex(*m_assets, mapDirectory, m_mapSizeX, m_mapSizeY) : 0;
    if (index == 0 || index >= texturePaths.size() || texturePaths[index].empty())
        index = texturePaths.size() > 5 && !texturePaths[5].empty() ? 5 : 1;
    if (index >= texturePaths.size() || texturePaths[index].empty())
        return false;

    DdsImage dds{};
    if (!m_assets || !LoadDdsImage(*m_assets, texturePaths[index], dds))
        return false;

    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(device.GetPhysicalDevice(), dds.format, &props);
    const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((props.optimalTilingFeatures & required) != required)
    {
        Tracenf("[TERRAIN-TEX] unsupported format features for %s", texturePaths[index].c_str());
        return false;
    }

    Texture newTexture{};
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    vkGetDeviceQueue(m_device, device.GetGraphicsQueueFamily(), 0, &graphicsQueue);
    CreateDeviceLocalImage(device, m_device, dds.width, dds.height, dds.mipLevels,
        dds.format, newTexture.image, newTexture.memory);

    Buffer staging{};
    CreateHostVisibleBuffer(device, m_device, dds.pixels.size(),
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT, dds.pixels.data(), staging);

    VkCommandPool uploadPool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = BeginOneTimeCommands(m_device, device.GetGraphicsQueueFamily(), uploadPool);
    TransitionImageLayout(cmd, newTexture.image, dds.mipLevels,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(cmd, staging.buffer, newTexture.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        static_cast<uint32_t>(dds.regions.size()),
        dds.regions.data());
    TransitionImageLayout(cmd, newTexture.image, dds.mipLevels,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EndOneTimeCommands(m_device, graphicsQueue, uploadPool, cmd);
    DestroyBuffer(staging);

    newTexture.format = dds.format;
    newTexture.width = dds.width;
    newTexture.height = dds.height;
    newTexture.mipLevels = dds.mipLevels;
    newTexture.name = dds.filename;

    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = newTexture.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = newTexture.format;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = newTexture.mipLevels;
    view.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(m_device, &view, nullptr, &newTexture.view));

    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = newTexture.mipLevels > 1 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.maxLod = static_cast<float>(newTexture.mipLevels);
    VK_CHECK(vkCreateSampler(m_device, &sampler, nullptr, &newTexture.sampler));

    DestroyTexture(m_baseTexture);
    m_baseTexture = newTexture;
    UpdateDescriptors();
    Tracenf("[TERRAIN-TEX] loaded textureset index=%u file=%s size=%ux%u mips=%u format=%s",
        index,
        texturePaths[index].c_str(),
        m_baseTexture.width,
        m_baseTexture.height,
        m_baseTexture.mipLevels,
        VkFormatName(m_baseTexture.format));
    return true;
}

bool TerrainRenderer::LoadTileIndices(const std::string& mapDirectory)
{
    if (m_mapSizeX == 0 || m_mapSizeY == 0)
        return false;

    constexpr uint32_t kTileSize = 256;
    m_tileGridWidth = m_mapSizeX * kTileSize;
    m_tileGridHeight = m_mapSizeY * kTileSize;
    m_tileIndices.assign(static_cast<size_t>(m_tileGridWidth) * m_tileGridHeight, 0);

    uint32_t loadedCells = 0;
    for (uint32_t cellX = 0; cellX < m_mapSizeX; ++cellX)
    {
        for (uint32_t cellY = 0; cellY < m_mapSizeY; ++cellY)
        {
            const uint32_t cellId = cellX * 1000u + cellY;
            char folder[16]{};
            std::snprintf(folder, sizeof(folder), "%06u", cellId);

            std::vector<uint8_t> interior;
            const std::string tilePath = mapDirectory + "/" + folder + "/tile.raw";
            if (!m_assets || !ReadTileRaw(*m_assets, tilePath, interior))
            {
                Tracenf("[TERRAIN-TILE] failed tile.raw: %s", tilePath.c_str());
                m_tileIndices.clear();
                m_tileGridWidth = 0;
                m_tileGridHeight = 0;
                return false;
            }

            ++loadedCells;
            for (uint32_t y = 0; y < kTileSize; ++y)
            {
                const size_t srcOffset = static_cast<size_t>(y) * kTileSize;
                const size_t dstOffset =
                    (static_cast<size_t>(cellY) * kTileSize + y) * m_tileGridWidth +
                    static_cast<size_t>(cellX) * kTileSize;
                std::memcpy(m_tileIndices.data() + dstOffset, interior.data() + srcOffset, kTileSize);
            }
        }
    }

    Tracenf("[TERRAIN-TILE] loaded cells=%u tileGrid=%ux%u",
        loadedCells,
        m_tileGridWidth,
        m_tileGridHeight);
    return true;
}

bool TerrainRenderer::BuildTerrainLayers(VulkanDevice& device, const std::string& mapDirectory)
{
    DestroyTerrainLayers();
    if (m_tileIndices.empty())
        return false;

    std::array<uint32_t, 256> coverage{};
    for (uint8_t index : m_tileIndices)
    {
        if (index != 0)
            ++coverage[index];
    }

    std::vector<uint32_t> textureIndices;
    for (uint32_t index = 1; index < coverage.size(); ++index)
    {
        if (coverage[index] > 0)
            textureIndices.push_back(index);
    }

    if (textureIndices.empty())
        return false;

    std::sort(textureIndices.begin(), textureIndices.end(),
        [&coverage](uint32_t a, uint32_t b)
        {
            if (coverage[a] == coverage[b])
                return a < b;
            return coverage[a] > coverage[b];
        });

    const std::string textureSetPath = m_assets
        ? ReadTextureSetPathFromSetting(*m_assets, mapDirectory + "/setting.txt")
        : std::string();
    const std::vector<TextureSetEntry> textureSet = m_assets
        ? LoadTextureSetEntries(*m_assets, textureSetPath)
        : std::vector<TextureSetEntry>{};
    if (textureSet.empty())
    {
        Tracenf("[TERRAIN-SPLAT] failed textureset: %s", textureSetPath.c_str());
        return false;
    }

    std::vector<TerrainLayer> newLayers;
    newLayers.reserve(textureIndices.size());

    VkQueue graphicsQueue = VK_NULL_HANDLE;
    vkGetDeviceQueue(m_device, device.GetGraphicsQueueFamily(), 0, &graphicsQueue);

    for (uint32_t textureIndex : textureIndices)
    {
        if (textureIndex >= textureSet.size() || textureSet[textureIndex].path.empty())
        {
            Tracenf("[TERRAIN-SPLAT] missing textureset index=%u", textureIndex);
            continue;
        }

        DdsImage dds{};
        if (!m_assets || !LoadDdsImage(*m_assets, textureSet[textureIndex].path, dds))
        {
            Tracenf("[TERRAIN-SPLAT] failed DDS index=%u path=%s",
                textureIndex,
                textureSet[textureIndex].path.c_str());
            continue;
        }

        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(device.GetPhysicalDevice(), dds.format, &props);
        const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if ((props.optimalTilingFeatures & required) != required)
        {
            Tracenf("[TERRAIN-SPLAT] unsupported format features for %s", textureSet[textureIndex].path.c_str());
            continue;
        }

        TerrainLayer layer{};
        layer.textureIndex = textureIndex;
        layer.tilingU = textureSet[textureIndex].scaleU;
        layer.tilingV = textureSet[textureIndex].scaleV;
        layer.coverage = coverage[textureIndex];

        CreateDeviceLocalImage(device, m_device, dds.width, dds.height, dds.mipLevels,
            dds.format, layer.diffuse.image, layer.diffuse.memory);

        Buffer staging{};
        CreateHostVisibleBuffer(device, m_device, dds.pixels.size(),
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT, dds.pixels.data(), staging);

        VkCommandPool uploadPool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = BeginOneTimeCommands(m_device, device.GetGraphicsQueueFamily(), uploadPool);
        TransitionImageLayout(cmd, layer.diffuse.image, dds.mipLevels,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyBufferToImage(cmd, staging.buffer, layer.diffuse.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            static_cast<uint32_t>(dds.regions.size()),
            dds.regions.data());
        TransitionImageLayout(cmd, layer.diffuse.image, dds.mipLevels,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        EndOneTimeCommands(m_device, graphicsQueue, uploadPool, cmd);
        DestroyBuffer(staging);

        layer.diffuse.format = dds.format;
        layer.diffuse.width = dds.width;
        layer.diffuse.height = dds.height;
        layer.diffuse.mipLevels = dds.mipLevels;
        layer.diffuse.name = dds.filename;

        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = layer.diffuse.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = layer.diffuse.format;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = layer.diffuse.mipLevels;
        view.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(m_device, &view, nullptr, &layer.diffuse.view));

        VkSamplerCreateInfo sampler{};
        sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler.magFilter = VK_FILTER_LINEAR;
        sampler.minFilter = VK_FILTER_LINEAR;
        sampler.mipmapMode = layer.diffuse.mipLevels > 1 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler.maxLod = static_cast<float>(layer.diffuse.mipLevels);
        VK_CHECK(vkCreateSampler(m_device, &sampler, nullptr, &layer.diffuse.sampler));

        newLayers.push_back(layer);
        Tracenf("[TERRAIN-SPLAT] layer index=%u coverage=%u file=%s tiling=(%.3f,%.3f)",
            textureIndex,
            coverage[textureIndex],
            textureSet[textureIndex].path.c_str(),
            textureSet[textureIndex].scaleU,
            textureSet[textureIndex].scaleV);
    }

    if (newLayers.empty())
        return false;

    m_layers = std::move(newLayers);
    for (TerrainLayer& layer : m_layers)
    {
        if (!GenerateLayerMask(device, layer))
        {
            DestroyTerrainLayers();
            return false;
        }
    }

    return true;
}

bool TerrainRenderer::GenerateLayerMask(VulkanDevice& device, TerrainLayer& layer)
{
    if (m_tileIndices.empty() || m_tileGridWidth == 0 || m_tileGridHeight == 0)
        return false;

    DestroyTexture(layer.mask);

    std::vector<uint8_t> pixels(m_tileIndices.size(), 0);
    const bool baseLayer = !m_layers.empty() && layer.textureIndex == m_layers.front().textureIndex;
    if (baseLayer)
    {
        // The most common texture is drawn as the base carpet; later binary masks blend over it.
        std::fill(pixels.begin(), pixels.end(), 255);
    }
    else
    {
        for (size_t i = 0; i < m_tileIndices.size(); ++i)
            pixels[i] = m_tileIndices[i] == layer.textureIndex ? 255 : 0;
    }

    VkQueue graphicsQueue = VK_NULL_HANDLE;
    vkGetDeviceQueue(m_device, device.GetGraphicsQueueFamily(), 0, &graphicsQueue);

    CreateDeviceLocalImage(device, m_device, m_tileGridWidth, m_tileGridHeight, 1,
        VK_FORMAT_R8_UNORM, layer.mask.image, layer.mask.memory);

    Buffer staging{};
    CreateHostVisibleBuffer(device, m_device, pixels.size(),
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT, pixels.data(), staging);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {m_tileGridWidth, m_tileGridHeight, 1};

    VkCommandPool uploadPool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = BeginOneTimeCommands(m_device, device.GetGraphicsQueueFamily(), uploadPool);
    TransitionImageLayout(cmd, layer.mask.image, 1,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(cmd, staging.buffer, layer.mask.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    TransitionImageLayout(cmd, layer.mask.image, 1,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EndOneTimeCommands(m_device, graphicsQueue, uploadPool, cmd);
    DestroyBuffer(staging);

    layer.mask.format = VK_FORMAT_R8_UNORM;
    layer.mask.width = m_tileGridWidth;
    layer.mask.height = m_tileGridHeight;
    layer.mask.mipLevels = 1;
    layer.mask.name = "terrain_mask_" + std::to_string(layer.textureIndex);

    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = layer.mask.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = layer.mask.format;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(m_device, &view, nullptr, &layer.mask.view));

    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.maxLod = 1.0f;
    VK_CHECK(vkCreateSampler(m_device, &sampler, nullptr, &layer.mask.sampler));

    return true;
}

bool TerrainRenderer::CreateDescriptors()
{
    if (!m_descriptorSetLayout)
    {
        VkDescriptorSetLayoutBinding ubo{};
        ubo.binding = 0;
        ubo.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ubo.descriptorCount = 1;
        ubo.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

        VkDescriptorSetLayoutBinding diffuse{};
        diffuse.binding = 1;
        diffuse.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        diffuse.descriptorCount = 1;
        diffuse.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutBinding mask{};
        mask.binding = 2;
        mask.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        mask.descriptorCount = 1;
        mask.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo layout{};
        layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        std::array<VkDescriptorSetLayoutBinding, 3> bindings = {ubo, diffuse, mask};
        layout.bindingCount = static_cast<uint32_t>(bindings.size());
        layout.pBindings = bindings.data();
        VK_CHECK(vkCreateDescriptorSetLayout(m_device, &layout, nullptr, &m_descriptorSetLayout));
    }

    if (m_descriptorPool)
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
    m_descriptorPool = VK_NULL_HANDLE;
    m_descriptorSets.fill(VK_NULL_HANDLE);
    m_layerDescriptorSets.clear();

    const uint32_t layerCount = static_cast<uint32_t>(std::max<size_t>(1, m_layers.size()));
    const uint32_t descriptorSetCount = kFramesInFlight * layerCount;

    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = descriptorSetCount;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = descriptorSetCount * 2u;

    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = descriptorSetCount;
    pool.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    pool.pPoolSizes = poolSizes.data();
    VK_CHECK(vkCreateDescriptorPool(m_device, &pool, nullptr, &m_descriptorPool));

    std::vector<VkDescriptorSetLayout> layouts(descriptorSetCount, m_descriptorSetLayout);

    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = m_descriptorPool;
    alloc.descriptorSetCount = descriptorSetCount;
    alloc.pSetLayouts = layouts.data();
    if (m_layers.empty())
    {
        VK_CHECK(vkAllocateDescriptorSets(m_device, &alloc, m_descriptorSets.data()));
    }
    else
    {
        m_layerDescriptorSets.resize(descriptorSetCount);
        VK_CHECK(vkAllocateDescriptorSets(m_device, &alloc, m_layerDescriptorSets.data()));
    }

    UpdateDescriptors();

    return true;
}

void TerrainRenderer::UpdateDescriptors()
{
    if (!m_descriptorPool)
        return;

    auto writeSet = [this](VkDescriptorSet descriptorSet, uint32_t frame, const Texture& diffuse, const Texture& mask)
    {
        if (!descriptorSet || !diffuse.view || !diffuse.sampler || !mask.view || !mask.sampler)
            return;

        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = m_uniformBuffers[frame].buffer;
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(UniformBlock);

        VkDescriptorImageInfo diffuseInfo{};
        diffuseInfo.sampler = diffuse.sampler;
        diffuseInfo.imageView = diffuse.view;
        diffuseInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo maskInfo{};
        maskInfo.sampler = mask.sampler;
        maskInfo.imageView = mask.view;
        maskInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        std::array<VkWriteDescriptorSet, 3> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = descriptorSet;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &bufferInfo;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = descriptorSet;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = &diffuseInfo;

        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = descriptorSet;
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[2].pImageInfo = &maskInfo;
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    };

    if (m_layers.empty())
    {
        for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
            writeSet(m_descriptorSets[frame], frame, m_baseTexture, m_fallbackMask);
        return;
    }

    if (m_layerDescriptorSets.size() != m_layers.size() * kFramesInFlight)
        return;

    for (size_t layerIndex = 0; layerIndex < m_layers.size(); ++layerIndex)
    {
        for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
        {
            writeSet(m_layerDescriptorSets[layerIndex * kFramesInFlight + frame],
                frame,
                m_layers[layerIndex].diffuse,
                m_layers[layerIndex].mask);
        }
    }
}

bool TerrainRenderer::CreatePipeline(VulkanDevice& device)
{
    if (!m_assets)
        return false;

    VkShaderModule vs = CreateShaderModule(m_device, *m_assets, "assets/shaders/terrain_vs.spv");
    VkShaderModule ps = CreateShaderModule(m_device, *m_assets, "assets/shaders/terrain_ps.spv");

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
    attributes[1].offset = offsetof(Vertex, texUv);
    attributes[2].location = 2;
    attributes[2].binding = 0;
    attributes[2].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[2].offset = offsetof(Vertex, maskUv);

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
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

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
    VkPushConstantRange pushConstant{};
    pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstant.offset = 0;
    pushConstant.size = sizeof(float) * 4u;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &pushConstant;
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

void TerrainRenderer::DestroyPipeline()
{
    if (m_pipeline)
        vkDestroyPipeline(m_device, m_pipeline, nullptr);
    m_pipeline = VK_NULL_HANDLE;

    if (m_pipelineLayout)
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    m_pipelineLayout = VK_NULL_HANDLE;
}

void TerrainRenderer::DestroyBuffer(Buffer& buffer)
{
    if (buffer.buffer)
        vkDestroyBuffer(m_device, buffer.buffer, nullptr);
    if (buffer.memory)
        vkFreeMemory(m_device, buffer.memory, nullptr);
    buffer = {};
}

void TerrainRenderer::DestroyTexture(Texture& texture)
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

void TerrainRenderer::DestroyTerrainLayers()
{
    for (TerrainLayer& layer : m_layers)
    {
        DestroyTexture(layer.diffuse);
        DestroyTexture(layer.mask);
    }
    m_layers.clear();
    m_layerDescriptorSets.clear();
}

void TerrainRenderer::UpdateUniform(uint32_t frameIndex, const WorldCamera& camera)
{
    const UniformBlock uniform{camera.viewProjection};
    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(m_device, m_uniformBuffers[frameIndex].memory, 0, sizeof(uniform), 0, &mapped));
    std::memcpy(mapped, &uniform, sizeof(uniform));
    vkUnmapMemory(m_device, m_uniformBuffers[frameIndex].memory);
}
