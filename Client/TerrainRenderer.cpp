#include "TerrainRenderer.h"

#include "Debug.h"
#include "asset/IAssetReader.h"
#include "map/MapData.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <utility>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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
    else if (oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
             newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
    {
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    else
    {
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    }

    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

bool CreateDeviceLocalImageArray(VulkanDevice& device, VkDevice vkDevice, uint32_t width, uint32_t height,
    uint32_t mipLevels, uint32_t arrayLayers, VkFormat format, VkImage& image, VkDeviceMemory& memory)
{
    VkImageCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    create.imageType = VK_IMAGE_TYPE_2D;
    create.format = format;
    create.extent = {width, height, 1};
    create.mipLevels = mipLevels;
    create.arrayLayers = arrayLayers;
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

void TransitionImageLayoutArray(VkCommandBuffer cmd, VkImage image, uint32_t mipLevels, uint32_t arrayLayers,
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
    barrier.subresourceRange.levelCount = mipLevels;
    barrier.subresourceRange.layerCount = arrayLayers;

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

std::array<float, 3> ZoneDebugColor(uint32_t zoneId)
{
    constexpr std::array<std::array<float, 3>, 12> kPalette = {{
        {0.00f, 0.45f, 0.70f}, // blue
        {0.90f, 0.62f, 0.00f}, // orange
        {0.00f, 0.62f, 0.45f}, // bluish green
        {0.80f, 0.47f, 0.65f}, // pink
        {0.34f, 0.71f, 0.91f}, // sky
        {0.94f, 0.89f, 0.26f}, // yellow
        {0.84f, 0.37f, 0.00f}, // vermillion
        {0.35f, 0.35f, 0.72f}, // indigo
        {0.56f, 0.80f, 0.22f}, // lime
        {0.64f, 0.36f, 0.20f}, // brown
        {0.58f, 0.40f, 0.74f}, // purple
        {0.10f, 0.70f, 0.80f}, // cyan
    }};
    return kPalette[zoneId % kPalette.size()];
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

uint32_t ReadU32LE(const uint8_t* data)
{
    return static_cast<uint32_t>(data[0]) |
        (static_cast<uint32_t>(data[1]) << 8) |
        (static_cast<uint32_t>(data[2]) << 16) |
        (static_cast<uint32_t>(data[3]) << 24);
}

void WriteI16LE(uint8_t* data, int16_t value)
{
    const uint16_t raw = static_cast<uint16_t>(value);
    data[0] = static_cast<uint8_t>(raw & 0xff);
    data[1] = static_cast<uint8_t>((raw >> 8) & 0xff);
}

void WriteU16LE(uint8_t* data, uint16_t value)
{
    data[0] = static_cast<uint8_t>(value & 0xff);
    data[1] = static_cast<uint8_t>((value >> 8) & 0xff);
}

void RemoveTemporaryFile(const std::filesystem::path& path)
{
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (ec)
        Tracenf("[TERRAIN-EDITOR] temp cleanup failed: %s (%s)",
            path.string().c_str(),
            ec.message().c_str());
}

#if defined(_WIN32)
std::string WindowsErrorMessage(DWORD error)
{
    char* message = nullptr;
    const DWORD length = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        0,
        reinterpret_cast<char*>(&message),
        0,
        nullptr);
    if (length == 0 || !message)
        return {};

    std::string result(message, length);
    LocalFree(message);
    while (!result.empty() && (result.back() == '\r' || result.back() == '\n' || result.back() == '.'))
        result.pop_back();
    return result;
}
#endif

bool AtomicReplace(const std::filesystem::path& temp, const std::filesystem::path& target)
{
#if defined(_WIN32)
    if (MoveFileExW(temp.wstring().c_str(),
                    target.wstring().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        return true;
    }

    const DWORD error = GetLastError();
    const std::string message = WindowsErrorMessage(error);
    Tracenf("[TERRAIN-EDITOR] atomic replace failed: %s (GetLastError=%lu%s%s)",
        target.string().c_str(),
        static_cast<unsigned long>(error),
        message.empty() ? "" : ": ",
        message.c_str());
    RemoveTemporaryFile(temp);
    return false;
#else
    std::error_code ec;
    std::filesystem::rename(temp, target, ec);
    if (!ec)
        return true;

    Tracenf("[TERRAIN-EDITOR] atomic replace failed: %s (%s)",
        target.string().c_str(),
        ec.message().c_str());
    RemoveTemporaryFile(temp);
    return false;
#endif
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
    LoadEditorConfig();

    const bool texture = CreateFallbackTexture(device);
    const bool mask = texture ? CreateFallbackSplatTextures(device) : false;
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
    DestroyBuffer(m_debugVertexBuffer);
    DestroyBuffer(m_debugIndexBuffer);
    DestroyBuffer(m_logicVertexBuffer);
    DestroyBuffer(m_logicIndexBuffer);
    DestroyTerrainLayers();
    m_tileIndices.clear();
    m_tileGridWidth = 0;
    m_tileGridHeight = 0;
    m_indexCount = 0;
    m_debugIndexCount = 0;
    m_spawnDebugIndexOffset = 0;
    m_spawnDebugIndexCount = 0;
    m_logicDebugIndexOffset = 0;
    m_logicDebugIndexCount = 0;
    m_zoneFillDebugRanges.clear();
    m_zoneBorderDebugRanges.clear();
    m_zoneLabelDebugRanges.clear();

    if (!CreateMapBuffers(device, mapDirectory, serverX, serverY))
    {
        Tracen("[TERRAIN-MAP] LoadMap failed; restoring flat fallback terrain");
        m_mapLoaded = false;
        m_heightCmGrid.clear();
        m_attributes.clear();
        m_splatABytes.clear();
        m_splatBBytes.clear();
        m_splatWidth = 0;
        m_splatHeight = 0;
        m_chunkSplatWidth = 0;
        m_chunkSplatHeight = 0;
        m_undoStack.clear();
        const bool flat = CreateFlatBuffers(device);
        CreateDescriptors();
        return flat;
    }

    CreateDescriptors();
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

    if (m_walkabilityDebug && m_debugIndexCount > 0 && m_debugVertexBuffer.buffer && m_debugIndexBuffer.buffer)
    {
        TerrainPushConstants push{{1.0f, 1.0f, 1.0f, 0.0f}};
        vkCmdPushConstants(cmd, m_pipelineLayout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(push), &push);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
            0, 1, &m_descriptorSets[frameIndex], 0, nullptr);
        vkCmdBindVertexBuffers(cmd, 0, 1, &m_debugVertexBuffer.buffer, &offset);
        vkCmdBindIndexBuffer(cmd, m_debugIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, m_debugIndexCount, 1, 0, 0, 0);
    }

    if (m_walkabilityDebug && m_mapLoaded && m_mapEditorOpen && m_editorBrushVisible)
    {
        TerrainPushConstants brushPush{{m_editorBrushLocalX,
                                        m_editorBrushLocalZ,
                                        6.0f,
                                        m_editorBrushRadiusMeters}};
        vkCmdPushConstants(cmd, m_pipelineLayout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(brushPush), &brushPush);
        vkCmdBindVertexBuffers(cmd, 0, 1, &m_vertexBuffer.buffer, &offset);
        vkCmdBindIndexBuffer(cmd, m_indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
            0, 1, &m_descriptorSets[frameIndex], 0, nullptr);
        vkCmdDrawIndexed(cmd, m_indexCount, 1, 0, 0, 0);
    }

    const bool hasZoneDebug = !m_zoneFillDebugRanges.empty() ||
        !m_zoneBorderDebugRanges.empty() ||
        !m_zoneLabelDebugRanges.empty();
    if (m_walkabilityDebug && (hasZoneDebug || m_logicDebugIndexCount > 0 || m_spawnDebugIndexCount > 0) &&
        m_logicVertexBuffer.buffer && m_logicIndexBuffer.buffer)
    {
        vkCmdBindVertexBuffers(cmd, 0, 1, &m_logicVertexBuffer.buffer, &offset);
        vkCmdBindIndexBuffer(cmd, m_logicIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
            0, 1, &m_descriptorSets[frameIndex], 0, nullptr);

        auto drawDebugRange = [this, cmd](const DebugDrawRange& range, float mode)
        {
            if (range.indexCount == 0)
                return;

            TerrainPushConstants push{{range.color[0], range.color[1], mode, range.color[2]}};
            vkCmdPushConstants(cmd, m_pipelineLayout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(push), &push);
            vkCmdDrawIndexed(cmd, range.indexCount, 1, range.indexOffset, 0, 0);
        };

        for (const DebugDrawRange& range : m_zoneFillDebugRanges)
            drawDebugRange(range, 4.0f);

        for (const DebugDrawRange& range : m_zoneBorderDebugRanges)
            drawDebugRange(range, 5.0f);

        for (const DebugDrawRange& range : m_zoneLabelDebugRanges)
            drawDebugRange(range, 5.0f);

        TerrainPushConstants warpPush{{1.0f, 1.0f, 3.0f, 0.0f}};
        vkCmdPushConstants(cmd, m_pipelineLayout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(warpPush), &warpPush);
        vkCmdDrawIndexed(cmd, m_logicDebugIndexCount, 1, m_logicDebugIndexOffset, 0, 0);

        if (m_spawnDebugIndexCount > 0)
        {
            TerrainPushConstants spawnPush{{1.0f, 1.0f, 2.0f, 0.0f}};
            vkCmdPushConstants(cmd, m_pipelineLayout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(spawnPush), &spawnPush);
            vkCmdDrawIndexed(cmd, m_spawnDebugIndexCount, 1, m_spawnDebugIndexOffset, 0, 0);
        }
    }

    if (!loggedDraw)
    {
        Tracenf("[TERRAIN] Render: %s indexCount=%u debugIndexCount=%u layers=%zu camera eye=(%.2f,%.2f,%.2f) target=(%.2f,%.2f,%.2f)",
            m_mapLoaded ? "clean-room heightmap" : "flat 100m ground",
            m_indexCount,
            m_debugIndexCount,
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

void TerrainRenderer::ToggleWalkabilityDebug()
{
    m_walkabilityDebug = !m_walkabilityDebug;
    if (!m_walkabilityDebug)
    {
        m_editorRaiseHeld = false;
        m_editorLowerHeld = false;
        SetMapEditorOpen(false);
    }
    Tracenf("[TERRAIN-DEBUG] walkability overlay %s", m_walkabilityDebug ? "ON" : "OFF");
}

void TerrainRenderer::SetMapEditorOpen(bool open)
{
    if (m_mapEditorOpen == open)
        return;
    m_mapEditorOpen = open;
    m_editorLmbHeld = false;
    m_editorBrushVisible = false;
    if (!open && m_editorStrokeActive)
        EndEditorStroke();
}

void TerrainRenderer::SetMapEditorSettings(const MapEditorSettings& settings)
{
    m_editorTool = settings.tool;
    m_editorBrushRadiusMeters = std::clamp(settings.brushRadiusMeters, 1.0f, 50.0f);
    m_editorBrushStrength = std::clamp(settings.brushStrength, 0.1f, 5.0f);
    m_editorTextureSlot = std::min<std::uint32_t>(settings.textureSlot, 7u);
}

void TerrainRenderer::RequestEditorSave()
{
    m_editorSaveRequested = true;
}

void TerrainRenderer::RequestEditorReload()
{
    m_editorReloadRequested = true;
}

void TerrainRenderer::RequestEditorUndo()
{
    m_editorUndoRequested = true;
}

bool TerrainRenderer::HandleEditorInput(const InputEvent& event)
{
    if (event.type == InputEvent::MouseMove)
    {
        m_editorCursorX = event.x;
        m_editorCursorY = event.y;
        return m_mapEditorOpen;
    }

    if (event.type == InputEvent::MouseDown || event.type == InputEvent::MouseUp)
    {
        if (!m_mapEditorOpen)
            return false;
        if (event.button == MouseButton_Left)
        {
            m_editorCursorX = event.x;
            m_editorCursorY = event.y;
            if (event.type == InputEvent::MouseDown)
                m_editorLmbHeld = true;
            else
            {
                m_editorLmbHeld = false;
                EndEditorStroke();
            }
            return true;
        }
        return false;
    }

    const bool keyEvent = event.type == InputEvent::KeyDown || event.type == InputEvent::KeyUp;
    if (!keyEvent)
        return false;

    const bool pressed = event.type == InputEvent::KeyDown;
    if (event.key == Key_Control)
    {
        m_editorCtrlHeld = pressed;
        return false;
    }

    if (m_mapEditorOpen)
    {
        if (event.key == Key_F7)
        {
            if (pressed)
                m_editorSaveRequested = true;
            return true;
        }
        if (event.key == Key_F8)
        {
            if (pressed)
                m_editorReloadRequested = true;
            return true;
        }
        if (event.key == Key_Z && pressed && m_editorCtrlHeld)
        {
            m_editorUndoRequested = true;
            return true;
        }
    }

    if (!m_walkabilityDebug)
    {
        if (event.key == Key_F5 || event.key == Key_F6)
        {
            if (!pressed)
            {
                if (event.key == Key_F5) m_editorRaiseHeld = false;
                if (event.key == Key_F6) m_editorLowerHeld = false;
            }
        }
        return false;
    }

    switch (event.key)
    {
    case Key_F5:
        m_editorRaiseHeld = pressed;
        return true;
    case Key_F6:
        m_editorLowerHeld = pressed;
        return true;
    case Key_F7:
        if (pressed)
            m_editorSaveRequested = true;
        return true;
    case Key_F8:
        if (pressed)
            m_editorReloadRequested = true;
        return true;
    default:
        return false;
    }
}

void TerrainRenderer::UpdateEditor(VulkanDevice& device,
                                   double deltaSeconds,
                                   const WorldCamera& camera,
                                   uint32_t viewportWidth,
                                   uint32_t viewportHeight)
{
    if (m_mapEditorOpen && m_walkabilityDebug && m_mapLoaded)
        RaycastEditorBrush(camera, viewportWidth, viewportHeight);
    else
    {
        m_editorBrushVisible = false;
        if (m_editorStrokeActive)
            EndEditorStroke();
    }

    if (m_editorReloadRequested)
    {
        m_editorReloadRequested = false;
        ReloadCurrentMap(device);
        return;
    }

    if (m_editorSaveRequested)
    {
        m_editorSaveRequested = false;
        SaveDirtyChunks();
    }

    if (m_editorUndoRequested)
    {
        m_editorUndoRequested = false;
        UndoLastEditorStroke(device);
    }

    if (!m_walkabilityDebug || !m_mapLoaded)
        return;

    if (m_mapEditorOpen)
    {
        if (m_editorLmbHeld && m_editorBrushVisible)
            ApplyEditorBrush(device, deltaSeconds);
        if (m_editorSplatGpuDirty)
            RefreshSplatTextures(device);
        return;
    }

    if (m_editorRaiseHeld)
        ApplyLegacyHeightBrush(device, 1.0f, deltaSeconds);
    if (m_editorLowerHeld)
        ApplyLegacyHeightBrush(device, -1.0f, deltaSeconds);
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
    DestroyBuffer(m_debugVertexBuffer);
    DestroyBuffer(m_debugIndexBuffer);
    DestroyBuffer(m_logicVertexBuffer);
    DestroyBuffer(m_logicIndexBuffer);
    for (Buffer& buffer : m_uniformBuffers)
        DestroyBuffer(buffer);
    DestroyTerrainLayers();
    DestroyTexture(m_baseTexture);
    DestroyTexture(m_fallbackMask);
    DestroyTexture(m_splatA);
    DestroyTexture(m_splatB);

    m_layerDescriptorSets.clear();
    m_tileIndices.clear();
    m_tileGridWidth = 0;
    m_tileGridHeight = 0;
    m_indexCount = 0;
    m_debugIndexCount = 0;
    m_spawnDebugIndexOffset = 0;
    m_spawnDebugIndexCount = 0;
    m_logicDebugIndexOffset = 0;
    m_logicDebugIndexCount = 0;
    m_zoneFillDebugRanges.clear();
    m_zoneBorderDebugRanges.clear();
    m_zoneLabelDebugRanges.clear();
    m_heightGridWidth = 0;
    m_heightGridHeight = 0;
    m_splatWidth = 0;
    m_splatHeight = 0;
    m_chunkSplatWidth = 0;
    m_chunkSplatHeight = 0;
    m_mapSizeX = 0;
    m_mapSizeY = 0;
    m_chunkSizeCells = 0;
    m_spawnLocalXcm = 0.0f;
    m_spawnLocalYcm = 0.0f;
    m_spawnHeightCm = 0.0f;
    m_mapLoaded = false;
    m_editorRaiseHeld = false;
    m_editorLowerHeld = false;
    m_editorSaveRequested = false;
    m_editorReloadRequested = false;
    m_editorUndoRequested = false;
    m_mapEditorOpen = false;
    m_editorLmbHeld = false;
    m_editorStrokeActive = false;
    m_editorBrushVisible = false;
    m_editorSplatGpuDirty = false;
    m_loadedMapDirectory.clear();
    m_loadedServerX = 0;
    m_loadedServerY = 0;
    m_heightCmGrid.clear();
    m_attributes.clear();
    m_splatABytes.clear();
    m_splatBBytes.clear();
    m_dirtyChunkTexels.clear();
    m_heightUndoRecorded.clear();
    m_splatUndoRecorded.clear();
    m_currentUndo = {};
    m_undoStack.clear();
    m_device = VK_NULL_HANDLE;
    m_assets = nullptr;
}

bool TerrainRenderer::CreateBuffers(VulkanDevice& device)
{
    if (CreateMapBuffers(device, "assets/Maps/test_zone", 0, 0)) {
        return true;
    }

    Tracen("[TERRAIN-MAP] clean-room test zone missing; using flat fallback terrain");
    return CreateFlatBuffers(device);
}

bool TerrainRenderer::EnsureUniformBuffers(VulkanDevice& device)
{
    for (Buffer& buffer : m_uniformBuffers)
    {
        if (buffer.buffer && buffer.memory)
            continue;

        DestroyBuffer(buffer);
        CreateHostVisibleBuffer(device, m_device, sizeof(UniformBlock),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, nullptr, buffer);
    }

    for (const Buffer& buffer : m_uniformBuffers)
    {
        if (!buffer.buffer || !buffer.memory)
        {
            Tracen("[TERRAIN] uniform buffer creation failed");
            return false;
        }
    }
    return true;
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

    return EnsureUniformBuffers(device);
}

bool TerrainRenderer::UploadRgbaTexture2D(VulkanDevice& device,
    const std::string& name,
    uint32_t width,
    uint32_t height,
    const std::vector<std::uint8_t>& pixels,
    VkSamplerAddressMode addressMode,
    Texture& out)
{
    if (width == 0 || height == 0 || pixels.size() != static_cast<size_t>(width) * height * 4u)
        return false;

    DestroyTexture(out);
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    vkGetDeviceQueue(m_device, device.GetGraphicsQueueFamily(), 0, &graphicsQueue);

    CreateDeviceLocalImage(device, m_device, width, height, 1, VK_FORMAT_R8G8B8A8_UNORM, out.image, out.memory);
    Buffer staging{};
    CreateHostVisibleBuffer(device, m_device, pixels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, pixels.data(), staging);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {width, height, 1};

    VkCommandPool uploadPool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = BeginOneTimeCommands(m_device, device.GetGraphicsQueueFamily(), uploadPool);
    TransitionImageLayout(cmd, out.image, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(cmd, staging.buffer, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    TransitionImageLayout(cmd, out.image, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EndOneTimeCommands(m_device, graphicsQueue, uploadPool, cmd);
    DestroyBuffer(staging);

    out.format = VK_FORMAT_R8G8B8A8_UNORM;
    out.width = width;
    out.height = height;
    out.mipLevels = 1;
    out.name = name;

    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = out.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = out.format;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(m_device, &view, nullptr, &out.view));

    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = addressMode;
    sampler.addressModeV = addressMode;
    sampler.addressModeW = addressMode;
    sampler.maxLod = 1.0f;
    VK_CHECK(vkCreateSampler(m_device, &sampler, nullptr, &out.sampler));
    return true;
}

bool TerrainRenderer::UpdateRgbaTexture2D(VulkanDevice& device,
                                          Texture& texture,
                                          const std::vector<std::uint8_t>& pixels)
{
    if (!texture.image || texture.width == 0 || texture.height == 0 ||
        pixels.size() != static_cast<size_t>(texture.width) * texture.height * 4u)
        return false;

    VkQueue graphicsQueue = VK_NULL_HANDLE;
    vkGetDeviceQueue(m_device, device.GetGraphicsQueueFamily(), 0, &graphicsQueue);

    Buffer staging{};
    CreateHostVisibleBuffer(device, m_device, pixels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, pixels.data(), staging);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {texture.width, texture.height, 1};

    VkCommandPool uploadPool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = BeginOneTimeCommands(m_device, device.GetGraphicsQueueFamily(), uploadPool);
    TransitionImageLayout(cmd, texture.image, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(cmd, staging.buffer, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    TransitionImageLayout(cmd, texture.image, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EndOneTimeCommands(m_device, graphicsQueue, uploadPool, cmd);
    DestroyBuffer(staging);
    return true;
}

bool TerrainRenderer::UploadRgbaTextureArray(VulkanDevice& device,
    const std::string& name,
    uint32_t width,
    uint32_t height,
    uint32_t layers,
    const std::vector<std::uint8_t>& pixels,
    Texture& out)
{
    if (width == 0 || height == 0 || layers == 0 ||
        pixels.size() != static_cast<size_t>(width) * height * layers * 4u)
        return false;

    DestroyTexture(out);
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    vkGetDeviceQueue(m_device, device.GetGraphicsQueueFamily(), 0, &graphicsQueue);

    CreateDeviceLocalImageArray(device, m_device, width, height, 1, layers,
        VK_FORMAT_R8G8B8A8_SRGB, out.image, out.memory);
    Buffer staging{};
    CreateHostVisibleBuffer(device, m_device, pixels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, pixels.data(), staging);

    std::vector<VkBufferImageCopy> regions;
    regions.reserve(layers);
    const VkDeviceSize layerSize = static_cast<VkDeviceSize>(width) * height * 4u;
    for (uint32_t layer = 0; layer < layers; ++layer)
    {
        VkBufferImageCopy region{};
        region.bufferOffset = layerSize * layer;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.baseArrayLayer = layer;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {width, height, 1};
        regions.push_back(region);
    }

    VkCommandPool uploadPool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = BeginOneTimeCommands(m_device, device.GetGraphicsQueueFamily(), uploadPool);
    TransitionImageLayoutArray(cmd, out.image, 1, layers, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(cmd, staging.buffer, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        static_cast<uint32_t>(regions.size()), regions.data());
    TransitionImageLayoutArray(cmd, out.image, 1, layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EndOneTimeCommands(m_device, graphicsQueue, uploadPool, cmd);
    DestroyBuffer(staging);

    out.format = VK_FORMAT_R8G8B8A8_SRGB;
    out.width = width;
    out.height = height;
    out.mipLevels = 1;
    out.name = name;

    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = out.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    view.format = out.format;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = layers;
    VK_CHECK(vkCreateImageView(m_device, &view, nullptr, &out.view));

    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.maxLod = 1.0f;
    VK_CHECK(vkCreateSampler(m_device, &sampler, nullptr, &out.sampler));
    return true;
}

bool TerrainRenderer::CreateMapBuffers(VulkanDevice& device, const std::string& mapDirectory, int32_t serverX, int32_t serverY)
{
    if (!m_assets) {
        return false;
    }
    m_zoneFillDebugRanges.clear();
    m_zoneBorderDebugRanges.clear();
    m_zoneLabelDebugRanges.clear();

    const auto field = mx::map::LoadHeightField(
        [this](std::string_view path) {
            return m_assets->ReadAll(path);
        },
        mapDirectory);
    if (!field) {
        Tracenf("[TERRAIN-MAP] failed to load clean-room map: %s", mapDirectory.c_str());
        return false;
    }

    m_heightGridWidth = field->width_vertices;
    m_heightGridHeight = field->height_vertices;
    m_mapSizeX = field->manifest.world_size_cells;
    m_mapSizeY = field->manifest.world_size_cells;
    m_chunkSizeCells = field->manifest.chunk_size_cells;
    m_cellScaleMeters = field->manifest.cell_size_meters;
    m_loadedMapDirectory = mapDirectory;
    m_loadedServerX = serverX;
    m_loadedServerY = serverY;
    m_spawnLocalXcm = static_cast<float>(serverX) * 100.0f;
    m_spawnLocalYcm = static_cast<float>(serverY) * 100.0f;
    m_heightCmGrid.resize(field->heights_cm.size());
    m_attributes = field->attributes;
    const uint32_t chunksX = m_chunkSizeCells > 0 ? (m_mapSizeX + m_chunkSizeCells - 1u) / m_chunkSizeCells : 0;
    const uint32_t chunksY = m_chunkSizeCells > 0 ? (m_mapSizeY + m_chunkSizeCells - 1u) / m_chunkSizeCells : 0;
    m_dirtyChunkTexels.assign(static_cast<size_t>(chunksX) * chunksY, 0);
    m_splatWidth = field->splat_width;
    m_splatHeight = field->splat_height;
    m_chunkSplatWidth = chunksX > 0 ? m_splatWidth / chunksX : 0;
    m_chunkSplatHeight = chunksY > 0 ? m_splatHeight / chunksY : 0;
    m_splatABytes = field->splat_a_rgba8;
    m_splatBBytes = field->splat_b_rgba8;
    m_heightUndoRecorded.assign(m_heightCmGrid.size(), 0);
    m_splatUndoRecorded.assign(static_cast<size_t>(m_splatWidth) * m_splatHeight, 0);
    m_currentUndo = {};
    m_undoStack.clear();
    m_editorSplatGpuDirty = false;
    if (!field->splat_a_rgba8.empty() && !field->splat_b_rgba8.empty())
    {
        UploadRgbaTexture2D(device, "splat_a", field->splat_width, field->splat_height,
            field->splat_a_rgba8, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, m_splatA);
        UploadRgbaTexture2D(device, "splat_b", field->splat_width, field->splat_height,
            field->splat_b_rgba8, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, m_splatB);
        if (!LoadTerrainPalette(device, field->manifest, mapDirectory))
            Tracen("[TERRAIN-PALETTE] configured palette load failed; using generated fallback palette");
        Tracenf("[TERRAIN-SPLAT] loaded atlas %ux%u from mxchunk RGBA8 sections",
            field->splat_width,
            field->splat_height);
    }
    float minHeightCm = std::numeric_limits<float>::max();
    float maxHeightCm = -std::numeric_limits<float>::max();
    for (std::size_t i = 0; i < field->heights_cm.size(); ++i) {
        m_heightCmGrid[i] = static_cast<float>(field->heights_cm[i]);
        minHeightCm = std::min(minHeightCm, m_heightCmGrid[i]);
        maxHeightCm = std::max(maxHeightCm, m_heightCmGrid[i]);
    }

    m_spawnHeightCm = BilinearHeightCm(m_heightCmGrid, m_heightGridWidth, m_heightGridHeight,
        m_spawnLocalXcm, m_spawnLocalYcm, m_cellScaleMeters * 100.0f);

    std::vector<Vertex> vertices;
    vertices.reserve(m_heightCmGrid.size());
    for (uint32_t gy = 0; gy < m_heightGridHeight; ++gy)
    {
        for (uint32_t gx = 0; gx < m_heightGridWidth; ++gx)
        {
            const float localXcm = static_cast<float>(gx) * m_cellScaleMeters * 100.0f;
            const float localYcm = static_cast<float>(gy) * m_cellScaleMeters * 100.0f;
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

    std::vector<Vertex> debugVertices;
    std::vector<uint32_t> debugIndices;
    if (!m_attributes.empty())
    {
        for (uint32_t cy = 0; cy < m_mapSizeY; ++cy)
        {
            for (uint32_t cx = 0; cx < m_mapSizeX; ++cx)
            {
                const auto attr = m_attributes[static_cast<size_t>(cy) * m_mapSizeX + cx];
                if ((attr & mx::map::HeightField::kAttributeBlocked) == 0)
                    continue;

                const uint32_t base = static_cast<uint32_t>(debugVertices.size());
                for (uint32_t corner = 0; corner < 4; ++corner)
                {
                    const uint32_t gx = cx + ((corner == 1 || corner == 2) ? 1u : 0u);
                    const uint32_t gy = cy + ((corner >= 2) ? 1u : 0u);
                    const float localXcm = static_cast<float>(gx) * m_cellScaleMeters * 100.0f;
                    const float localYcm = static_cast<float>(gy) * m_cellScaleMeters * 100.0f;
                    const float heightCm = m_heightCmGrid[static_cast<size_t>(gy) * m_heightGridWidth + gx] + 6.0f;

                    Vertex vertex{};
                    vertex.position[0] = (localXcm - m_spawnLocalXcm) * 0.01f;
                    vertex.position[1] = heightCm * 0.01f;
                    vertex.position[2] = -(localYcm - m_spawnLocalYcm) * 0.01f;
                    vertex.texUv[0] = 0.0f;
                    vertex.texUv[1] = 0.0f;
                    vertex.maskUv[0] = 0.0f;
                    vertex.maskUv[1] = 0.0f;
                    debugVertices.push_back(vertex);
                }
                debugIndices.push_back(base + 0);
                debugIndices.push_back(base + 1);
                debugIndices.push_back(base + 2);
                debugIndices.push_back(base + 0);
                debugIndices.push_back(base + 2);
                debugIndices.push_back(base + 3);
            }
        }
    }

    auto makeRectOverlay = [this](std::vector<Vertex>& outVertices,
                                  std::vector<uint32_t>& outIndices,
                                  const mx::map::Rect& rect,
                                  float liftCm) {
        const uint32_t base = static_cast<uint32_t>(outVertices.size());
        const std::array<std::pair<float, float>, 4> corners = {{{rect.min_x, rect.min_y},
                                                                  {rect.max_x, rect.min_y},
                                                                  {rect.max_x, rect.max_y},
                                                                  {rect.min_x, rect.max_y}}};
        for (const auto& [worldX, worldY] : corners) {
            const float localXcm = worldX * 100.0f;
            const float localYcm = worldY * 100.0f;
            const float heightCm = BilinearHeightCm(m_heightCmGrid,
                                      m_heightGridWidth,
                                      m_heightGridHeight,
                                      localXcm,
                                      localYcm,
                                      m_cellScaleMeters * 100.0f) +
                                  liftCm;

            Vertex vertex{};
            vertex.position[0] = (localXcm - m_spawnLocalXcm) * 0.01f;
            vertex.position[1] = heightCm * 0.01f;
            vertex.position[2] = -(localYcm - m_spawnLocalYcm) * 0.01f;
            outVertices.push_back(vertex);
        }
        outIndices.push_back(base + 0);
        outIndices.push_back(base + 1);
        outIndices.push_back(base + 2);
        outIndices.push_back(base + 0);
        outIndices.push_back(base + 2);
        outIndices.push_back(base + 3);
    };

    auto makeZoneIdLabel = [&makeRectOverlay](std::vector<Vertex>& outVertices,
                                              std::vector<uint32_t>& outIndices,
                                              const mx::map::Rect& zoneBounds,
                                              uint32_t zoneId) {
        constexpr bool digits[10][7] = {
            {true, true, true, true, true, true, false},
            {false, true, true, false, false, false, false},
            {true, true, false, true, true, false, true},
            {true, true, true, true, false, false, true},
            {false, true, true, false, false, true, true},
            {true, false, true, true, false, true, true},
            {true, false, true, true, true, true, true},
            {true, true, true, false, false, false, false},
            {true, true, true, true, true, true, true},
            {true, true, true, true, false, true, true},
        };

        const std::string text = std::to_string(zoneId);
        constexpr float digitWidth = 9.0f;
        constexpr float digitHeight = 15.0f;
        constexpr float thickness = 1.7f;
        constexpr float spacing = 2.2f;
        const float totalWidth =
            static_cast<float>(text.size()) * digitWidth +
            static_cast<float>(text.empty() ? 0 : text.size() - 1) * spacing;
        const float startX = zoneBounds.CenterX() - totalWidth * 0.5f;
        const float baseY = zoneBounds.CenterY() - digitHeight * 0.5f;

        auto addSegment = [&](float x, float y, int segment) {
            const bool* d = digits[static_cast<std::size_t>(std::clamp(segment, 0, 9))];
            auto rect = [&](float minX, float minY, float maxX, float maxY) {
                makeRectOverlay(outVertices, outIndices, {minX, minY, maxX, maxY}, 24.0f);
            };
            if (d[0])
                rect(x, y + digitHeight - thickness, x + digitWidth, y + digitHeight);
            if (d[1])
                rect(x + digitWidth - thickness, y + digitHeight * 0.5f, x + digitWidth, y + digitHeight);
            if (d[2])
                rect(x + digitWidth - thickness, y, x + digitWidth, y + digitHeight * 0.5f);
            if (d[3])
                rect(x, y, x + digitWidth, y + thickness);
            if (d[4])
                rect(x, y, x + thickness, y + digitHeight * 0.5f);
            if (d[5])
                rect(x, y + digitHeight * 0.5f, x + thickness, y + digitHeight);
            if (d[6])
                rect(x, y + digitHeight * 0.5f - thickness * 0.5f,
                     x + digitWidth,
                     y + digitHeight * 0.5f + thickness * 0.5f);
        };

        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] < '0' || text[i] > '9')
                continue;
            addSegment(startX + static_cast<float>(i) * (digitWidth + spacing),
                       baseY,
                       text[i] - '0');
        }
    };

    auto makeRange = [](uint32_t begin, uint32_t end, std::array<float, 3> color) {
        DebugDrawRange range{};
        range.indexOffset = begin;
        range.indexCount = end - begin;
        range.color[0] = color[0];
        range.color[1] = color[1];
        range.color[2] = color[2];
        return range;
    };

    std::vector<Vertex> logicVertices;
    std::vector<uint32_t> logicIndices;
    if (const auto logic = mx::map::LoadWorldLogic(
            [this](std::string_view path) {
                return m_assets->ReadAll(path);
            },
            mapDirectory))
    {
        for (const auto& zone : logic->zones) {
            const auto zoneColor = ZoneDebugColor(zone.id);
            uint32_t begin = static_cast<uint32_t>(logicIndices.size());
            makeRectOverlay(logicVertices, logicIndices, zone.bounds, 7.0f);
            m_zoneFillDebugRanges.push_back(
                makeRange(begin, static_cast<uint32_t>(logicIndices.size()), zoneColor));

            const float inset = 1.0f;
            begin = static_cast<uint32_t>(logicIndices.size());
            makeRectOverlay(logicVertices,
                            logicIndices,
                            {zone.bounds.min_x,
                             zone.bounds.min_y,
                             zone.bounds.max_x,
                             zone.bounds.min_y + inset},
                            9.0f);
            makeRectOverlay(logicVertices,
                            logicIndices,
                            {zone.bounds.min_x,
                             zone.bounds.max_y - inset,
                             zone.bounds.max_x,
                             zone.bounds.max_y},
                            9.0f);
            makeRectOverlay(logicVertices,
                            logicIndices,
                            {zone.bounds.min_x,
                             zone.bounds.min_y,
                             zone.bounds.min_x + inset,
                             zone.bounds.max_y},
                            9.0f);
            makeRectOverlay(logicVertices,
                            logicIndices,
                            {zone.bounds.max_x - inset,
                             zone.bounds.min_y,
                             zone.bounds.max_x,
                             zone.bounds.max_y},
                            9.0f);
            m_zoneBorderDebugRanges.push_back(
                makeRange(begin, static_cast<uint32_t>(logicIndices.size()), zoneColor));

            begin = static_cast<uint32_t>(logicIndices.size());
            makeZoneIdLabel(logicVertices, logicIndices, zone.bounds, zone.id);
            m_zoneLabelDebugRanges.push_back(
                makeRange(begin, static_cast<uint32_t>(logicIndices.size()), zoneColor));
        }

        m_logicDebugIndexOffset = static_cast<uint32_t>(logicIndices.size());
        for (const auto& warp : logic->warps)
            makeRectOverlay(logicVertices, logicIndices, warp.source, 12.0f);
        m_logicDebugIndexCount = static_cast<uint32_t>(logicIndices.size()) - m_logicDebugIndexOffset;

        m_spawnDebugIndexOffset = static_cast<uint32_t>(logicIndices.size());
        for (const auto& spawn : logic->spawns)
            makeRectOverlay(logicVertices, logicIndices, spawn.bounds, 15.0f);
        m_spawnDebugIndexCount = static_cast<uint32_t>(logicIndices.size()) - m_spawnDebugIndexOffset;

        Tracenf("[TERRAIN-DEBUG] worldlogic overlay built: zones=%zu spawns=%zu warps=%zu",
            logic->zones.size(),
            logic->spawns.size(),
            logic->warps.size());
    }

    m_indexCount = static_cast<uint32_t>(indices.size());
    CreateHostVisibleBuffer(device, m_device, sizeof(Vertex) * vertices.size(),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertices.data(), m_vertexBuffer);
    CreateHostVisibleBuffer(device, m_device, sizeof(uint32_t) * indices.size(),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indices.data(), m_indexBuffer);
    m_debugIndexCount = static_cast<uint32_t>(debugIndices.size());
    if (!debugVertices.empty() && !debugIndices.empty())
    {
        CreateHostVisibleBuffer(device, m_device, sizeof(Vertex) * debugVertices.size(),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, debugVertices.data(), m_debugVertexBuffer);
        CreateHostVisibleBuffer(device, m_device, sizeof(uint32_t) * debugIndices.size(),
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT, debugIndices.data(), m_debugIndexBuffer);
    }
    if (!logicVertices.empty() && !logicIndices.empty())
    {
        CreateHostVisibleBuffer(device, m_device, sizeof(Vertex) * logicVertices.size(),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, logicVertices.data(), m_logicVertexBuffer);
        CreateHostVisibleBuffer(device, m_device, sizeof(uint32_t) * logicIndices.size(),
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT, logicIndices.data(), m_logicIndexBuffer);
    }
    if (!EnsureUniformBuffers(device)) {
        return false;
    }

    m_mapLoaded = true;
    Tracenf("[TERRAIN-MAP] loaded clean-room map dir=%s world=%s sizeCells=%u chunkCells=%u grid=%ux%u spawnServer=(%d,%d) spawnLocalCm=(%.0f,%.0f) spawnHeightCm=%.1f heightCm=%.1f..%.1f vertices=%zu indices=%zu",
        mapDirectory.c_str(),
        field->manifest.world_id.c_str(),
        m_mapSizeX,
        field->manifest.chunk_size_cells,
        field->manifest.zone_grid_x,
        field->manifest.zone_grid_y,
        serverX,
        serverY,
        m_spawnLocalXcm,
        m_spawnLocalYcm,
        m_spawnHeightCm,
        minHeightCm,
        maxHeightCm,
        vertices.size(),
        indices.size());
    if (m_debugIndexCount > 0)
        Tracenf("[TERRAIN-DEBUG] blocked cell overlay built: cells=%u indices=%u",
            m_debugIndexCount / 6,
            m_debugIndexCount);
    return true;
}

void TerrainRenderer::LoadEditorConfig()
{
    m_editorBrushRadiusMeters = 5.0f;
    m_editorBrushStrength = 1.0f;
    if (!m_assets)
        return;

    auto text = m_assets->ReadText("assets/mmorpg.conf");
    if (!text)
        return;

    std::istringstream file(*text);
    std::string key;
    while (file >> key)
    {
        const std::string lower = LowerCopy(key);
        if (lower == "map_editor.brush_radius_meters")
            file >> m_editorBrushRadiusMeters;
        else if (lower == "map_editor.brush_strength_meters_per_second")
            file >> m_editorBrushStrength;
        else
        {
            std::string rest;
            std::getline(file, rest);
        }
    }
    m_editorBrushRadiusMeters = std::clamp(m_editorBrushRadiusMeters, 0.5f, 50.0f);
    m_editorBrushStrength = std::clamp(m_editorBrushStrength, 0.05f, 10.0f);
}

void TerrainRenderer::ApplyLegacyHeightBrush(VulkanDevice& device, float sign, double deltaSeconds)
{
    (void)device;
    if (!m_mapLoaded || m_heightCmGrid.empty() || !m_vertexBuffer.memory ||
        m_heightGridWidth < 2 || m_heightGridHeight < 2 || m_chunkSizeCells == 0)
        return;

    const float centerXcm = m_spawnLocalXcm + m_editorBrushLocalX * 100.0f;
    const float centerYcm = m_spawnLocalYcm - m_editorBrushLocalZ * 100.0f;
    const float centerGridX = centerXcm / (m_cellScaleMeters * 100.0f);
    const float centerGridY = centerYcm / (m_cellScaleMeters * 100.0f);
    if (centerGridX < 0.0f || centerGridY < 0.0f ||
        centerGridX > static_cast<float>(m_heightGridWidth - 1u) ||
        centerGridY > static_cast<float>(m_heightGridHeight - 1u))
        return;

    const uint32_t chunkX = std::min(static_cast<uint32_t>(centerGridX) / m_chunkSizeCells,
                                     (m_mapSizeX - 1u) / m_chunkSizeCells);
    const uint32_t chunkY = std::min(static_cast<uint32_t>(centerGridY) / m_chunkSizeCells,
                                     (m_mapSizeY - 1u) / m_chunkSizeCells);
    const uint32_t chunkMinX = chunkX * m_chunkSizeCells;
    const uint32_t chunkMinY = chunkY * m_chunkSizeCells;
    const uint32_t chunkMaxX = std::min(chunkMinX + m_chunkSizeCells, m_heightGridWidth - 1u);
    const uint32_t chunkMaxY = std::min(chunkMinY + m_chunkSizeCells, m_heightGridHeight - 1u);

    const float radiusCells = m_editorBrushRadiusMeters / std::max(m_cellScaleMeters, 0.001f);
    const uint32_t minX = std::max(chunkMinX, static_cast<uint32_t>(std::max(0.0f, std::floor(centerGridX - radiusCells))));
    const uint32_t minY = std::max(chunkMinY, static_cast<uint32_t>(std::max(0.0f, std::floor(centerGridY - radiusCells))));
    const uint32_t maxX = std::min(chunkMaxX, static_cast<uint32_t>(std::ceil(centerGridX + radiusCells)));
    const uint32_t maxY = std::min(chunkMaxY, static_cast<uint32_t>(std::ceil(centerGridY + radiusCells)));

    const float deltaCenterCm =
        sign * m_editorBrushStrength * static_cast<float>(deltaSeconds) * 100.0f;
    if (std::abs(deltaCenterCm) < 0.0001f)
        return;

    void* mapped = nullptr;
    const VkDeviceSize vertexBytes = static_cast<VkDeviceSize>(m_heightGridWidth) * m_heightGridHeight * sizeof(Vertex);
    VK_CHECK(vkMapMemory(m_device, m_vertexBuffer.memory, 0, vertexBytes, 0, &mapped));
    auto* vertices = reinterpret_cast<Vertex*>(mapped);

    uint32_t changed = 0;
    for (uint32_t gy = minY; gy <= maxY; ++gy)
    {
        for (uint32_t gx = minX; gx <= maxX; ++gx)
        {
            const float dx = (static_cast<float>(gx) - centerGridX) * m_cellScaleMeters;
            const float dy = (static_cast<float>(gy) - centerGridY) * m_cellScaleMeters;
            const float dist = std::sqrt(dx * dx + dy * dy);
            if (dist > m_editorBrushRadiusMeters)
                continue;

            const float t = 1.0f - dist / std::max(m_editorBrushRadiusMeters, 0.001f);
            const float falloff = t * t;
            const size_t index = static_cast<size_t>(gy) * m_heightGridWidth + gx;
            const float newHeight = std::clamp(m_heightCmGrid[index] + deltaCenterCm * falloff,
                                               -32768.0f,
                                               32767.0f);
            m_heightCmGrid[index] = newHeight;
            vertices[index].position[1] = newHeight * 0.01f;
            ++changed;
        }
    }
    vkUnmapMemory(m_device, m_vertexBuffer.memory);

    if (changed > 0)
    {
        const uint32_t chunksX = m_chunkSizeCells > 0 ? (m_mapSizeX + m_chunkSizeCells - 1u) / m_chunkSizeCells : 0;
        const size_t dirtyIndex = static_cast<size_t>(chunkY) * chunksX + chunkX;
        if (dirtyIndex < m_dirtyChunkTexels.size())
            m_dirtyChunkTexels[dirtyIndex] += changed;
    }
}

bool TerrainRenderer::RaycastEditorBrush(const WorldCamera& camera,
                                         uint32_t viewportWidth,
                                         uint32_t viewportHeight)
{
    if (viewportWidth == 0 || viewportHeight == 0)
    {
        m_editorBrushVisible = false;
        return false;
    }

    const float aspect = static_cast<float>(viewportWidth) / static_cast<float>(viewportHeight);
    const float tanHalfFov = std::tan(45.0f * 3.1415926535f / 180.0f * 0.5f);
    const float ndcX = (static_cast<float>(m_editorCursorX) / static_cast<float>(viewportWidth)) * 2.0f - 1.0f;
    const float ndcY = 1.0f - (static_cast<float>(m_editorCursorY) / static_cast<float>(viewportHeight)) * 2.0f;

    const WorldVec3 forward = WorldNormalize(WorldSub(camera.target, camera.eye));
    const WorldVec3 right = WorldNormalize(WorldCross({0.0f, 1.0f, 0.0f}, forward));
    const WorldVec3 up = WorldCross(forward, right);
    WorldVec3 rayDir = WorldNormalize(WorldAdd(forward,
        WorldAdd(WorldScale(right, ndcX * aspect * tanHalfFov),
                 WorldScale(up, ndcY * tanHalfFov))));

    const MovementBounds bounds = GetMovementBounds();
    if (!bounds.valid)
    {
        m_editorBrushVisible = false;
        return false;
    }

    constexpr float kStepMeters = 0.5f;
    constexpr float kMaxDistanceMeters = 700.0f;
    float previousT = 0.0f;
    float previousDelta = camera.eye.y - SampleHeight(camera.eye);
    for (float t = kStepMeters; t <= kMaxDistanceMeters; t += kStepMeters)
    {
        const WorldVec3 p = WorldAdd(camera.eye, WorldScale(rayDir, t));
        if (p.x < bounds.minX || p.x > bounds.maxX || p.z < bounds.minZ || p.z > bounds.maxZ)
        {
            previousT = t;
            previousDelta = 1.0f;
            continue;
        }

        const float terrainY = SampleHeight(p);
        const float delta = p.y - terrainY;
        if (delta <= 0.0f && previousDelta > 0.0f)
        {
            const float denom = previousDelta - delta;
            const float lerp = denom > 0.0001f ? previousDelta / denom : 0.0f;
            const float hitT = previousT + (t - previousT) * std::clamp(lerp, 0.0f, 1.0f);
            const WorldVec3 hit = WorldAdd(camera.eye, WorldScale(rayDir, hitT));
            m_editorBrushLocalX = std::clamp(hit.x, bounds.minX, bounds.maxX);
            m_editorBrushLocalZ = std::clamp(hit.z, bounds.minZ, bounds.maxZ);
            m_editorBrushVisible = true;
            return true;
        }

        previousT = t;
        previousDelta = delta;
    }

    m_editorBrushVisible = false;
    return false;
}

void TerrainRenderer::BeginEditorStroke()
{
    if (m_editorStrokeActive)
        return;

    m_editorStrokeActive = true;
    m_editorHasFlattenTarget = false;
    m_currentUndo = {};
    std::fill(m_heightUndoRecorded.begin(), m_heightUndoRecorded.end(), 0);
    std::fill(m_splatUndoRecorded.begin(), m_splatUndoRecorded.end(), 0);
}

void TerrainRenderer::EndEditorStroke()
{
    if (!m_editorStrokeActive)
        return;

    m_editorStrokeActive = false;
    m_editorHasFlattenTarget = false;
    if (m_currentUndo.heights.empty() && m_currentUndo.splats.empty())
        return;

    m_undoStack.push_back(std::move(m_currentUndo));
    if (m_undoStack.size() > 32)
        m_undoStack.pop_front();
    m_currentUndo = {};
}

void TerrainRenderer::RecordHeightUndo(size_t index)
{
    if (index >= m_heightCmGrid.size())
        return;
    if (index < m_heightUndoRecorded.size() && m_heightUndoRecorded[index])
        return;
    if (index < m_heightUndoRecorded.size())
        m_heightUndoRecorded[index] = 1;
    m_currentUndo.heights.push_back({index, m_heightCmGrid[index]});
}

void TerrainRenderer::RecordSplatUndo(size_t index)
{
    if (index >= static_cast<size_t>(m_splatWidth) * m_splatHeight)
        return;
    if (index < m_splatUndoRecorded.size() && m_splatUndoRecorded[index])
        return;
    if (index < m_splatUndoRecorded.size())
        m_splatUndoRecorded[index] = 1;

    SplatUndo undo{};
    undo.index = index;
    const size_t byte = index * 4u;
    for (size_t i = 0; i < 4; ++i)
        undo.oldWeights[i] = m_splatABytes[byte + i];
    for (size_t i = 0; i < 4; ++i)
        undo.oldWeights[4 + i] = m_splatBBytes[byte + i];
    m_currentUndo.splats.push_back(undo);
}

void TerrainRenderer::MarkHeightDirty(size_t heightIndex)
{
    if (m_chunkSizeCells == 0 || m_dirtyChunkTexels.empty() || m_heightGridWidth == 0)
        return;
    const uint32_t gx = static_cast<uint32_t>(heightIndex % m_heightGridWidth);
    const uint32_t gy = static_cast<uint32_t>(heightIndex / m_heightGridWidth);
    const uint32_t chunksX = (m_mapSizeX + m_chunkSizeCells - 1u) / m_chunkSizeCells;
    const uint32_t chunkX = std::min(gx / m_chunkSizeCells, chunksX - 1u);
    const uint32_t chunkY = std::min(gy / m_chunkSizeCells, ((m_mapSizeY + m_chunkSizeCells - 1u) / m_chunkSizeCells) - 1u);
    const size_t dirtyIndex = static_cast<size_t>(chunkY) * chunksX + chunkX;
    if (dirtyIndex < m_dirtyChunkTexels.size())
        ++m_dirtyChunkTexels[dirtyIndex];
}

void TerrainRenderer::MarkSplatDirty(size_t splatIndex)
{
    if (m_chunkSplatWidth == 0 || m_chunkSplatHeight == 0 || m_dirtyChunkTexels.empty() || m_splatWidth == 0)
        return;
    const uint32_t sx = static_cast<uint32_t>(splatIndex % m_splatWidth);
    const uint32_t sy = static_cast<uint32_t>(splatIndex / m_splatWidth);
    const uint32_t chunksX = m_chunkSplatWidth > 0 ? m_splatWidth / m_chunkSplatWidth : 0;
    if (chunksX == 0)
        return;
    const uint32_t chunkX = std::min(sx / m_chunkSplatWidth, chunksX - 1u);
    const uint32_t chunkY = std::min(sy / m_chunkSplatHeight,
        static_cast<uint32_t>(m_dirtyChunkTexels.size() / chunksX) - 1u);
    const size_t dirtyIndex = static_cast<size_t>(chunkY) * chunksX + chunkX;
    if (dirtyIndex < m_dirtyChunkTexels.size())
        ++m_dirtyChunkTexels[dirtyIndex];
}

void TerrainRenderer::ApplyEditorBrush(VulkanDevice& device, double deltaSeconds)
{
    (void)device;
    if (!m_editorBrushVisible || m_heightCmGrid.empty() || !m_vertexBuffer.memory ||
        m_heightGridWidth < 2 || m_heightGridHeight < 2 || m_chunkSizeCells == 0)
        return;

    BeginEditorStroke();

    const float centerXcm = m_spawnLocalXcm + m_editorBrushLocalX * 100.0f;
    const float centerYcm = m_spawnLocalYcm - m_editorBrushLocalZ * 100.0f;
    const float centerGridX = centerXcm / (m_cellScaleMeters * 100.0f);
    const float centerGridY = centerYcm / (m_cellScaleMeters * 100.0f);
    if (centerGridX < 0.0f || centerGridY < 0.0f ||
        centerGridX > static_cast<float>(m_heightGridWidth - 1u) ||
        centerGridY > static_cast<float>(m_heightGridHeight - 1u))
        return;

    const uint32_t chunksX = (m_mapSizeX + m_chunkSizeCells - 1u) / m_chunkSizeCells;
    const uint32_t chunksY = (m_mapSizeY + m_chunkSizeCells - 1u) / m_chunkSizeCells;
    const uint32_t chunkX = std::min(static_cast<uint32_t>(centerGridX) / m_chunkSizeCells, chunksX - 1u);
    const uint32_t chunkY = std::min(static_cast<uint32_t>(centerGridY) / m_chunkSizeCells, chunksY - 1u);
    const uint32_t chunkMinX = chunkX * m_chunkSizeCells;
    const uint32_t chunkMinY = chunkY * m_chunkSizeCells;
    const uint32_t chunkMaxX = std::min(chunkMinX + m_chunkSizeCells, m_heightGridWidth - 1u);
    const uint32_t chunkMaxY = std::min(chunkMinY + m_chunkSizeCells, m_heightGridHeight - 1u);
    const float radiusCells = m_editorBrushRadiusMeters / std::max(m_cellScaleMeters, 0.001f);
    const float alphaCenter = std::clamp(m_editorBrushStrength * static_cast<float>(deltaSeconds), 0.0f, 1.0f);

    if (m_editorTool == MapEditorTool::Paint)
    {
        if (m_splatABytes.empty() || m_splatBBytes.empty() || m_splatWidth == 0 || m_splatHeight == 0)
            return;
        const uint32_t splatChunkMinX = chunkX * m_chunkSplatWidth;
        const uint32_t splatChunkMinY = chunkY * m_chunkSplatHeight;
        const uint32_t splatChunkMaxX = std::min(splatChunkMinX + m_chunkSplatWidth - 1u, m_splatWidth - 1u);
        const uint32_t splatChunkMaxY = std::min(splatChunkMinY + m_chunkSplatHeight - 1u, m_splatHeight - 1u);
        const uint32_t minX = std::max(splatChunkMinX, static_cast<uint32_t>(std::max(0.0f, std::floor(centerGridX - radiusCells))));
        const uint32_t minY = std::max(splatChunkMinY, static_cast<uint32_t>(std::max(0.0f, std::floor(centerGridY - radiusCells))));
        const uint32_t maxX = std::min(splatChunkMaxX, static_cast<uint32_t>(std::ceil(centerGridX + radiusCells)));
        const uint32_t maxY = std::min(splatChunkMaxY, static_cast<uint32_t>(std::ceil(centerGridY + radiusCells)));
        bool changed = false;
        for (uint32_t sy = minY; sy <= maxY; ++sy)
        {
            for (uint32_t sx = minX; sx <= maxX; ++sx)
            {
                const float dx = (static_cast<float>(sx) - centerGridX) * m_cellScaleMeters;
                const float dy = (static_cast<float>(sy) - centerGridY) * m_cellScaleMeters;
                const float dist = std::sqrt(dx * dx + dy * dy);
                if (dist > m_editorBrushRadiusMeters)
                    continue;

                const float t = 1.0f - dist / std::max(m_editorBrushRadiusMeters, 0.001f);
                const float alpha = std::clamp(alphaCenter * t * t, 0.0f, 1.0f);
                if (alpha <= 0.0001f)
                    continue;

                const size_t index = static_cast<size_t>(sy) * m_splatWidth + sx;
                const size_t byte = index * 4u;
                RecordSplatUndo(index);

                float weights[8];
                for (int i = 0; i < 4; ++i)
                    weights[i] = static_cast<float>(m_splatABytes[byte + i]) / 255.0f;
                for (int i = 0; i < 4; ++i)
                    weights[4 + i] = static_cast<float>(m_splatBBytes[byte + i]) / 255.0f;

                const uint32_t slot = std::min<std::uint32_t>(m_editorTextureSlot, 7u);
                const float oldTarget = weights[slot];
                const float newTarget = oldTarget + (1.0f - oldTarget) * alpha;
                float otherSum = 0.0f;
                for (uint32_t i = 0; i < 8; ++i)
                    if (i != slot)
                        otherSum += weights[i];
                const float otherScale = otherSum > 0.0001f ? (1.0f - newTarget) / otherSum : 0.0f;
                for (uint32_t i = 0; i < 8; ++i)
                    weights[i] = (i == slot) ? newTarget : weights[i] * otherScale;

                for (int i = 0; i < 4; ++i)
                    m_splatABytes[byte + i] = static_cast<uint8_t>(std::clamp(std::lround(weights[i] * 255.0f), 0l, 255l));
                for (int i = 0; i < 4; ++i)
                    m_splatBBytes[byte + i] = static_cast<uint8_t>(std::clamp(std::lround(weights[4 + i] * 255.0f), 0l, 255l));
                MarkSplatDirty(index);
                changed = true;
            }
        }
        m_editorSplatGpuDirty = m_editorSplatGpuDirty || changed;
        return;
    }

    if (m_editorTool == MapEditorTool::Flatten && !m_editorHasFlattenTarget)
    {
        m_editorFlattenTargetCm = BilinearHeightCm(m_heightCmGrid, m_heightGridWidth, m_heightGridHeight,
            centerXcm, centerYcm, m_cellScaleMeters * 100.0f);
        m_editorHasFlattenTarget = true;
    }

    std::vector<float> smoothSource;
    if (m_editorTool == MapEditorTool::Smooth)
        smoothSource = m_heightCmGrid;

    const uint32_t minX = std::max(chunkMinX, static_cast<uint32_t>(std::max(0.0f, std::floor(centerGridX - radiusCells))));
    const uint32_t minY = std::max(chunkMinY, static_cast<uint32_t>(std::max(0.0f, std::floor(centerGridY - radiusCells))));
    const uint32_t maxX = std::min(chunkMaxX, static_cast<uint32_t>(std::ceil(centerGridX + radiusCells)));
    const uint32_t maxY = std::min(chunkMaxY, static_cast<uint32_t>(std::ceil(centerGridY + radiusCells)));

    void* mapped = nullptr;
    const VkDeviceSize vertexBytes = static_cast<VkDeviceSize>(m_heightGridWidth) * m_heightGridHeight * sizeof(Vertex);
    VK_CHECK(vkMapMemory(m_device, m_vertexBuffer.memory, 0, vertexBytes, 0, &mapped));
    auto* vertices = reinterpret_cast<Vertex*>(mapped);

    for (uint32_t gy = minY; gy <= maxY; ++gy)
    {
        for (uint32_t gx = minX; gx <= maxX; ++gx)
        {
            const float dx = (static_cast<float>(gx) - centerGridX) * m_cellScaleMeters;
            const float dy = (static_cast<float>(gy) - centerGridY) * m_cellScaleMeters;
            const float dist = std::sqrt(dx * dx + dy * dy);
            if (dist > m_editorBrushRadiusMeters)
                continue;
            const float t = 1.0f - dist / std::max(m_editorBrushRadiusMeters, 0.001f);
            const float falloff = t * t;
            const size_t index = static_cast<size_t>(gy) * m_heightGridWidth + gx;

            float newHeight = m_heightCmGrid[index];
            if (m_editorTool == MapEditorTool::Raise || m_editorTool == MapEditorTool::Lower)
            {
                const float sign = m_editorTool == MapEditorTool::Raise ? 1.0f : -1.0f;
                newHeight += sign * m_editorBrushStrength * static_cast<float>(deltaSeconds) * 100.0f * falloff;
            }
            else if (m_editorTool == MapEditorTool::Smooth)
            {
                float sum = 0.0f;
                float count = 0.0f;
                for (int oy = -1; oy <= 1; ++oy)
                {
                    for (int ox = -1; ox <= 1; ++ox)
                    {
                        const int nx = static_cast<int>(gx) + ox;
                        const int ny = static_cast<int>(gy) + oy;
                        if (nx < 0 || ny < 0 ||
                            nx >= static_cast<int>(m_heightGridWidth) ||
                            ny >= static_cast<int>(m_heightGridHeight))
                            continue;
                        sum += smoothSource[static_cast<size_t>(ny) * m_heightGridWidth + static_cast<size_t>(nx)];
                        count += 1.0f;
                    }
                }
                const float avg = count > 0.0f ? sum / count : m_heightCmGrid[index];
                const float alpha = std::clamp(alphaCenter * falloff, 0.0f, 1.0f);
                newHeight = m_heightCmGrid[index] + (avg - m_heightCmGrid[index]) * alpha;
            }
            else if (m_editorTool == MapEditorTool::Flatten)
            {
                const float alpha = std::clamp(alphaCenter * falloff, 0.0f, 1.0f);
                newHeight = m_heightCmGrid[index] + (m_editorFlattenTargetCm - m_heightCmGrid[index]) * alpha;
            }

            newHeight = std::clamp(newHeight, -32768.0f, 32767.0f);
            if (std::abs(newHeight - m_heightCmGrid[index]) <= 0.001f)
                continue;
            RecordHeightUndo(index);
            m_heightCmGrid[index] = newHeight;
            vertices[index].position[1] = newHeight * 0.01f;
            MarkHeightDirty(index);
        }
    }
    vkUnmapMemory(m_device, m_vertexBuffer.memory);
}

bool TerrainRenderer::RefreshSplatTextures(VulkanDevice& device)
{
    if (m_splatABytes.empty() || m_splatBBytes.empty())
        return false;
    const bool okA = UpdateRgbaTexture2D(device, m_splatA, m_splatABytes);
    const bool okB = UpdateRgbaTexture2D(device, m_splatB, m_splatBBytes);
    m_editorSplatGpuDirty = !(okA && okB);
    return okA && okB;
}

void TerrainRenderer::UndoLastEditorStroke(VulkanDevice& device)
{
    EndEditorStroke();
    if (m_undoStack.empty())
    {
        Tracen("[TERRAIN-EDITOR] undo requested but stack is empty");
        return;
    }

    EditorUndoEntry entry = std::move(m_undoStack.back());
    m_undoStack.pop_back();

    if (!entry.heights.empty() && m_vertexBuffer.memory)
    {
        void* mapped = nullptr;
        const VkDeviceSize vertexBytes = static_cast<VkDeviceSize>(m_heightGridWidth) * m_heightGridHeight * sizeof(Vertex);
        VK_CHECK(vkMapMemory(m_device, m_vertexBuffer.memory, 0, vertexBytes, 0, &mapped));
        auto* vertices = reinterpret_cast<Vertex*>(mapped);
        for (const HeightUndo& undo : entry.heights)
        {
            if (undo.index >= m_heightCmGrid.size())
                continue;
            m_heightCmGrid[undo.index] = undo.oldCm;
            vertices[undo.index].position[1] = undo.oldCm * 0.01f;
            MarkHeightDirty(undo.index);
        }
        vkUnmapMemory(m_device, m_vertexBuffer.memory);
    }

    if (!entry.splats.empty())
    {
        for (const SplatUndo& undo : entry.splats)
        {
            if (undo.index >= static_cast<size_t>(m_splatWidth) * m_splatHeight)
                continue;
            const size_t byte = undo.index * 4u;
            for (size_t i = 0; i < 4; ++i)
                m_splatABytes[byte + i] = undo.oldWeights[i];
            for (size_t i = 0; i < 4; ++i)
                m_splatBBytes[byte + i] = undo.oldWeights[4 + i];
            MarkSplatDirty(undo.index);
        }
        m_editorSplatGpuDirty = true;
        RefreshSplatTextures(device);
    }
    Tracenf("[TERRAIN-EDITOR] undo applied heights=%zu splats=%zu",
        entry.heights.size(),
        entry.splats.size());
}

std::string TerrainRenderer::ResolveWritableMapPath(const std::string& relativePath) const
{
    std::filesystem::path path(relativePath);
    if (path.is_absolute())
        return path.string();
    std::filesystem::path base = std::filesystem::current_path();
    for (;;)
    {
        const auto candidate = base / path;
        if (std::filesystem::exists(candidate.parent_path()))
            return candidate.string();
        if (!base.has_parent_path() || base == base.parent_path())
            break;
        base = base.parent_path();
    }
    return (std::filesystem::current_path() / path).string();
}

bool TerrainRenderer::SaveDirtyChunks()
{
    if (!m_mapLoaded || m_dirtyChunkTexels.empty() || m_chunkSizeCells == 0)
        return false;

    const uint32_t chunksX = (m_mapSizeX + m_chunkSizeCells - 1u) / m_chunkSizeCells;
    bool savedAny = false;
    bool attemptedAny = false;
    for (size_t i = 0; i < m_dirtyChunkTexels.size(); ++i)
    {
        const uint32_t dirty = m_dirtyChunkTexels[i];
        if (dirty == 0)
            continue;
        attemptedAny = true;
        const uint32_t chunkX = static_cast<uint32_t>(i % chunksX);
        const uint32_t chunkY = static_cast<uint32_t>(i / chunksX);
        if (SaveChunkHeights(chunkX, chunkY, dirty))
        {
            m_dirtyChunkTexels[i] = 0;
            savedAny = true;
        }
    }
    if (!attemptedAny)
        Tracen("[TERRAIN-EDITOR] save requested but no dirty chunks");
    else if (!savedAny)
        Tracen("[TERRAIN-EDITOR] save failed; dirty chunks remain pending");
    return savedAny;
}

bool TerrainRenderer::SaveChunkHeights(uint32_t chunkX, uint32_t chunkY, uint32_t dirtyTexels)
{
    const std::string relPath = m_loadedMapDirectory + "/chunks/chunk_" +
        std::to_string(chunkX) + "_" + std::to_string(chunkY) + ".mxchunk";
    const std::filesystem::path path = ResolveWritableMapPath(relPath);

    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
    {
        Tracenf("[TERRAIN-EDITOR] save failed; cannot open %s", path.string().c_str());
        return false;
    }
    const auto size = in.tellg();
    if (size <= 0)
        return false;
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!in || bytes.size() < 14)
        return false;
    in.close();

    if (ReadU32LE(bytes.data()) != 0x3143584d || ReadU16LE(bytes.data() + 4) != 2)
    {
        Tracenf("[TERRAIN-EDITOR] save rejected non-MAP4a chunk: %s", path.string().c_str());
        return false;
    }
    const uint16_t sectionCount = ReadU16LE(bytes.data() + 12);
    const size_t tocBegin = 14;
    const size_t tocEntrySize = 12;
    if (bytes.size() < tocBegin + static_cast<size_t>(sectionCount) * tocEntrySize)
        return false;

    uint32_t heightOffset = 0;
    uint32_t heightLength = 0;
    uint32_t splatAOffset = 0;
    uint32_t splatALength = 0;
    uint32_t splatBOffset = 0;
    uint32_t splatBLength = 0;
    for (uint16_t i = 0; i < sectionCount; ++i)
    {
        const uint8_t* entry = bytes.data() + tocBegin + static_cast<size_t>(i) * tocEntrySize;
        const uint16_t type = ReadU16LE(entry);
        if (type == 1)
        {
            heightOffset = ReadU32LE(entry + 4);
            heightLength = ReadU32LE(entry + 8);
        }
        else if (type == 2)
        {
            splatAOffset = ReadU32LE(entry + 4);
            splatALength = ReadU32LE(entry + 8);
        }
        else if (type == 4)
        {
            splatBOffset = ReadU32LE(entry + 4);
            splatBLength = ReadU32LE(entry + 8);
        }
    }
    const uint32_t chunkVertices = m_chunkSizeCells + 1u;
    const size_t expectedHeightBytes = static_cast<size_t>(chunkVertices) * chunkVertices * sizeof(int16_t);
    if (heightOffset == 0 || heightOffset + heightLength > bytes.size() || heightLength != expectedHeightBytes)
        return false;

    for (uint32_t y = 0; y < chunkVertices; ++y)
    {
        for (uint32_t x = 0; x < chunkVertices; ++x)
        {
            const uint32_t gx = chunkX * m_chunkSizeCells + x;
            const uint32_t gy = chunkY * m_chunkSizeCells + y;
            const size_t src = static_cast<size_t>(gy) * m_heightGridWidth + gx;
            const size_t dst = heightOffset + (static_cast<size_t>(y) * chunkVertices + x) * sizeof(int16_t);
            const int16_t h = static_cast<int16_t>(std::lround(std::clamp(m_heightCmGrid[src], -32768.0f, 32767.0f)));
            WriteI16LE(bytes.data() + dst, h);
        }
    }

    const size_t expectedSplatBytes =
        4u + static_cast<size_t>(m_chunkSplatWidth) * m_chunkSplatHeight * 4u;
    if (m_chunkSplatWidth > 0 && m_chunkSplatHeight > 0 &&
        splatAOffset != 0 && splatBOffset != 0 &&
        splatAOffset + splatALength <= bytes.size() &&
        splatBOffset + splatBLength <= bytes.size() &&
        splatALength == expectedSplatBytes &&
        splatBLength == expectedSplatBytes)
    {
        WriteU16LE(bytes.data() + splatAOffset, static_cast<uint16_t>(m_chunkSplatWidth));
        WriteU16LE(bytes.data() + splatAOffset + 2, static_cast<uint16_t>(m_chunkSplatHeight));
        WriteU16LE(bytes.data() + splatBOffset, static_cast<uint16_t>(m_chunkSplatWidth));
        WriteU16LE(bytes.data() + splatBOffset + 2, static_cast<uint16_t>(m_chunkSplatHeight));
        for (uint32_t y = 0; y < m_chunkSplatHeight; ++y)
        {
            for (uint32_t x = 0; x < m_chunkSplatWidth; ++x)
            {
                const uint32_t sx = chunkX * m_chunkSplatWidth + x;
                const uint32_t sy = chunkY * m_chunkSplatHeight + y;
                const size_t src = (static_cast<size_t>(sy) * m_splatWidth + sx) * 4u;
                const size_t dst = (static_cast<size_t>(y) * m_chunkSplatWidth + x) * 4u;
                if (src + 4u <= m_splatABytes.size())
                    std::memcpy(bytes.data() + splatAOffset + 4u + dst, m_splatABytes.data() + src, 4);
                if (src + 4u <= m_splatBBytes.size())
                    std::memcpy(bytes.data() + splatBOffset + 4u + dst, m_splatBBytes.data() + src, 4);
            }
        }
    }

    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out)
            return false;
    }

    if (!AtomicReplace(tmp, path))
        return false;

    const uint32_t chunkId = chunkY * ((m_mapSizeX + m_chunkSizeCells - 1u) / m_chunkSizeCells) + chunkX;
    Tracenf("[TERRAIN-EDITOR] saved chunk_id=%u path=%s changes=%u texels",
        chunkId,
        path.string().c_str(),
        dirtyTexels);
    return true;
}

bool TerrainRenderer::ReloadCurrentMap(VulkanDevice& device)
{
    if (m_loadedMapDirectory.empty())
        return false;
    Tracen("[TERRAIN-EDITOR] reload heightmap from disk");
    return LoadMap(device, m_loadedMapDirectory, m_loadedServerX, m_loadedServerY);
}

bool TerrainRenderer::CreateFallbackTexture(VulkanDevice& device)
{
    constexpr uint32_t kSize = 4;
    constexpr uint32_t kLayers = 8;
    const std::array<std::array<uint8_t, 4>, kLayers> colors = {{
        {{42, 73, 105, 255}},   // water/mud
        {{194, 171, 101, 255}}, // sand
        {{72, 126, 55, 255}},   // grass
        {{119, 96, 66, 255}},   // dirt
        {{111, 112, 108, 255}}, // rock
        {{82, 83, 86, 255}},    // steep rock
        {{55, 112, 72, 255}},   // moss
        {{222, 229, 232, 255}}, // snow
    }};
    std::vector<uint8_t> pixels;
    pixels.reserve(kSize * kSize * kLayers * 4u);
    for (uint32_t layer = 0; layer < kLayers; ++layer) {
        for (uint32_t y = 0; y < kSize; ++y) {
            for (uint32_t x = 0; x < kSize; ++x) {
                const float checker = ((x ^ y) & 1u) ? 0.86f : 1.08f;
                pixels.push_back(static_cast<uint8_t>(std::clamp(colors[layer][0] * checker, 0.0f, 255.0f)));
                pixels.push_back(static_cast<uint8_t>(std::clamp(colors[layer][1] * checker, 0.0f, 255.0f)));
                pixels.push_back(static_cast<uint8_t>(std::clamp(colors[layer][2] * checker, 0.0f, 255.0f)));
                pixels.push_back(colors[layer][3]);
            }
        }
    }
    return UploadRgbaTextureArray(device, "terrain_palette_fallback", kSize, kSize, kLayers, pixels, m_baseTexture);
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

bool TerrainRenderer::CreateFallbackSplatTextures(VulkanDevice& device)
{
    std::vector<uint8_t> splatA(4, 0);
    std::vector<uint8_t> splatB(4, 0);
    splatA[2] = 255; // fallback grass.
    return UploadRgbaTexture2D(device, "splat_a_fallback", 1, 1, splatA,
               VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, m_splatA) &&
           UploadRgbaTexture2D(device, "splat_b_fallback", 1, 1, splatB,
               VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, m_splatB);
}

bool TerrainRenderer::LoadTerrainPalette(VulkanDevice& device,
    const mx::map::Manifest& manifest,
    const std::string& mapDirectory)
{
    if (!m_assets || manifest.texture_palette_paths.size() < 8)
        return false;

    std::array<DdsImage, 8> images{};
    for (uint32_t i = 0; i < images.size(); ++i)
    {
        std::string path = manifest.texture_palette_paths[i];
        if (!path.empty() && path.rfind("assets/", 0) != 0 && path.find(':') == std::string::npos)
            path = mapDirectory + "/" + path;
        if (!LoadDdsImage(*m_assets, path, images[i]))
            return false;
        if (i > 0 &&
            (images[i].width != images[0].width ||
             images[i].height != images[0].height ||
             images[i].mipLevels != images[0].mipLevels ||
             images[i].format != images[0].format ||
             images[i].compressed != images[0].compressed))
        {
            Tracenf("[TERRAIN-PALETTE] incompatible texture array layer %u: %s", i, path.c_str());
            return false;
        }
    }

    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(device.GetPhysicalDevice(), images[0].format, &props);
    const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((props.optimalTilingFeatures & required) != required)
        return false;

    Texture palette{};
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    vkGetDeviceQueue(m_device, device.GetGraphicsQueueFamily(), 0, &graphicsQueue);
    CreateDeviceLocalImageArray(device, m_device, images[0].width, images[0].height, images[0].mipLevels,
        static_cast<uint32_t>(images.size()), images[0].format, palette.image, palette.memory);

    std::vector<uint8_t> pixels;
    std::vector<VkBufferImageCopy> regions;
    for (uint32_t layer = 0; layer < images.size(); ++layer)
    {
        const VkDeviceSize baseOffset = static_cast<VkDeviceSize>(pixels.size());
        pixels.insert(pixels.end(), images[layer].pixels.begin(), images[layer].pixels.end());
        for (VkBufferImageCopy region : images[layer].regions)
        {
            region.bufferOffset += baseOffset;
            region.imageSubresource.baseArrayLayer = layer;
            region.imageSubresource.layerCount = 1;
            regions.push_back(region);
        }
    }

    Buffer staging{};
    CreateHostVisibleBuffer(device, m_device, pixels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, pixels.data(), staging);

    VkCommandPool uploadPool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = BeginOneTimeCommands(m_device, device.GetGraphicsQueueFamily(), uploadPool);
    TransitionImageLayoutArray(cmd, palette.image, images[0].mipLevels, static_cast<uint32_t>(images.size()),
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(cmd, staging.buffer, palette.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        static_cast<uint32_t>(regions.size()), regions.data());
    TransitionImageLayoutArray(cmd, palette.image, images[0].mipLevels, static_cast<uint32_t>(images.size()),
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EndOneTimeCommands(m_device, graphicsQueue, uploadPool, cmd);
    DestroyBuffer(staging);

    palette.format = images[0].format;
    palette.width = images[0].width;
    palette.height = images[0].height;
    palette.mipLevels = images[0].mipLevels;
    palette.name = "terrain_palette";

    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = palette.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    view.format = palette.format;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = palette.mipLevels;
    view.subresourceRange.layerCount = static_cast<uint32_t>(images.size());
    VK_CHECK(vkCreateImageView(m_device, &view, nullptr, &palette.view));

    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = palette.mipLevels > 1 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.maxLod = static_cast<float>(palette.mipLevels);
    VK_CHECK(vkCreateSampler(m_device, &sampler, nullptr, &palette.sampler));

    DestroyTexture(m_baseTexture);
    m_baseTexture = palette;
    Tracenf("[TERRAIN-PALETTE] loaded 8-layer palette size=%ux%u mips=%u format=%s",
        m_baseTexture.width,
        m_baseTexture.height,
        m_baseTexture.mipLevels,
        VkFormatName(m_baseTexture.format));
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
    for (const Buffer& buffer : m_uniformBuffers)
    {
        if (!buffer.buffer || !buffer.memory)
        {
            Tracen("[TERRAIN] CreateDescriptors skipped: uniform buffer is not ready");
            return false;
        }
    }

    if (!m_descriptorSetLayout)
    {
        VkDescriptorSetLayoutBinding ubo{};
        ubo.binding = 0;
        ubo.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ubo.descriptorCount = 1;
        ubo.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

        VkDescriptorSetLayoutBinding palette{};
        palette.binding = 1;
        palette.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        palette.descriptorCount = 1;
        palette.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutBinding splatA{};
        splatA.binding = 2;
        splatA.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        splatA.descriptorCount = 1;
        splatA.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutBinding splatB{};
        splatB.binding = 3;
        splatB.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        splatB.descriptorCount = 1;
        splatB.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo layout{};
        layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        std::array<VkDescriptorSetLayoutBinding, 4> bindings = {ubo, palette, splatA, splatB};
        layout.bindingCount = static_cast<uint32_t>(bindings.size());
        layout.pBindings = bindings.data();
        VK_CHECK(vkCreateDescriptorSetLayout(m_device, &layout, nullptr, &m_descriptorSetLayout));
    }

    if (m_descriptorPool)
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
    m_descriptorPool = VK_NULL_HANDLE;
    m_descriptorSets.fill(VK_NULL_HANDLE);
    m_layerDescriptorSets.clear();

    const uint32_t descriptorSetCount = kFramesInFlight;

    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = descriptorSetCount;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = descriptorSetCount * 3u;

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
    VK_CHECK(vkAllocateDescriptorSets(m_device, &alloc, m_descriptorSets.data()));

    UpdateDescriptors();

    return true;
}

void TerrainRenderer::UpdateDescriptors()
{
    if (!m_descriptorPool)
        return;

    auto writeSet = [this](VkDescriptorSet descriptorSet, uint32_t frame)
    {
        if (!descriptorSet || !m_baseTexture.view || !m_baseTexture.sampler ||
            !m_splatA.view || !m_splatA.sampler || !m_splatB.view || !m_splatB.sampler)
            return;
        if (!m_uniformBuffers[frame].buffer || !m_uniformBuffers[frame].memory)
        {
            Tracen("[TERRAIN] descriptor update skipped: uniform buffer is not ready");
            return;
        }

        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = m_uniformBuffers[frame].buffer;
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(UniformBlock);

        VkDescriptorImageInfo paletteInfo{};
        paletteInfo.sampler = m_baseTexture.sampler;
        paletteInfo.imageView = m_baseTexture.view;
        paletteInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo splatAInfo{};
        splatAInfo.sampler = m_splatA.sampler;
        splatAInfo.imageView = m_splatA.view;
        splatAInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo splatBInfo{};
        splatBInfo.sampler = m_splatB.sampler;
        splatBInfo.imageView = m_splatB.view;
        splatBInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        std::array<VkWriteDescriptorSet, 4> writes{};
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
        writes[1].pImageInfo = &paletteInfo;

        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = descriptorSet;
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[2].pImageInfo = &splatAInfo;

        writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[3].dstSet = descriptorSet;
        writes[3].dstBinding = 3;
        writes[3].descriptorCount = 1;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[3].pImageInfo = &splatBInfo;

        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    };

    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
        writeSet(m_descriptorSets[frame], frame);
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
    if (frameIndex >= kFramesInFlight || !m_uniformBuffers[frameIndex].memory)
    {
        Tracen("[TERRAIN] UpdateUniform skipped: uniform buffer is not ready");
        return;
    }

    const UniformBlock uniform{camera.viewProjection};
    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(m_device, m_uniformBuffers[frameIndex].memory, 0, sizeof(uniform), 0, &mapped));
    std::memcpy(mapped, &uniform, sizeof(uniform));
    vkUnmapMemory(m_device, m_uniformBuffers[frameIndex].memory);
}
