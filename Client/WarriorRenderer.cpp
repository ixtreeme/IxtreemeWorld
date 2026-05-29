#include "WarriorRenderer.h"

#include <windows.h>
#include <granny.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace
{
void Log(const char* text)
{
    OutputDebugStringA(text);
    OutputDebugStringA("\n");
    std::fprintf(stderr, "%s\n", text);
}

void LogFormat(const char* format, ...)
{
    char buffer[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    Log(buffer);
}

unsigned long long HandleValue(VkPipeline handle)
{
    return static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(handle));
}

const char* VkResultName(VkResult result)
{
    switch (result)
    {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    default: return "UNKNOWN_VK_RESULT";
    }
}

void CheckVk(VkResult result, const char* call, const char* file, int line)
{
    if (result == VK_SUCCESS)
        return;

    char buffer[512];
    std::snprintf(buffer, sizeof(buffer), "%s:%d: Vulkan call failed: %s -> %s (%d)",
        file, line, call, VkResultName(result), result);
    Log(buffer);
    std::abort();
}

#define VK_CHECK(call) CheckVk((call), #call, __FILE__, __LINE__)

struct Mat4
{
    float m[16];
};

struct UniformBlock
{
    Mat4 mvp;
    Mat4 model;
};

static_assert(sizeof(WarriorRenderer::Vertex) == 32, "Graphics vertex layout must stay 32 bytes");

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

struct DdsHeaderDxt10
{
    uint32_t dxgiFormat;
    uint32_t resourceDimension;
    uint32_t miscFlag;
    uint32_t arraySize;
    uint32_t miscFlags2;
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
    bool srgb = false;
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::vector<uint8_t> pixels;
    std::vector<VkBufferImageCopy> regions;
};

granny_data_type_definition g_sourceVertexType[] =
{
    {GrannyReal32Member, "Position", nullptr, 3, {0, 0, 0}, 0},
    {GrannyNormalUInt8Member, "BoneWeights", nullptr, 4, {0, 0, 0}, 0},
    {GrannyUInt8Member, "BoneIndices", nullptr, 4, {0, 0, 0}, 0},
    {GrannyReal32Member, "Normal", nullptr, 3, {0, 0, 0}, 0},
    {GrannyReal32Member, "TextureCoordinates0", nullptr, 2, {0, 0, 0}, 0},
    {GrannyEndMember, nullptr, nullptr, 0, {0, 0, 0}, 0},
};

Mat4 Identity()
{
    Mat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

Mat4 Multiply(const Mat4& a, const Mat4& b)
{
    Mat4 r{};
    for (int row = 0; row < 4; ++row)
    {
        for (int col = 0; col < 4; ++col)
        {
            for (int k = 0; k < 4; ++k)
                r.m[row * 4 + col] += a.m[row * 4 + k] * b.m[k * 4 + col];
        }
    }
    return r;
}

Mat4 Scale(float value)
{
    Mat4 r = Identity();
    r.m[0] = value;
    r.m[5] = value;
    r.m[10] = value;
    return r;
}

Mat4 RawGrannyToDisplay()
{
    Mat4 r{};
    // Row-vector matrix: raw Granny centimeters/Z-up -> renderer meters/Y-up.
    // (x, y, z) -> (x * 0.01, z * 0.01, -y * 0.01)
    r.m[0] = 0.01f;
    r.m[6] = -0.01f;
    r.m[9] = 0.01f;
    r.m[15] = 1.0f;
    return r;
}

Mat4 RotationY(float angle)
{
    const float c = std::cos(angle);
    const float s = std::sin(angle);

    Mat4 r = Identity();
    r.m[0] = c;
    r.m[2] = s;
    r.m[8] = -s;
    r.m[10] = c;
    return r;
}

Mat4 Translation(float x, float y, float z)
{
    Mat4 r = Identity();
    r.m[12] = x;
    r.m[13] = y;
    r.m[14] = z;
    return r;
}

Mat4 Perspective(float fovYRadians, float aspect, float zNear, float zFar)
{
    const float f = 1.0f / std::tan(fovYRadians * 0.5f);
    Mat4 r{};
    r.m[0] = f / aspect;
    r.m[5] = -f;
    r.m[10] = zFar / (zFar - zNear);
    r.m[11] = 1.0f;
    r.m[14] = -(zNear * zFar) / (zFar - zNear);
    return r;
}

Mat4 ToLocalMat4(const WorldMat4& matrix)
{
    Mat4 r{};
    std::memcpy(r.m, matrix.m, sizeof(r.m));
    return r;
}

size_t MotionIndex(WarriorRenderer::MotionState state)
{
    return static_cast<size_t>(state);
}

const char* MotionStateName(WarriorRenderer::MotionState state)
{
    switch (state)
    {
    case WarriorRenderer::MotionState::Idle: return "wait";
    case WarriorRenderer::MotionState::Walk: return "walk";
    case WarriorRenderer::MotionState::Run: return "run";
    default: return "unknown";
    }
}

std::string ExecutableDirectory()
{
    char path[MAX_PATH]{};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string result(path);
    const size_t slash = result.find_last_of("\\/");
    return slash == std::string::npos ? std::string(".") : result.substr(0, slash);
}

std::vector<char> ReadBinaryFile(const std::string& path)
{
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file)
    {
        std::string message = "Failed to open shader: " + path;
        Log(message.c_str());
        std::abort();
    }

    const size_t size = static_cast<size_t>(file.tellg());
    std::vector<char> bytes(size);
    file.seekg(0);
    file.read(bytes.data(), bytes.size());
    return bytes;
}

uint32_t MakeFourCC(char a, char b, char c, char d)
{
    return static_cast<uint32_t>(a) |
        (static_cast<uint32_t>(b) << 8) |
        (static_cast<uint32_t>(c) << 16) |
        (static_cast<uint32_t>(d) << 24);
}

std::string FourCCString(uint32_t fourCC)
{
    char text[5]{};
    text[0] = static_cast<char>(fourCC & 0xff);
    text[1] = static_cast<char>((fourCC >> 8) & 0xff);
    text[2] = static_cast<char>((fourCC >> 16) & 0xff);
    text[3] = static_cast<char>((fourCC >> 24) & 0xff);
    for (int i = 0; i < 4; ++i)
    {
        if (text[i] < 32 || text[i] > 126)
            text[i] = '?';
    }
    return text;
}

const char* VkFormatName(VkFormat format)
{
    switch (format)
    {
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return "VK_FORMAT_BC1_RGBA_SRGB_BLOCK";
    case VK_FORMAT_BC2_SRGB_BLOCK: return "VK_FORMAT_BC2_SRGB_BLOCK";
    case VK_FORMAT_BC3_SRGB_BLOCK: return "VK_FORMAT_BC3_SRGB_BLOCK";
    case VK_FORMAT_BC7_SRGB_BLOCK: return "VK_FORMAT_BC7_SRGB_BLOCK";
    case VK_FORMAT_R8G8B8A8_SRGB: return "VK_FORMAT_R8G8B8A8_SRGB";
    case VK_FORMAT_B8G8R8A8_SRGB: return "VK_FORMAT_B8G8R8A8_SRGB";
    default: return "VK_FORMAT_UNDEFINED";
    }
}

bool DxgiFormatToVk(uint32_t dxgiFormat, VkFormat& format, uint32_t& blockBytes, bool& compressed)
{
    compressed = true;
    switch (dxgiFormat)
    {
    case 71: format = VK_FORMAT_BC1_RGBA_SRGB_BLOCK; blockBytes = 8; return true;
    case 74: format = VK_FORMAT_BC2_SRGB_BLOCK; blockBytes = 16; return true;
    case 77: format = VK_FORMAT_BC3_SRGB_BLOCK; blockBytes = 16; return true;
    case 99: format = VK_FORMAT_BC7_SRGB_BLOCK; blockBytes = 16; return true;
    default: return false;
    }
}

bool LoadDdsImage(const std::string& path, DdsImage& out)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
    {
        LogFormat("[DDS] failed to open %s", path.c_str());
        return false;
    }

    const size_t fileSize = static_cast<size_t>(file.tellg());
    file.seekg(0);
    std::vector<uint8_t> bytes(fileSize);
    file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());

    if (bytes.size() < sizeof(uint32_t) + sizeof(DdsHeader))
    {
        LogFormat("[DDS] invalid file too small: %s", path.c_str());
        return false;
    }

    const uint32_t magic = *reinterpret_cast<const uint32_t*>(bytes.data());
    if (magic != MakeFourCC('D', 'D', 'S', ' '))
    {
        LogFormat("[DDS] invalid magic: %s", path.c_str());
        return false;
    }

    const DdsHeader* header = reinterpret_cast<const DdsHeader*>(bytes.data() + sizeof(uint32_t));
    if (header->size != 124 || header->pixelFormat.size != 32)
    {
        LogFormat("[DDS] invalid header sizes: %s", path.c_str());
        return false;
    }

    size_t dataOffset = sizeof(uint32_t) + sizeof(DdsHeader);
    const uint32_t ddpfFourCC = 0x00000004;
    const uint32_t ddpfRGB = 0x00000040;
    const uint32_t ddpfAlphaPixels = 0x00000001;
    std::string fourCC = "----";

    out = {};
    out.filename = path.substr(path.find_last_of("\\/") + 1);
    out.width = header->width;
    out.height = header->height;
    out.mipLevels = std::max<uint32_t>(1, header->mipMapCount);
    out.srgb = true;

    if (header->pixelFormat.flags & ddpfFourCC)
    {
        fourCC = FourCCString(header->pixelFormat.fourCC);
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
        else if (header->pixelFormat.fourCC == MakeFourCC('D', 'X', '1', '0'))
        {
            if (bytes.size() < dataOffset + sizeof(DdsHeaderDxt10))
            {
                LogFormat("[DDS] missing DX10 header: %s", path.c_str());
                return false;
            }
            const DdsHeaderDxt10* dxt10 = reinterpret_cast<const DdsHeaderDxt10*>(bytes.data() + dataOffset);
            dataOffset += sizeof(DdsHeaderDxt10);
            if (!DxgiFormatToVk(dxt10->dxgiFormat, out.format, out.blockBytes, out.compressed))
            {
                LogFormat("[DDS] unsupported DXGI format %u in %s", dxt10->dxgiFormat, path.c_str());
                return false;
            }
        }
        else
        {
            LogFormat("[DDS] unsupported fourCC '%s' in %s", fourCC.c_str(), path.c_str());
            return false;
        }
    }
    else if ((header->pixelFormat.flags & ddpfRGB) && header->pixelFormat.rgbBitCount == 32)
    {
        out.compressed = false;
        out.bytesPerPixel = 4;
        const bool hasAlpha = (header->pixelFormat.flags & ddpfAlphaPixels) != 0;
        (void)hasAlpha;
        if (header->pixelFormat.rBitMask == 0x00ff0000 &&
            header->pixelFormat.gBitMask == 0x0000ff00 &&
            header->pixelFormat.bBitMask == 0x000000ff)
        {
            out.format = VK_FORMAT_B8G8R8A8_SRGB;
        }
        else
        {
            out.format = VK_FORMAT_R8G8B8A8_SRGB;
        }
    }
    else
    {
        LogFormat("[DDS] unsupported pixel format flags=0x%08x bpp=%u in %s",
            header->pixelFormat.flags, header->pixelFormat.rgbBitCount, path.c_str());
        return false;
    }

    size_t offset = dataOffset;
    out.regions.clear();
    for (uint32_t mip = 0; mip < out.mipLevels; ++mip)
    {
        const uint32_t mipWidth = std::max(1u, out.width >> mip);
        const uint32_t mipHeight = std::max(1u, out.height >> mip);
        size_t mipSize = 0;
        if (out.compressed)
        {
            const uint32_t blocksWide = std::max(1u, (mipWidth + 3u) / 4u);
            const uint32_t blocksHigh = std::max(1u, (mipHeight + 3u) / 4u);
            mipSize = static_cast<size_t>(blocksWide) * blocksHigh * out.blockBytes;
        }
        else
        {
            mipSize = static_cast<size_t>(mipWidth) * mipHeight * out.bytesPerPixel;
        }

        if (offset + mipSize > bytes.size())
        {
            LogFormat("[DDS] mip data truncated in %s at mip=%u", path.c_str(), mip);
            return false;
        }

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

    LogFormat("[DDS] %s: %ux%u mips=%u fourCC='%s' vkFormat=%s sRGB=yes",
        out.filename.c_str(),
        out.width,
        out.height,
        out.mipLevels,
        fourCC.c_str(),
        VkFormatName(out.format));
    return true;
}

VkShaderModule CreateShaderModule(VkDevice device, const std::string& path)
{
    const std::vector<char> code = ReadBinaryFile(path);
    VkShaderModuleCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    create.codeSize = code.size();
    create.pCode = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule module = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(device, &create, nullptr, &module));
    return module;
}

VkCommandBuffer BeginOneTimeCommands(VkDevice vkDevice, uint32_t queueFamily, VkCommandPool& pool);
void EndOneTimeCommands(VkDevice vkDevice, VkQueue queue, VkCommandPool pool, VkCommandBuffer cmd);

bool CreateHostVisibleBuffer(VulkanDevice& device, VkDevice vkDevice, VkDeviceSize size,
    VkBufferUsageFlags usage, const void* initialData, WarriorRenderer::Buffer& out)
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

bool CreateDeviceLocalBuffer(VulkanDevice& device, VkDevice vkDevice, VkQueue queue, VkDeviceSize size,
    VkBufferUsageFlags usage, const void* initialData, WarriorRenderer::Buffer& out)
{
    VkBufferCreateInfo buffer{};
    buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer.size = size;
    buffer.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(vkDevice, &buffer, nullptr, &out.buffer));

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(vkDevice, out.buffer, &req);

    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = device.FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(vkDevice, &alloc, nullptr, &out.memory));
    VK_CHECK(vkBindBufferMemory(vkDevice, out.buffer, out.memory, 0));

    if (initialData && size > 0)
    {
        WarriorRenderer::Buffer staging{};
        CreateHostVisibleBuffer(device, vkDevice, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, initialData, staging);

        VkCommandPool uploadPool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = BeginOneTimeCommands(vkDevice, device.GetGraphicsQueueFamily(), uploadPool);
        CopyBuffer(cmd, staging.buffer, out.buffer, size);
        EndOneTimeCommands(vkDevice, queue, uploadPool, cmd);
        if (staging.buffer)
            vkDestroyBuffer(vkDevice, staging.buffer, nullptr);
        if (staging.memory)
            vkFreeMemory(vkDevice, staging.memory, nullptr);
    }

    return true;
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
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    }

    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

VkRect2D PreviewRect(VkExtent2D extent)
{
    // TODO: derive this from the actual Lobby.xaml preview-frame bounds instead of mirroring the current fixed layout.
    const int32_t cardWidth = 940;
    const int32_t cardHeight = 492;
    const int32_t cardX = std::max<int32_t>(0, (static_cast<int32_t>(extent.width) - cardWidth) / 2);
    const int32_t cardY = std::max<int32_t>(0, (static_cast<int32_t>(extent.height) - cardHeight) / 2);

    VkRect2D rect{};
    rect.offset.x = cardX + 46;
    rect.offset.y = cardY + 96;
    rect.extent.width = 224;
    rect.extent.height = 324;

    if (rect.offset.x < 0) rect.offset.x = 0;
    if (rect.offset.y < 0) rect.offset.y = 0;
    if (rect.offset.x + static_cast<int32_t>(rect.extent.width) > static_cast<int32_t>(extent.width))
        rect.extent.width = extent.width - rect.offset.x;
    if (rect.offset.y + static_cast<int32_t>(rect.extent.height) > static_cast<int32_t>(extent.height))
        rect.extent.height = extent.height - rect.offset.y;
    return rect;
}

float Length3(const float v[3])
{
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

void Normalize3(float v[3])
{
    const float len = Length3(v);
    if (len <= 0.000001f)
        return;
    v[0] /= len;
    v[1] /= len;
    v[2] /= len;
}

void TransformPointRowVector(const granny_real32* matrix, const float in[3], float out[3])
{
    out[0] = in[0] * matrix[0] + in[1] * matrix[4] + in[2] * matrix[8] + matrix[12];
    out[1] = in[0] * matrix[1] + in[1] * matrix[5] + in[2] * matrix[9] + matrix[13];
    out[2] = in[0] * matrix[2] + in[1] * matrix[6] + in[2] * matrix[10] + matrix[14];
}

void TransformVectorRowVector(const granny_real32* matrix, const float in[3], float out[3])
{
    out[0] = in[0] * matrix[0] + in[1] * matrix[4] + in[2] * matrix[8];
    out[1] = in[0] * matrix[1] + in[1] * matrix[5] + in[2] * matrix[9];
    out[2] = in[0] * matrix[2] + in[1] * matrix[6] + in[2] * matrix[10];
}

void SkinVertex(const WarriorRenderer::SourceVertex& source, const granny_matrix_4x4* compositeMatrices,
    const granny_int32x* toBoneIndices, int bindingBoneCount, float outPosition[3],
    int skeletonBoneCount, float outNormal[3], float outUv[2], int modelBones[4])
{
    outPosition[0] = outPosition[1] = outPosition[2] = 0.0f;
    outNormal[0] = outNormal[1] = outNormal[2] = 0.0f;
    outUv[0] = outUv[1] = 0.0f;
    modelBones[0] = modelBones[1] = modelBones[2] = modelBones[3] = -1;

    int totalWeight = 0;
    for (int i = 0; i < 4; ++i)
        totalWeight += source.boneWeights[i];

    if (totalWeight <= 0 || !compositeMatrices || !toBoneIndices)
    {
        std::memcpy(outPosition, source.position, sizeof(float) * 3);
        std::memcpy(outNormal, source.normal, sizeof(float) * 3);
        std::memcpy(outUv, source.uv, sizeof(float) * 2);
        Normalize3(outNormal);
        return;
    }

    for (int influence = 0; influence < 4; ++influence)
    {
        const int weightByte = source.boneWeights[influence];
        if (weightByte == 0)
            continue;

        const int localBone = source.boneIndices[influence];
        if (localBone < 0 || localBone >= bindingBoneCount)
            continue;

        const int modelBone = toBoneIndices[localBone];
        modelBones[influence] = modelBone;
        if (modelBone < 0 || modelBone >= skeletonBoneCount)
            continue;

        const float weight = static_cast<float>(weightByte) / static_cast<float>(totalWeight);
        const granny_real32* matrix = reinterpret_cast<const granny_real32*>(compositeMatrices[modelBone]);

        float skinnedPosition[3]{};
        float skinnedNormal[3]{};
        TransformPointRowVector(matrix, source.position, skinnedPosition);
        TransformVectorRowVector(matrix, source.normal, skinnedNormal);

        for (int axis = 0; axis < 3; ++axis)
        {
            outPosition[axis] += skinnedPosition[axis] * weight;
            outNormal[axis] += skinnedNormal[axis] * weight;
        }
    }

    Normalize3(outNormal);
    outUv[0] = source.uv[0];
    outUv[1] = source.uv[1];
}

uint32_t PackBytes(uint32_t b0, uint32_t b1, uint32_t b2, uint32_t b3)
{
    return (b0 & 0xffu) |
        ((b1 & 0xffu) << 8u) |
        ((b2 & 0xffu) << 16u) |
        ((b3 & 0xffu) << 24u);
}

bool MeshUsesFaceTexture(granny_mesh* mesh)
{
    if (mesh && mesh->Name && std::strstr(mesh->Name, "face"))
        return true;

    if (!mesh)
        return false;

    for (int materialIndex = 0; materialIndex < mesh->MaterialBindingCount; ++materialIndex)
    {
        granny_material* material = mesh->MaterialBindings[materialIndex].Material;
        if (!material)
            continue;

        if (material->Name && std::strstr(material->Name, "face"))
            return true;

        for (int mapIndex = 0; mapIndex < material->MapCount; ++mapIndex)
        {
            granny_material* mapped = material->Maps[mapIndex].Material;
            if (mapped && mapped->Texture && mapped->Texture->FromFileName &&
                std::strstr(mapped->Texture->FromFileName, "face"))
            {
                return true;
            }
        }
    }
    return false;
}
}

bool WarriorRenderer::Create(VulkanDevice& device, const std::string& modelPath)
{
    Destroy();
    m_device = device.GetDevice();

    const bool loaded = LoadGrannyMesh(modelPath);
    const bool buffers = loaded ? CreateBuffers(device) : false;
    const bool compute = buffers ? CreateComputeResources(device) : false;
    const bool textures = compute ? CreateTextures(device, modelPath) : false;
    const bool descriptors = textures ? CreateDescriptors() : false;
    const bool pipeline = descriptors ? CreatePipeline(device) : false;

    LogFormat("[MESH] Create: loaded=%d buffers=%d compute=%d textures=%d descriptors=%d pipeline=%d",
        loaded ? 1 : 0,
        buffers ? 1 : 0,
        compute ? 1 : 0,
        textures ? 1 : 0,
        descriptors ? 1 : 0,
        pipeline ? 1 : 0);
    if (loaded && buffers && compute && textures && descriptors && pipeline)
    {
        LogFormat("[MESH] Create OK, pipeline=0x%llx", HandleValue(m_pipeline));
        return true;
    }

    Destroy();
    return false;
}

bool WarriorRenderer::RecreatePipeline(VulkanDevice& device)
{
    if (!m_device)
        return true;

    DestroyPipeline();
    if (device.GetRenderPass() == VK_NULL_HANDLE)
        return true;

    return CreatePipeline(device);
}

void WarriorRenderer::Skin(VulkanDevice& device, double timeSeconds)
{
    SkinInstance(device, 0, m_motionState, static_cast<float>(timeSeconds));
}

void WarriorRenderer::SkinInstance(VulkanDevice& device, uint32_t skinSlot, MotionState state, float animTimeSeconds)
{
    if (!m_computePipeline || !device.IsFrameActive())
        return;
    if (skinSlot >= kSkinSlots)
        return;

    const uint32_t frameIndex = device.GetFrameIndex();

    const auto recordStart = std::chrono::high_resolution_clock::now();
    if (!UploadBonePalette(state, animTimeSeconds, frameIndex, skinSlot))
    {
        Log("[COMPUTE] Skin skipped: failed to upload bone palette");
        return;
    }

    VkCommandBuffer cmd = device.GetCommandBuffer();
    DispatchSkin(cmd, frameIndex, skinSlot);

    VkBufferMemoryBarrier computeToVertex{};
    computeToVertex.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    computeToVertex.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    computeToVertex.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    computeToVertex.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    computeToVertex.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    computeToVertex.buffer = m_skinnedOutputBuffers[frameIndex][skinSlot].buffer;
    computeToVertex.offset = 0;
    computeToVertex.size = sizeof(Vertex) * m_vertices.size();
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
        0, 0, nullptr, 1, &computeToVertex, 0, nullptr);

    const auto recordEnd = std::chrono::high_resolution_clock::now();
    const double recordMs = std::chrono::duration<double, std::milli>(recordEnd - recordStart).count();
   //if (timeSeconds - m_lastAnimationLogTime >= 1.0)
   //{
   //    LogFormat("[ANIM] gpu-skin animTime=%.2f/%.2f dispatchMs=%.3f",
   //        animTimeSeconds,
   //        0.0f,
   //        recordMs);
   //    m_lastAnimationLogTime = timeSeconds;
   //}
}

void WarriorRenderer::Render(VulkanDevice& device, double timeSeconds)
{
    static bool loggedNoPipeline = false;
    static bool loggedFrameInactive = false;
    static bool loggedExtentZero = false;
    static bool loggedRectZero = false;
    static bool loggedDraw = false;

    if (!m_pipeline || m_indexCount == 0)
    {
        if (!loggedNoPipeline)
        {
            Log("[MESH] Render skip: no pipeline or indices");
            loggedNoPipeline = true;
        }
        return;
    }

    if (!device.IsFrameActive())
    {
        if (!loggedFrameInactive)
        {
            Log("[MESH] Render skip: frame inactive");
            loggedFrameInactive = true;
        }
        return;
    }

    const VkExtent2D extent = device.GetSwapchainExtent();
    if (extent.width == 0 || extent.height == 0)
    {
        if (!loggedExtentZero)
        {
            Log("[MESH] Render skip: extent 0");
            loggedExtentZero = true;
        }
        return;
    }

    const VkRect2D rect = PreviewRect(extent);
    if (rect.extent.width == 0 || rect.extent.height == 0)
    {
        if (!loggedRectZero)
        {
            LogFormat("[MESH] Render skip: preview rect 0 (extent=%ux%u)", extent.width, extent.height);
            loggedRectZero = true;
        }
        return;
    }

    if (!loggedDraw)
    {
        LogFormat("[MESH] Render: drawing warrior in rect x=%d y=%d w=%u h=%u (swapchain %ux%u)",
            rect.offset.x,
            rect.offset.y,
            rect.extent.width,
            rect.extent.height,
            extent.width,
            extent.height);
        loggedDraw = true;
    }

    const uint32_t frameIndex = device.GetFrameIndex();
    const float aspect = static_cast<float>(rect.extent.width) / static_cast<float>(rect.extent.height);
    UpdateUniform(frameIndex, 0, timeSeconds, aspect);

    VkCommandBuffer cmd = device.GetCommandBuffer();

    VkClearAttachment depthClear{};
    depthClear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depthClear.clearValue.depthStencil.depth = 1.0f;

    VkClearRect depthRect{};
    depthRect.rect = rect;
    depthRect.baseArrayLayer = 0;
    depthRect.layerCount = 1;
    vkCmdClearAttachments(cmd, 1, &depthClear, 1, &depthRect);

    VkViewport viewport{};
    viewport.x = static_cast<float>(rect.offset.x);
    viewport.y = static_cast<float>(rect.offset.y);
    viewport.width = static_cast<float>(rect.extent.width);
    viewport.height = static_cast<float>(rect.extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &rect);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m_skinnedOutputBuffers[frameIndex][0].buffer, &offset);
    vkCmdBindIndexBuffer(cmd, m_indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);

    static bool loggedDraws = false;
    for (size_t i = 0; i < m_draws.size(); ++i)
    {
        const MeshDraw& draw = m_draws[i];
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
            0, 1, &m_descriptorSets[frameIndex][0][draw.textureIndex], 0, nullptr);
        vkCmdDrawIndexed(cmd, draw.indexCount, 1, draw.firstIndex, 0, 0);

        if (!loggedDraws)
        {
            LogFormat("[MESH] drawing mesh[%zu] with texture %s",
                i,
                draw.textureIndex == 1 ? "warrior_face.DDS" : "warrior_4-1.dds");
        }
    }
    loggedDraws = true;

    VkViewport fullViewport{};
    fullViewport.x = 0.0f;
    fullViewport.y = 0.0f;
    fullViewport.width = static_cast<float>(extent.width);
    fullViewport.height = static_cast<float>(extent.height);
    fullViewport.minDepth = 0.0f;
    fullViewport.maxDepth = 1.0f;
    VkRect2D fullScissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &fullViewport);
    vkCmdSetScissor(cmd, 0, 1, &fullScissor);
}

void WarriorRenderer::RenderInWorld(VulkanDevice& device, double, const WorldCamera& camera, WorldVec3 position, float yawRadians, uint32_t skinSlot)
{
    static bool loggedDraw = false;
    static bool loggedNoPipeline = false;

    if (!m_pipeline || m_indexCount == 0)
    {
        if (!loggedNoPipeline)
        {
            Log("[MESH] World render skip: no pipeline or indices");
            loggedNoPipeline = true;
        }
        return;
    }

    if (!device.IsFrameActive())
        return;

    const VkExtent2D extent = device.GetSwapchainExtent();
    if (extent.width == 0 || extent.height == 0)
        return;

    const uint32_t frameIndex = device.GetFrameIndex();
    if (skinSlot >= kSkinSlots)
        skinSlot = 0;
    if (m_worldRenderFrameIndex != frameIndex)
    {
        m_worldRenderFrameIndex = frameIndex;
        m_worldUniformCursor = 0;
    }

    const uint32_t uniformSlot = std::min(m_worldUniformCursor++, kUniformSlots - 1);
    UpdateWorldUniform(frameIndex, uniformSlot, camera, position, yawRadians);

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
    vkCmdBindVertexBuffers(cmd, 0, 1, &m_skinnedOutputBuffers[frameIndex][skinSlot].buffer, &offset);
    vkCmdBindIndexBuffer(cmd, m_indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);

    for (const MeshDraw& draw : m_draws)
    {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
            0, 1, &m_descriptorSets[frameIndex][uniformSlot][draw.textureIndex], 0, nullptr);
        vkCmdDrawIndexed(cmd, draw.indexCount, 1, draw.firstIndex, 0, 0);
    }

    if (!loggedDraw)
    {
        LogFormat("[MESH] World render: warrior pos=(%.2f,%.2f,%.2f) yaw=%.2f camera eye=(%.2f,%.2f,%.2f) target=(%.2f,%.2f,%.2f)",
            position.x,
            position.y,
            position.z,
            yawRadians,
            camera.eye.x,
            camera.eye.y,
            camera.eye.z,
            camera.target.x,
            camera.target.y,
            camera.target.z);
        loggedDraw = true;
    }
}

void WarriorRenderer::Destroy()
{
    DestroyAnimation();

    if (!m_device)
        return;

    DestroyPipeline();

    if (m_descriptorPool)
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
    m_descriptorPool = VK_NULL_HANDLE;

    if (m_descriptorSetLayout)
        vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);
    m_descriptorSetLayout = VK_NULL_HANDLE;

    DestroyComputeResources();
    DestroyBuffer(m_indexBuffer);
    for (auto& frameBuffers : m_uniformBuffers)
    {
        for (Buffer& buffer : frameBuffers)
            DestroyBuffer(buffer);
    }
    for (Texture& texture : m_textures)
        DestroyTexture(texture);

    m_vertices.clear();
    m_indices.clear();
    m_draws.clear();
    m_rawMeshes.clear();
    m_indexCount = 0;
    m_device = VK_NULL_HANDLE;
}

bool WarriorRenderer::LoadGrannyMesh(const std::string& modelPath)
{
    DestroyAnimation();
    m_grannyFile = GrannyReadEntireFile(modelPath.c_str());
    if (!m_grannyFile)
    {
        LogFormat("[MESH] GrannyReadEntireFile failed: %s", modelPath.c_str());
        return false;
    }

    granny_file_info* info = GrannyGetFileInfo(m_grannyFile);
    if (!info)
    {
        Log("[MESH] GrannyGetFileInfo failed");
        DestroyAnimation();
        return false;
    }

    if (info->ModelCount <= 0 || !info->Models[0] || !info->Models[0]->Skeleton)
    {
        Log("[SKIN] model has no usable skeleton");
        DestroyAnimation();
        return false;
    }

    m_model = info->Models[0];
    m_skeleton = m_model->Skeleton;
    const size_t slash = modelPath.find_last_of("\\/");
    const std::string dir = slash == std::string::npos ? std::string(".") : modelPath.substr(0, slash);

    constexpr float kPoseTimeSeconds = 0.0f;
    m_modelInstance = GrannyInstantiateModel(m_model);
    m_localPose = GrannyNewLocalPose(m_skeleton->BoneCount);
    m_worldPose = GrannyNewWorldPose(m_skeleton->BoneCount);
    if (!m_modelInstance || !m_localPose || !m_worldPose)
    {
        Log("[SKIN] failed to allocate Granny pose objects");
        DestroyAnimation();
        return false;
    }

    const std::string baseAnimDir = dir + "\\BaseAnim";
    if (!LoadMotionAnimation(baseAnimDir + "\\wait.gr2", MotionState::Idle) ||
        !LoadMotionAnimation(baseAnimDir + "\\walk.gr2", MotionState::Walk) ||
        !LoadMotionAnimation(baseAnimDir + "\\run.gr2", MotionState::Run))
    {
        DestroyAnimation();
        return false;
    }
    SetMotionState(MotionState::Idle);

    m_vertices.clear();
    m_indices.clear();
    m_rawMeshes.clear();
    m_restVerticesGpu.clear();

    int observedIndexBytes = 0;
    uint32_t invalidRemappedInfluences = 0;

    for (int meshIndex = 0; meshIndex < m_model->MeshBindingCount; ++meshIndex)
    {
        granny_mesh* mesh = m_model->MeshBindings[meshIndex].Mesh;
        if (!mesh)
            continue;

        const int vertexCount = GrannyGetMeshVertexCount(mesh);
        const int indexCount = GrannyGetMeshIndexCount(mesh);
        const int bytesPerIndex = GrannyGetMeshBytesPerIndex(mesh);
        observedIndexBytes = std::max(observedIndexBytes, bytesPerIndex);

        std::vector<SourceVertex> sourceVertices(static_cast<size_t>(vertexCount));
        GrannyCopyMeshVertices(mesh, g_sourceVertexType, sourceVertices.data());

        granny_mesh_binding* meshBinding = GrannyNewMeshBinding(mesh, m_skeleton, m_skeleton);
        if (!meshBinding)
        {
            LogFormat("[SKIN] GrannyNewMeshBinding failed for mesh[%d]", meshIndex);
            DestroyAnimation();
            return false;
        }

        const int bindingBoneCount = GrannyGetMeshBindingBoneCount(meshBinding);
        const granny_int32x* toBoneIndices = GrannyGetMeshBindingToBoneIndices(meshBinding);
        if (!toBoneIndices)
        {
            LogFormat("[SKIN] GrannyGetMeshBindingToBoneIndices failed for mesh[%d]", meshIndex);
            GrannyFreeMeshBinding(meshBinding);
            DestroyAnimation();
            return false;
        }

        const uint32_t baseVertex = static_cast<uint32_t>(m_vertices.size());
        m_vertices.resize(m_vertices.size() + sourceVertices.size());

        RawMesh rawMesh{};
        rawMesh.meshIndex = static_cast<uint32_t>(meshIndex);
        rawMesh.baseVertex = baseVertex;
        rawMesh.vertexCount = static_cast<uint32_t>(vertexCount);
        rawMesh.sourceVertices = std::move(sourceVertices);
        rawMesh.toBoneIndices.assign(toBoneIndices, toBoneIndices + bindingBoneCount);

        m_restVerticesGpu.reserve(m_restVerticesGpu.size() + rawMesh.sourceVertices.size());
        for (const SourceVertex& source : rawMesh.sourceVertices)
        {
            RestVertexGpu gpu{};
            gpu.position[0] = source.position[0];
            gpu.position[1] = source.position[1];
            gpu.position[2] = source.position[2];
            gpu.position[3] = 0.0f;
            gpu.normal[0] = source.normal[0];
            gpu.normal[1] = source.normal[1];
            gpu.normal[2] = source.normal[2];
            gpu.normal[3] = 0.0f;
            gpu.uv[0] = source.uv[0];
            gpu.uv[1] = source.uv[1];
            gpu.uv[2] = 0.0f;
            gpu.uv[3] = 0.0f;
            gpu.packedWeights = PackBytes(
                source.boneWeights[0], source.boneWeights[1],
                source.boneWeights[2], source.boneWeights[3]);

            uint32_t remappedBones[4]{255u, 255u, 255u, 255u};
            for (uint32_t influence = 0; influence < 4; ++influence)
            {
                const uint32_t localBone = source.boneIndices[influence];
                if (localBone < rawMesh.toBoneIndices.size())
                {
                    const int32_t modelBone = rawMesh.toBoneIndices[localBone];
                    if (modelBone >= 0 && modelBone < m_skeleton->BoneCount)
                    {
                        remappedBones[influence] = static_cast<uint32_t>(modelBone);
                        continue;
                    }
                }
                if (source.boneWeights[influence] != 0)
                    ++invalidRemappedInfluences;
            }
            gpu.packedBones = PackBytes(remappedBones[0], remappedBones[1], remappedBones[2], remappedBones[3]);
            m_restVerticesGpu.push_back(gpu);
        }

        m_rawMeshes.push_back(std::move(rawMesh));
        GrannyFreeMeshBinding(meshBinding);

        const uint32_t firstIndex = static_cast<uint32_t>(m_indices.size());
        void* indices = GrannyGetMeshIndices(mesh);
        if (bytesPerIndex == 2)
        {
            const uint16_t* src = static_cast<const uint16_t*>(indices);
            for (int i = 0; i < indexCount; ++i)
                m_indices.push_back(baseVertex + src[i]);
        }
        else if (bytesPerIndex == 4)
        {
            const uint32_t* src = static_cast<const uint32_t*>(indices);
            for (int i = 0; i < indexCount; ++i)
                m_indices.push_back(baseVertex + src[i]);
        }
        else
        {
            LogFormat("[MESH] unsupported index size=%d in mesh[%d]", bytesPerIndex, meshIndex);
            DestroyAnimation();
            return false;
        }

        LogFormat("[MESH] extracted mesh[%d] '%s': verts=%d indices=%d indexSize=%d",
            meshIndex,
            mesh->Name ? mesh->Name : "<null>",
            vertexCount,
            indexCount,
            bytesPerIndex * 8);

        MeshDraw draw{};
        draw.firstIndex = firstIndex;
        draw.indexCount = static_cast<uint32_t>(indexCount);
        draw.textureIndex = MeshUsesFaceTexture(mesh) ? 1u : 0u;
        m_draws.push_back(draw);
        LogFormat("[COMPUTE] rest mesh[%d]: baseVertex=%u vertexCount=%d bindingBones=%d",
            meshIndex,
            baseVertex,
            vertexCount,
            bindingBoneCount);
    }

    if (m_vertices.empty() || m_indices.empty())
    {
        Log("[MESH] no renderable Granny mesh data extracted");
        DestroyAnimation();
        return false;
    }

    LogFormat("[SKIN] static pose sampled from BaseAnim/wait.gr2 at t=%.3f bones=%d duration=%.3f",
        kPoseTimeSeconds,
        m_skeleton->BoneCount,
        m_motionClips[MotionIndex(MotionState::Idle)].duration);
    if (!SkinPose(kPoseTimeSeconds, true, true))
        return false;

    m_indexCount = static_cast<uint32_t>(m_indices.size());

    LogFormat("[MESH] total verts=%zu indices=%zu", m_vertices.size(), m_indices.size());
    LogFormat("[MESH] skinned raw bbox cm min=(%.3f, %.3f, %.3f) max=(%.3f, %.3f, %.3f)",
        m_bounds.min[0], m_bounds.min[1], m_bounds.min[2],
        m_bounds.max[0], m_bounds.max[1], m_bounds.max[2]);
    LogFormat("[MESH] skinned raw bbox center=(%.3f, %.3f, %.3f) displayFitScale=%.3f",
        m_bounds.center[0], m_bounds.center[1], m_bounds.center[2], m_bounds.fitScale);
    LogFormat("[MESH] index size=%d bit, output index buffer=32 bit", observedIndexBytes * 8);
    LogFormat("[COMPUTE] rest-mesh SSBO vertices=%zu stride=%zu invalidRemapSentinels=%u",
        m_restVerticesGpu.size(),
        sizeof(RestVertexGpu),
        invalidRemappedInfluences);

    return true;
}

bool WarriorRenderer::LoadMotionAnimation(const std::string& path, MotionState state)
{
    AnimationClip& clip = m_motionClips[MotionIndex(state)];
    clip.path = path;
    clip.file = GrannyReadEntireFile(path.c_str());
    if (!clip.file)
    {
        LogFormat("[ANIM] GrannyReadEntireFile failed: %s", path.c_str());
        return false;
    }

    granny_file_info* animInfo = GrannyGetFileInfo(clip.file);
    if (!animInfo || animInfo->AnimationCount <= 0 || !animInfo->Animations[0])
    {
        LogFormat("[ANIM] no usable animation in %s", path.c_str());
        return false;
    }

    clip.animation = animInfo->Animations[0];
    clip.duration = clip.animation->Duration > 0.0f ? clip.animation->Duration : 2.0f;
    clip.control = m_modelInstance ? GrannyPlayControlledAnimation(0.0f, clip.animation, m_modelInstance) : nullptr;
    if (!clip.control)
    {
        LogFormat("[ANIM] GrannyPlayControlledAnimation failed: %s", path.c_str());
        return false;
    }

    GrannySetControlWeight(clip.control, 0.0f);
    GrannySetControlSpeed(clip.control, 1.0f);
    GrannySetControlLoopCount(clip.control, 0);
    GrannySetControlActive(clip.control, true);
    LogFormat("[ANIM] loaded state=%s file=%s duration=%.3f",
        MotionStateName(state),
        path.c_str(),
        clip.duration);
    return true;
}

void WarriorRenderer::SetMotionState(MotionState state)
{
    const bool changed = m_motionState != state;

    m_motionState = state;
    for (size_t i = 0; i < m_motionClips.size(); ++i)
    {
        if (m_motionClips[i].control)
            GrannySetControlWeight(m_motionClips[i].control, i == MotionIndex(state) ? 1.0f : 0.0f);
    }

    if (changed)
        LogFormat("[ANIM-PREVIEW] state=%s", MotionStateName(state));
}

bool WarriorRenderer::ApplyMotionControls(float timeSeconds)
{
    return ApplyMotionControls(m_motionState, timeSeconds);
}

bool WarriorRenderer::ApplyMotionControls(MotionState state, float timeSeconds)
{
    bool hasActiveControl = false;
    for (size_t i = 0; i < m_motionClips.size(); ++i)
    {
        AnimationClip& clip = m_motionClips[i];
        if (!clip.control)
            continue;

        GrannySetControlActive(clip.control, true);
        GrannySetControlLoopCount(clip.control, 0);
        GrannySetControlWeight(clip.control, i == MotionIndex(state) ? 1.0f : 0.0f);
        hasActiveControl = true;
    }

    GrannySetModelClock(m_modelInstance, timeSeconds);
    return hasActiveControl;
}

bool WarriorRenderer::SkinPose(float animTimeSeconds, bool updateBounds, bool logSamples)
{
    if (!m_modelInstance || !m_localPose || !m_worldPose || !m_skeleton)
        return false;

    if (!ApplyMotionControls(animTimeSeconds))
        return false;
    GrannySampleModelAnimations(m_modelInstance, 0, m_skeleton->BoneCount, m_localPose);
    GrannyBuildWorldPose(m_skeleton, 0, m_skeleton->BoneCount, m_localPose, nullptr, m_worldPose);

    granny_matrix_4x4* compositeMatrices = GrannyGetWorldPoseComposite4x4Array(m_worldPose);
    if (!compositeMatrices)
    {
        Log("[SKIN] GrannyGetWorldPoseComposite4x4Array returned null");
        return false;
    }

    float minValue[3] = {
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max()};
    float maxValue[3] = {
        -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max()};

    int sampleLogs = 0;
    for (const RawMesh& rawMesh : m_rawMeshes)
    {
        const int bindingBoneCount = static_cast<int>(rawMesh.toBoneIndices.size());
        for (uint32_t vertexIndex = 0; vertexIndex < rawMesh.vertexCount; ++vertexIndex)
        {
            const SourceVertex& source = rawMesh.sourceVertices[vertexIndex];
            Vertex out{};
            int modelBones[4]{};
            SkinVertex(source, compositeMatrices, rawMesh.toBoneIndices.data(), bindingBoneCount,
                out.position, m_skeleton->BoneCount, out.normal, out.uv, modelBones);

            const uint32_t outputIndex = rawMesh.baseVertex + vertexIndex;
            if (outputIndex >= m_vertices.size())
                return false;
            m_vertices[outputIndex] = out;

            if (updateBounds)
            {
                for (int axis = 0; axis < 3; ++axis)
                {
                    minValue[axis] = std::min(minValue[axis], out.position[axis]);
                    maxValue[axis] = std::max(maxValue[axis], out.position[axis]);
                }
            }

            if (logSamples && sampleLogs < 2)
            {
                LogFormat("[SKIN] sample mesh[%u] vertex[%u]: rawPos=(%.3f, %.3f, %.3f) skinnedPos=(%.3f, %.3f, %.3f) weights=(%u,%u,%u,%u) localBones=(%u,%u,%u,%u) modelBones=(%d,%d,%d,%d)",
                    rawMesh.meshIndex,
                    vertexIndex,
                    source.position[0], source.position[1], source.position[2],
                    out.position[0], out.position[1], out.position[2],
                    source.boneWeights[0], source.boneWeights[1], source.boneWeights[2], source.boneWeights[3],
                    source.boneIndices[0], source.boneIndices[1], source.boneIndices[2], source.boneIndices[3],
                    modelBones[0], modelBones[1], modelBones[2], modelBones[3]);
                ++sampleLogs;
            }
        }

        if (logSamples)
        {
            LogFormat("[SKIN] mesh[%u]: skinned %u verts, %zu bones in binding",
                rawMesh.meshIndex,
                rawMesh.vertexCount,
                rawMesh.toBoneIndices.size());
        }
    }

    if (updateBounds)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            m_bounds.min[axis] = minValue[axis];
            m_bounds.max[axis] = maxValue[axis];
            m_bounds.center[axis] = (minValue[axis] + maxValue[axis]) * 0.5f;
        }

        const float sizeX = m_bounds.max[0] - m_bounds.min[0];
        const float sizeY = m_bounds.max[1] - m_bounds.min[1];
        const float sizeZ = m_bounds.max[2] - m_bounds.min[2];
        const float maxDimension = std::max(sizeX, std::max(sizeY, sizeZ)) * 0.01f;
        m_bounds.fitScale = maxDimension > 0.0001f ? (2.35f / maxDimension) : 1.0f;
    }

    return true;
}

bool WarriorRenderer::UploadBonePalette(float animTimeSeconds, uint32_t frameIndex)
{
    return UploadBonePalette(m_motionState, animTimeSeconds, frameIndex, 0);
}

bool WarriorRenderer::UploadBonePalette(MotionState state, float animTimeSeconds, uint32_t frameIndex, uint32_t skinSlot)
{
    if (frameIndex >= kFramesInFlight || skinSlot >= kSkinSlots || !m_bonePaletteBuffers[frameIndex][skinSlot].memory ||
        !m_modelInstance || !m_localPose || !m_worldPose || !m_skeleton)
    {
        return false;
    }

    if (!ApplyMotionControls(state, animTimeSeconds))
        return false;
    GrannySampleModelAnimations(m_modelInstance, 0, m_skeleton->BoneCount, m_localPose);
    GrannyBuildWorldPose(m_skeleton, 0, m_skeleton->BoneCount, m_localPose, nullptr, m_worldPose);
    granny_matrix_4x4* compositeMatrices = GrannyGetWorldPoseComposite4x4Array(m_worldPose);
    if (!compositeMatrices)
        return false;

    const VkDeviceSize size = sizeof(granny_matrix_4x4) * static_cast<size_t>(m_skeleton->BoneCount);
    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(m_device, m_bonePaletteBuffers[frameIndex][skinSlot].memory, 0, size, 0, &mapped));
    std::memcpy(mapped, compositeMatrices, static_cast<size_t>(size));
    vkUnmapMemory(m_device, m_bonePaletteBuffers[frameIndex][skinSlot].memory);
    return true;
}

void WarriorRenderer::DispatchSkin(VkCommandBuffer cmd, uint32_t frameIndex)
{
    DispatchSkin(cmd, frameIndex, 0);
}

void WarriorRenderer::DispatchSkin(VkCommandBuffer cmd, uint32_t frameIndex, uint32_t skinSlot)
{
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_computePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_computePipelineLayout,
        0, 1, &m_computeDescriptorSets[frameIndex][skinSlot], 0, nullptr);

    SkinPushConstants push{};
    push.vertexCount = static_cast<uint32_t>(m_vertices.size());
    push.boneCount = static_cast<uint32_t>(m_skeleton ? m_skeleton->BoneCount : 0);
    vkCmdPushConstants(cmd, m_computePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
        0, sizeof(push), &push);

    const uint32_t groupCount = (push.vertexCount + 63u) / 64u;
    vkCmdDispatch(cmd, groupCount, 1, 1);
}

bool WarriorRenderer::VerifyComputeSkin(VulkanDevice& device)
{
    constexpr float verifyTime = 0.5f;
    if (!SkinPose(verifyTime, false, false))
        return false;
    const std::vector<Vertex> cpuVertices = m_vertices;

    if (!UploadBonePalette(verifyTime, 0))
        return false;

    const VkDeviceSize vertexSize = sizeof(Vertex) * cpuVertices.size();
    Buffer staging{};
    CreateHostVisibleBuffer(device, m_device, vertexSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, nullptr, staging);

    VkQueue graphicsQueue = VK_NULL_HANDLE;
    vkGetDeviceQueue(m_device, device.GetGraphicsQueueFamily(), 0, &graphicsQueue);

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = BeginOneTimeCommands(m_device, device.GetGraphicsQueueFamily(), pool);
    DispatchSkin(cmd, 0);

    VkBufferMemoryBarrier computeToCopy{};
    computeToCopy.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    computeToCopy.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    computeToCopy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    computeToCopy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    computeToCopy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    computeToCopy.buffer = m_skinnedOutputBuffers[0][0].buffer;
    computeToCopy.offset = 0;
    computeToCopy.size = vertexSize;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 1, &computeToCopy, 0, nullptr);

    CopyBuffer(cmd, m_skinnedOutputBuffers[0][0].buffer, staging.buffer, vertexSize);
    EndOneTimeCommands(m_device, graphicsQueue, pool, cmd);

    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(m_device, staging.memory, 0, vertexSize, 0, &mapped));
    const Vertex* gpuVertices = static_cast<const Vertex*>(mapped);

    float maxPosDelta = 0.0f;
    float maxNormalDelta = 0.0f;
    uint32_t withinCount = 0;
    uint32_t offendingLogged = 0;
    for (size_t i = 0; i < cpuVertices.size(); ++i)
    {
        float vertexPosDelta = 0.0f;
        float vertexNormalDelta = 0.0f;
        for (int axis = 0; axis < 3; ++axis)
        {
            vertexPosDelta = std::max(vertexPosDelta, std::fabs(cpuVertices[i].position[axis] - gpuVertices[i].position[axis]));
            vertexNormalDelta = std::max(vertexNormalDelta, std::fabs(cpuVertices[i].normal[axis] - gpuVertices[i].normal[axis]));
        }
        maxPosDelta = std::max(maxPosDelta, vertexPosDelta);
        maxNormalDelta = std::max(maxNormalDelta, vertexNormalDelta);
        if (vertexPosDelta < 0.01f && vertexNormalDelta < 0.001f)
            ++withinCount;
        else if (offendingLogged < 5)
        {
            const RestVertexGpu& rest = m_restVerticesGpu[i];
            LogFormat("[SKIN-VERIFY] mismatch vertex=%zu cpuPos=(%.6f,%.6f,%.6f) gpuPos=(%.6f,%.6f,%.6f) posDelta=%.6f cpuN=(%.6f,%.6f,%.6f) gpuN=(%.6f,%.6f,%.6f) nrmDelta=%.6f weights=0x%08x bones=0x%08x",
                i,
                cpuVertices[i].position[0], cpuVertices[i].position[1], cpuVertices[i].position[2],
                gpuVertices[i].position[0], gpuVertices[i].position[1], gpuVertices[i].position[2],
                vertexPosDelta,
                cpuVertices[i].normal[0], cpuVertices[i].normal[1], cpuVertices[i].normal[2],
                gpuVertices[i].normal[0], gpuVertices[i].normal[1], gpuVertices[i].normal[2],
                vertexNormalDelta,
                rest.packedWeights,
                rest.packedBones);
            ++offendingLogged;
        }
    }
    vkUnmapMemory(m_device, staging.memory);
    DestroyBuffer(staging);

    LogFormat("[SKIN-VERIFY] verts=%zu maxPosDelta=%.6f cm maxNormalDelta=%.6f within(pos<0.01,nrm<0.001)=%u/%zu",
        cpuVertices.size(),
        maxPosDelta,
        maxNormalDelta,
        withinCount,
        cpuVertices.size());

    if (maxPosDelta >= 0.01f || maxNormalDelta >= 0.001f)
    {
        Log("[SKIN-VERIFY] FAILED: GPU compute skinning differs from CPU reference");
        return false;
    }
    return true;
}

bool WarriorRenderer::CreateBuffers(VulkanDevice& device)
{
    CreateHostVisibleBuffer(device, m_device, sizeof(uint32_t) * m_indices.size(),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT, m_indices.data(), m_indexBuffer);

    for (auto& frameBuffers : m_uniformBuffers)
    {
        for (Buffer& buffer : frameBuffers)
        {
            CreateHostVisibleBuffer(device, m_device, sizeof(UniformBlock),
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, nullptr, buffer);
        }
    }
    return true;
}

bool WarriorRenderer::CreateComputeResources(VulkanDevice& device)
{
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    vkGetDeviceQueue(m_device, device.GetGraphicsQueueFamily(), 0, &graphicsQueue);

    const VkDeviceSize restSize = sizeof(RestVertexGpu) * m_restVerticesGpu.size();
    const VkDeviceSize vertexSize = sizeof(Vertex) * m_vertices.size();
    const VkDeviceSize paletteSize = sizeof(granny_matrix_4x4) * static_cast<size_t>(m_skeleton ? m_skeleton->BoneCount : 0);
    if (restSize == 0 || vertexSize == 0 || paletteSize == 0)
    {
        LogFormat("[COMPUTE] invalid buffer sizes rest=%llu output=%llu palette=%llu",
            static_cast<unsigned long long>(restSize),
            static_cast<unsigned long long>(vertexSize),
            static_cast<unsigned long long>(paletteSize));
        return false;
    }

    CreateDeviceLocalBuffer(device, m_device, graphicsQueue, restSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, m_restVerticesGpu.data(), m_restVertexBuffer);

    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        for (uint32_t skinSlot = 0; skinSlot < kSkinSlots; ++skinSlot)
        {
            CreateHostVisibleBuffer(device, m_device, paletteSize,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, nullptr, m_bonePaletteBuffers[frame][skinSlot]);
            CreateDeviceLocalBuffer(device, m_device, graphicsQueue, vertexSize,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                nullptr, m_skinnedOutputBuffers[frame][skinSlot]);
        }
    }

    if (!CreateComputeDescriptors())
        return false;
    if (!CreateComputePipeline())
        return false;
    if (!VerifyComputeSkin(device))
        return false;

    LogFormat("[COMPUTE] resources OK rest=%zu bytes output/frame/slot=%zu bytes palette/frame/slot=%zu bytes slots=%u",
        static_cast<size_t>(restSize),
        static_cast<size_t>(vertexSize),
        static_cast<size_t>(paletteSize),
        kSkinSlots);
    return true;
}

bool WarriorRenderer::CreateTextures(VulkanDevice& device, const std::string& modelPath)
{
    const size_t slash = modelPath.find_last_of("\\/");
    const std::string dir = slash == std::string::npos ? std::string(".") : modelPath.substr(0, slash);
    const std::array<std::string, kTextureCount> textureFiles = {
        dir + "\\warrior_4-1.dds",
        dir + "\\warrior_face.DDS"};

    VkQueue graphicsQueue = VK_NULL_HANDLE;
    vkGetDeviceQueue(m_device, device.GetGraphicsQueueFamily(), 0, &graphicsQueue);

    for (uint32_t textureIndex = 0; textureIndex < kTextureCount; ++textureIndex)
    {
        DdsImage dds{};
        if (!LoadDdsImage(textureFiles[textureIndex], dds))
            return false;

        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(device.GetPhysicalDevice(), dds.format, &props);
        const VkFormatFeatureFlags required =
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if ((props.optimalTilingFeatures & required) != required)
        {
            LogFormat("[DDS] unsupported Vulkan format features for %s format=%s features=0x%08x",
                dds.filename.c_str(),
                VkFormatName(dds.format),
                props.optimalTilingFeatures);
            return false;
        }

        Texture& texture = m_textures[textureIndex];
        texture.name = dds.filename;
        texture.width = dds.width;
        texture.height = dds.height;
        texture.mipLevels = dds.mipLevels;
        texture.format = dds.format;

        CreateDeviceLocalImage(device, m_device, dds.width, dds.height, dds.mipLevels,
            dds.format, texture.image, texture.memory);

        Buffer staging{};
        CreateHostVisibleBuffer(device, m_device, dds.pixels.size(),
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT, dds.pixels.data(), staging);

        VkCommandPool uploadPool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = BeginOneTimeCommands(m_device, device.GetGraphicsQueueFamily(), uploadPool);
        TransitionImageLayout(cmd, texture.image, dds.mipLevels,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyBufferToImage(cmd, staging.buffer, texture.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            static_cast<uint32_t>(dds.regions.size()),
            dds.regions.data());
        TransitionImageLayout(cmd, texture.image, dds.mipLevels,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        EndOneTimeCommands(m_device, graphicsQueue, uploadPool, cmd);

        DestroyBuffer(staging);

        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = texture.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = texture.format;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.baseMipLevel = 0;
        view.subresourceRange.levelCount = texture.mipLevels;
        view.subresourceRange.baseArrayLayer = 0;
        view.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(m_device, &view, nullptr, &texture.view));

        VkSamplerCreateInfo sampler{};
        sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler.magFilter = VK_FILTER_LINEAR;
        sampler.minFilter = VK_FILTER_LINEAR;
        sampler.mipmapMode = texture.mipLevels > 1 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler.minLod = 0.0f;
        sampler.maxLod = static_cast<float>(texture.mipLevels);
        sampler.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        VK_CHECK(vkCreateSampler(m_device, &sampler, nullptr, &texture.sampler));

        LogFormat("[TEX] uploaded %s as %s (%ux%u mips=%u sampler=linear/repeat)",
            texture.name.c_str(),
            VkFormatName(texture.format),
            texture.width,
            texture.height,
            texture.mipLevels);
    }

    return true;
}

bool WarriorRenderer::CreateDescriptors()
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

    VkDescriptorSetLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    std::array<VkDescriptorSetLayoutBinding, 2> bindings = {ubo, diffuse};
    layout.bindingCount = static_cast<uint32_t>(bindings.size());
    layout.pBindings = bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(m_device, &layout, nullptr, &m_descriptorSetLayout));

    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = kFramesInFlight * kUniformSlots * kTextureCount;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = kFramesInFlight * kUniformSlots * kTextureCount;

    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = kFramesInFlight * kUniformSlots * kTextureCount;
    pool.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    pool.pPoolSizes = poolSizes.data();
    VK_CHECK(vkCreateDescriptorPool(m_device, &pool, nullptr, &m_descriptorPool));

    std::array<VkDescriptorSetLayout, kFramesInFlight * kUniformSlots * kTextureCount> layouts{};
    layouts.fill(m_descriptorSetLayout);

    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = m_descriptorPool;
    alloc.descriptorSetCount = static_cast<uint32_t>(layouts.size());
    alloc.pSetLayouts = layouts.data();

    std::array<VkDescriptorSet, kFramesInFlight * kUniformSlots * kTextureCount> flatSets{};
    VK_CHECK(vkAllocateDescriptorSets(m_device, &alloc, flatSets.data()));

    uint32_t setIndex = 0;
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        for (uint32_t uniformSlot = 0; uniformSlot < kUniformSlots; ++uniformSlot)
        {
            for (uint32_t textureIndex = 0; textureIndex < kTextureCount; ++textureIndex)
            {
                VkDescriptorSet descriptorSet = flatSets[setIndex++];
                m_descriptorSets[frame][uniformSlot][textureIndex] = descriptorSet;

                VkDescriptorBufferInfo bufferInfo{};
                bufferInfo.buffer = m_uniformBuffers[frame][uniformSlot].buffer;
                bufferInfo.offset = 0;
                bufferInfo.range = sizeof(UniformBlock);

                VkDescriptorImageInfo imageInfo{};
                imageInfo.sampler = m_textures[textureIndex].sampler;
                imageInfo.imageView = m_textures[textureIndex].view;
                imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                std::array<VkWriteDescriptorSet, 2> writes{};
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
                writes[1].pImageInfo = &imageInfo;
                vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
            }
        }
    }

    return true;
}

bool WarriorRenderer::CreateComputeDescriptors()
{
    VkDescriptorSetLayoutBinding rest{};
    rest.binding = 0;
    rest.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    rest.descriptorCount = 1;
    rest.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutBinding bones{};
    bones.binding = 1;
    bones.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bones.descriptorCount = 1;
    bones.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutBinding output{};
    output.binding = 2;
    output.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    output.descriptorCount = 1;
    output.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    std::array<VkDescriptorSetLayoutBinding, 3> bindings = {rest, bones, output};
    VkDescriptorSetLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout.bindingCount = static_cast<uint32_t>(bindings.size());
    layout.pBindings = bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(m_device, &layout, nullptr, &m_computeDescriptorSetLayout));

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSize.descriptorCount = kFramesInFlight * kSkinSlots * 3;

    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = kFramesInFlight * kSkinSlots;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &poolSize;
    VK_CHECK(vkCreateDescriptorPool(m_device, &pool, nullptr, &m_computeDescriptorPool));

    std::array<VkDescriptorSetLayout, kFramesInFlight * kSkinSlots> layouts{};
    layouts.fill(m_computeDescriptorSetLayout);
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = m_computeDescriptorPool;
    alloc.descriptorSetCount = static_cast<uint32_t>(layouts.size());
    alloc.pSetLayouts = layouts.data();
    std::array<VkDescriptorSet, kFramesInFlight * kSkinSlots> flatSets{};
    VK_CHECK(vkAllocateDescriptorSets(m_device, &alloc, flatSets.data()));
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        for (uint32_t skinSlot = 0; skinSlot < kSkinSlots; ++skinSlot)
            m_computeDescriptorSets[frame][skinSlot] = flatSets[frame * kSkinSlots + skinSlot];
    }

    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        for (uint32_t skinSlot = 0; skinSlot < kSkinSlots; ++skinSlot)
        {
            VkDescriptorBufferInfo restInfo{};
            restInfo.buffer = m_restVertexBuffer.buffer;
            restInfo.range = sizeof(RestVertexGpu) * m_restVerticesGpu.size();

            VkDescriptorBufferInfo bonesInfo{};
            bonesInfo.buffer = m_bonePaletteBuffers[frame][skinSlot].buffer;
            bonesInfo.range = sizeof(granny_matrix_4x4) * static_cast<size_t>(m_skeleton->BoneCount);

            VkDescriptorBufferInfo outputInfo{};
            outputInfo.buffer = m_skinnedOutputBuffers[frame][skinSlot].buffer;
            outputInfo.range = sizeof(Vertex) * m_vertices.size();

            std::array<VkWriteDescriptorSet, 3> writes{};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = m_computeDescriptorSets[frame][skinSlot];
            writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[0].pBufferInfo = &restInfo;

            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = m_computeDescriptorSets[frame][skinSlot];
            writes[1].dstBinding = 1;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[1].pBufferInfo = &bonesInfo;

            writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[2].dstSet = m_computeDescriptorSets[frame][skinSlot];
            writes[2].dstBinding = 2;
            writes[2].descriptorCount = 1;
            writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[2].pBufferInfo = &outputInfo;

            vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
    }

    Log("[COMPUTE] descriptor sets created");
    return true;
}

bool WarriorRenderer::CreateComputePipeline()
{
    const std::string shaderDir = ExecutableDirectory() + "\\shaders\\";
    VkShaderModule cs = CreateShaderModule(m_device, shaderDir + "warrior_cs.spv");

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push.offset = 0;
    push.size = sizeof(SkinPushConstants);

    VkPipelineLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &m_computeDescriptorSetLayout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    VK_CHECK(vkCreatePipelineLayout(m_device, &layout, nullptr, &m_computePipelineLayout));

    VkComputePipelineCreateInfo pipeline{};
    pipeline.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline.stage.module = cs;
    pipeline.stage.pName = "CSMain";
    pipeline.layout = m_computePipelineLayout;
    VK_CHECK(vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &m_computePipeline));

    vkDestroyShaderModule(m_device, cs, nullptr);
    LogFormat("[COMPUTE] pipeline OK, pipeline=0x%llx", HandleValue(m_computePipeline));
    return true;
}

bool WarriorRenderer::CreatePipeline(VulkanDevice& device)
{
    const std::string shaderDir = ExecutableDirectory() + "\\shaders\\";
    VkShaderModule vs = CreateShaderModule(m_device, shaderDir + "warrior_vs.spv");
    VkShaderModule ps = CreateShaderModule(m_device, shaderDir + "warrior_ps.spv");

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
    attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[1].offset = offsetof(Vertex, normal);
    attributes[2].location = 2;
    attributes[2].binding = 0;
    attributes[2].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[2].offset = offsetof(Vertex, uv);

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
    depth.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blendAttachment{};
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

void WarriorRenderer::DestroyPipeline()
{
    if (m_pipeline)
        vkDestroyPipeline(m_device, m_pipeline, nullptr);
    m_pipeline = VK_NULL_HANDLE;

    if (m_pipelineLayout)
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    m_pipelineLayout = VK_NULL_HANDLE;
}

void WarriorRenderer::DestroyBuffer(Buffer& buffer)
{
    if (buffer.buffer)
        vkDestroyBuffer(m_device, buffer.buffer, nullptr);
    if (buffer.memory)
        vkFreeMemory(m_device, buffer.memory, nullptr);
    buffer = {};
}

void WarriorRenderer::DestroyComputeResources()
{
    if (m_computePipeline)
        vkDestroyPipeline(m_device, m_computePipeline, nullptr);
    m_computePipeline = VK_NULL_HANDLE;

    if (m_computePipelineLayout)
        vkDestroyPipelineLayout(m_device, m_computePipelineLayout, nullptr);
    m_computePipelineLayout = VK_NULL_HANDLE;

    if (m_computeDescriptorPool)
        vkDestroyDescriptorPool(m_device, m_computeDescriptorPool, nullptr);
    m_computeDescriptorPool = VK_NULL_HANDLE;

    if (m_computeDescriptorSetLayout)
        vkDestroyDescriptorSetLayout(m_device, m_computeDescriptorSetLayout, nullptr);
    m_computeDescriptorSetLayout = VK_NULL_HANDLE;

    DestroyBuffer(m_restVertexBuffer);
    for (auto& frameBuffers : m_bonePaletteBuffers)
    {
        for (Buffer& buffer : frameBuffers)
            DestroyBuffer(buffer);
    }
    for (auto& frameBuffers : m_skinnedOutputBuffers)
    {
        for (Buffer& buffer : frameBuffers)
            DestroyBuffer(buffer);
    }
}

void WarriorRenderer::DestroyAnimation()
{
    if (m_worldPose)
        GrannyFreeWorldPose(m_worldPose);
    if (m_localPose)
        GrannyFreeLocalPose(m_localPose);
    for (AnimationClip& clip : m_motionClips)
    {
        if (clip.control)
            GrannyFreeControl(clip.control);
        if (clip.file)
            GrannyFreeFile(clip.file);
        clip = {};
    }
    if (m_modelInstance)
        GrannyFreeModelInstance(m_modelInstance);
    if (m_grannyFile)
        GrannyFreeFile(m_grannyFile);

    m_worldPose = nullptr;
    m_localPose = nullptr;
    m_modelInstance = nullptr;
    m_grannyFile = nullptr;
    m_model = nullptr;
    m_skeleton = nullptr;
    m_motionState = MotionState::Idle;
    m_lastAnimationLogTime = -1000.0;
}

void WarriorRenderer::DestroyTexture(Texture& texture)
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

void WarriorRenderer::UpdateUniform(uint32_t frameIndex, uint32_t uniformSlot, double timeSeconds, float aspect)
{
    static bool loggedMvp = false;

    const Mat4 center = Translation(-m_bounds.center[0], -m_bounds.center[1], -m_bounds.center[2]);
    const Mat4 display = RawGrannyToDisplay();
    const Mat4 fit = Scale(m_bounds.fitScale);
    const Mat4 spin = RotationY(static_cast<float>(timeSeconds) * 0.55f);
    const Mat4 place = Translation(0.0f, 0.0f, 4.0f);
    const Mat4 model = Multiply(Multiply(Multiply(Multiply(center, display), fit), spin), place);
    const Mat4 projection = Perspective(45.0f * 3.1415926535f / 180.0f, aspect, 0.1f, 50.0f);
    const Mat4 mvp = Multiply(model, projection);

    if (!loggedMvp)
    {
        LogFormat("[MESH] MVP: raw skinned cm/Z-up vertices, display transform in model matrix (cmScale=0.01, Z-up->Y-up), fitScale=%.3f, translateZ=4.0, fov=45, near=0.1 far=50, aspect=%.3f",
            m_bounds.fitScale,
            aspect);
        loggedMvp = true;
    }

    const UniformBlock uniform{mvp, model};

    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(m_device, m_uniformBuffers[frameIndex][uniformSlot].memory, 0, sizeof(uniform), 0, &mapped));
    std::memcpy(mapped, &uniform, sizeof(uniform));
    vkUnmapMemory(m_device, m_uniformBuffers[frameIndex][uniformSlot].memory);
}

void WarriorRenderer::UpdateWorldUniform(uint32_t frameIndex, uint32_t uniformSlot, const WorldCamera& camera, WorldVec3 position, float yawRadians)
{
    static bool loggedMvp = false;

    const Mat4 model = Multiply(Multiply(RawGrannyToDisplay(), RotationY(-yawRadians)),
        Translation(position.x, position.y, position.z));
    const Mat4 viewProjection = ToLocalMat4(camera.viewProjection);
    const Mat4 mvp = Multiply(model, viewProjection);

    if (!loggedMvp)
    {
        Log("[MESH] World MVP: raw skinned cm/Z-up vertices -> display meters/Y-up, spawn translated to local origin, shared full-screen camera");
        loggedMvp = true;
    }

    const UniformBlock uniform{mvp, model};

    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(m_device, m_uniformBuffers[frameIndex][uniformSlot].memory, 0, sizeof(uniform), 0, &mapped));
    std::memcpy(mapped, &uniform, sizeof(uniform));
    vkUnmapMemory(m_device, m_uniformBuffers[frameIndex][uniformSlot].memory);
}
