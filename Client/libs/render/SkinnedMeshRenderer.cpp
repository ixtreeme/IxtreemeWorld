#include "SkinnedMeshRenderer.h"

#include "AssimpImporter.h"
#include "Debug.h"
#include "math/IXMath.h"
#include "asset/IAssetReader.h"

#include <fastgltf/core.hpp>
#include <fastgltf/math.hpp>
#include <fastgltf/tools.hpp>
#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/local_to_model_job.h>
#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>
#include <ozz/base/maths/simd_math.h>
#include <ozz/base/maths/soa_transform.h>
#include <ozz/base/span.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

struct SkinnedMeshRenderer::OzzRuntime
{
    ozz::animation::Skeleton skeleton;
    ozz::animation::Animation idle;
    ozz::animation::SamplingJob::Context context;
    std::vector<ozz::math::SoaTransform> locals;
    std::vector<ozz::math::Float4x4> models;
    bool hasIdle = false;
};

namespace
{
namespace xm = ixtreeme::math;

void Log(const char* text)
{
    Tracen(text);
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

using Mat4 = ixtreeme::math::Mat4;

Mat4 Translation(float x, float y, float z);

struct UniformBlock
{
    struct PointLightUniform
    {
        float position[4];
        float color[4];
    };

    struct SpotLightUniform
    {
        float position[4];
        float direction[4];
        float color[4];
    };

    Mat4 mvp;
    Mat4 model;
    float tint[4];
    float sunDir[4];
    float sunColor[4];
    float ambientColor[4];
    float waterParams[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float causticParams[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    std::int32_t numPointLights = 0;
    std::int32_t numSpotLights = 0;
    float lightPadding[2] = {0.0f, 0.0f};
    PointLightUniform pointLights[kMaxDynamicPointLights]{};
    SpotLightUniform spotLights[kMaxDynamicSpotLights]{};
};

static_assert(sizeof(SkinnedMeshRenderer::Vertex) == 32, "Graphics vertex layout must stay 32 bytes");

void FillLightingUniform(const LightingState& lighting, UniformBlock& uniform)
{
    const DirectionalLight& directional = lighting.directional;
    const AmbientLight& ambient = lighting.ambient;
    const float azimuthRadians = xm::DegreesToRadians(std::clamp(directional.azimuthDegrees, 0.0f, 360.0f));
    const float elevationRadians = xm::DegreesToRadians(std::clamp(directional.elevationDegrees, 0.0f, 90.0f));
    const float cosElevation = std::cos(elevationRadians);
    const float sunIntensity = std::max(0.0f, directional.intensity) * (directional.enabled ? 1.0f : 0.0f);
    const float ambientIntensity = std::max(0.0f, ambient.intensity);
    uniform.sunDir[0] = cosElevation * std::sin(azimuthRadians);
    uniform.sunDir[1] = std::sin(elevationRadians);
    uniform.sunDir[2] = cosElevation * std::cos(azimuthRadians);
    uniform.sunDir[3] = 0.0f;
    uniform.sunColor[0] = std::max(0.0f, directional.r) * sunIntensity;
    uniform.sunColor[1] = std::max(0.0f, directional.g) * sunIntensity;
    uniform.sunColor[2] = std::max(0.0f, directional.b) * sunIntensity;
    uniform.sunColor[3] = 0.0f;
    uniform.ambientColor[0] = std::max(0.0f, ambient.r) * ambientIntensity;
    uniform.ambientColor[1] = std::max(0.0f, ambient.g) * ambientIntensity;
    uniform.ambientColor[2] = std::max(0.0f, ambient.b) * ambientIntensity;
    uniform.ambientColor[3] = 0.0f;
    uniform.numPointLights = static_cast<std::int32_t>(
        std::min<std::uint32_t>(lighting.numPointLights, kMaxDynamicPointLights));
    uniform.numSpotLights = static_cast<std::int32_t>(
        std::min<std::uint32_t>(lighting.numSpotLights, kMaxDynamicSpotLights));
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(uniform.numPointLights); ++i)
    {
        const PointLight& point = lighting.pointLights[i];
        auto& out = uniform.pointLights[i];
        out.position[0] = point.position[0];
        out.position[1] = point.position[1];
        out.position[2] = point.position[2];
        out.position[3] = std::max(0.1f, point.radius);
        const float intensity = point.enabled ? std::max(0.0f, point.intensity) : 0.0f;
        out.color[0] = std::max(0.0f, point.r);
        out.color[1] = std::max(0.0f, point.g);
        out.color[2] = std::max(0.0f, point.b);
        out.color[3] = intensity;
    }
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(uniform.numSpotLights); ++i)
    {
        SpotLight spot = lighting.spotLights[i];
        spot.outerConeDegrees = std::clamp(spot.outerConeDegrees, 1.0f, 90.0f);
        spot.innerConeDegrees = std::clamp(spot.innerConeDegrees, 1.0f, spot.outerConeDegrees);
        const float pitch = spot.rotation[0];
        const float yaw = spot.rotation[1];
        const float cosPitch = std::cos(pitch);
        auto& out = uniform.spotLights[i];
        out.position[0] = spot.position[0];
        out.position[1] = spot.position[1];
        out.position[2] = spot.position[2];
        out.position[3] = std::max(0.1f, spot.radius);
        out.direction[0] = std::sin(yaw) * cosPitch;
        out.direction[1] = std::sin(pitch);
        out.direction[2] = std::cos(yaw) * cosPitch;
        out.direction[3] = std::cos(xm::DegreesToRadians(spot.innerConeDegrees));
        const float intensity = spot.enabled ? std::max(0.0f, spot.intensity) : 0.0f;
        out.color[0] = std::max(0.0f, spot.r) * intensity;
        out.color[1] = std::max(0.0f, spot.g) * intensity;
        out.color[2] = std::max(0.0f, spot.b) * intensity;
        out.color[3] = std::cos(xm::DegreesToRadians(spot.outerConeDegrees));
        out.direction[3] = std::max(out.direction[3], out.color[3]);
    }
}

void FillWaterUniform(const WaterConfig& water, double timeSeconds, UniformBlock& uniform)
{
    uniform.waterParams[0] = water.enabled ? 1.0f : 0.0f;
    uniform.waterParams[1] = water.waterLevelY;
    uniform.waterParams[2] = static_cast<float>(static_cast<int>(water.causticMode));
    uniform.waterParams[3] = std::clamp(water.causticIntensity, 0.0f, 3.0f);
    uniform.causticParams[0] = std::clamp(water.causticScale, 0.05f, 2.0f);
    uniform.causticParams[1] = static_cast<float>(timeSeconds) * std::clamp(water.causticSpeed, 0.0f, 2.0f);
    uniform.causticParams[2] = std::clamp(water.causticMaxDepth, 1.0f, 30.0f);
    uniform.causticParams[3] = 0.0f;
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

Mat4 Identity()
{
    return xm::Mat4Identity();
}

Mat4 Multiply(const Mat4& a, const Mat4& b)
{
    return xm::MultiplyRowMajor(a, b);
}

Mat4 Scale(float value)
{
    return xm::Scale({value, value, value});
}

Mat4 RotationY(float angle)
{
    return xm::RotationYRowMajor(angle);
}

Mat4 Translation(float x, float y, float z)
{
    return xm::Translation({x, y, z});
}

Mat4 Perspective(float fovYRadians, float aspect, float zNear, float zFar)
{
    return xm::PerspectiveVulkan(fovYRadians, aspect, zNear, zFar);
}

Mat4 ToLocalMat4(const WorldMat4& matrix)
{
    Mat4 r{};
    std::memcpy(r.m, matrix.m, sizeof(r.m));
    return r;
}

const char* MotionStateName(SkinnedMeshRenderer::MotionState state)
{
    switch (state)
    {
    case SkinnedMeshRenderer::MotionState::Idle: return "wait";
    case SkinnedMeshRenderer::MotionState::Walk: return "walk";
    case SkinnedMeshRenderer::MotionState::Run: return "run";
    default: return "unknown";
    }
}

std::vector<char> ReadBinaryFile(client::asset::IAssetReader& assets, const std::string& path)
{
    auto bytes = assets.ReadAll(path);
    if (!bytes)
    {
        std::string message = "Failed to open shader: " + path;
        Log(message.c_str());
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
    case VK_FORMAT_R8G8B8A8_UNORM: return "VK_FORMAT_R8G8B8A8_UNORM";
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

bool LoadDdsImage(client::asset::IAssetReader& assets, const std::string& path, DdsImage& out)
{
    auto maybeBytes = assets.ReadAll(path);
    if (!maybeBytes)
    {
        LogFormat("[DDS] failed to open %s", path.c_str());
        return false;
    }

    std::vector<uint8_t> bytes = std::move(*maybeBytes);

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

DdsImage CreateFallbackWhiteDdsImage(const std::string& sourcePath)
{
    DdsImage out{};
    const size_t slash = sourcePath.find_last_of("\\/");
    const std::string filename = slash == std::string::npos ? sourcePath : sourcePath.substr(slash + 1);

    out.filename = filename + " (fallback white)";
    out.width = 4;
    out.height = 4;
    out.mipLevels = 1;
    out.bytesPerPixel = 4;
    out.compressed = false;
    out.srgb = false;
    out.format = VK_FORMAT_R8G8B8A8_UNORM;
    out.pixels.assign(static_cast<size_t>(out.width) * out.height * out.bytesPerPixel, 0xff);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {out.width, out.height, 1};
    out.regions.push_back(region);
    return out;
}

bool CopyDataSourceBytes(const fastgltf::Asset& asset, const fastgltf::DataSource& source,
    size_t byteOffset, size_t byteLength, std::vector<uint8_t>& out);

bool CopyBufferViewBytes(const fastgltf::Asset& asset, size_t bufferViewIndex, std::vector<uint8_t>& out)
{
    if (bufferViewIndex >= asset.bufferViews.size())
        return false;

    const auto& view = asset.bufferViews[bufferViewIndex];
    if (view.bufferIndex >= asset.buffers.size())
        return false;

    return CopyDataSourceBytes(asset, asset.buffers[view.bufferIndex].data,
        view.byteOffset, view.byteLength, out);
}

bool CopyDataSourceBytes(const fastgltf::Asset& asset, const fastgltf::DataSource& source,
    size_t byteOffset, size_t byteLength, std::vector<uint8_t>& out)
{
    const std::byte* data = nullptr;
    size_t size = 0;

    if (const auto* bufferView = std::get_if<fastgltf::sources::BufferView>(&source))
        return CopyBufferViewBytes(asset, bufferView->bufferViewIndex, out);
    if (const auto* array = std::get_if<fastgltf::sources::Array>(&source))
    {
        data = array->bytes.data();
        size = array->bytes.size();
    }
    else if (const auto* vector = std::get_if<fastgltf::sources::Vector>(&source))
    {
        data = vector->bytes.data();
        size = vector->bytes.size();
    }
    else if (const auto* byteView = std::get_if<fastgltf::sources::ByteView>(&source))
    {
        data = byteView->bytes.data();
        size = byteView->bytes.size();
    }
    else
    {
        return false;
    }

    if (!data || byteOffset > size)
        return false;
    const size_t available = size - byteOffset;
    const size_t length = byteLength == std::numeric_limits<size_t>::max()
        ? available
        : byteLength;
    if (length > available)
        return false;

    const auto* begin = reinterpret_cast<const uint8_t*>(data + byteOffset);
    out.assign(begin, begin + length);
    return true;
}

DdsImage CreateRgbaImage(std::string name, uint32_t width, uint32_t height, std::vector<uint8_t> pixels)
{
    DdsImage out{};
    out.filename = std::move(name);
    out.width = width;
    out.height = height;
    out.mipLevels = 1;
    out.bytesPerPixel = 4;
    out.compressed = false;
    out.srgb = true;
    out.format = VK_FORMAT_R8G8B8A8_SRGB;
    out.pixels = std::move(pixels);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {width, height, 1};
    out.regions.push_back(region);
    return out;
}

bool LoadGltfBaseColorTexture(client::asset::IAssetReader& assets,
    const std::string& modelPath, DdsImage& out)
{
    const size_t slash = modelPath.find_last_of("\\/");
    const std::string dir = slash == std::string::npos ? std::string(".") : modelPath.substr(0, slash);

    auto modelBytes = assets.ReadAll(modelPath);
    if (!modelBytes)
        return false;

    auto data = fastgltf::GltfDataBuffer::FromBytes(
        reinterpret_cast<const std::byte*>(modelBytes->data()), modelBytes->size());
    if (data.error() != fastgltf::Error::None)
        return false;

    fastgltf::Parser parser;
    auto assetResult = parser.loadGltf(data.get(), std::filesystem::path(dir),
        fastgltf::Options::DecomposeNodeMatrices);
    if (assetResult.error() != fastgltf::Error::None)
        return false;

    fastgltf::Asset asset = std::move(assetResult.get());
    for (const auto& material : asset.materials)
    {
        if (!material.pbrData.baseColorTexture.has_value())
            continue;

        const size_t textureIndex = material.pbrData.baseColorTexture->textureIndex;
        if (textureIndex >= asset.textures.size())
            continue;

        const auto& texture = asset.textures[textureIndex];
        if (!texture.imageIndex.has_value() || texture.imageIndex.value() >= asset.images.size())
            continue;

        const auto& image = asset.images[texture.imageIndex.value()];
        std::vector<uint8_t> encoded;
        if (!CopyDataSourceBytes(asset, image.data, 0, std::numeric_limits<size_t>::max(), encoded) ||
            encoded.empty())
            continue;

        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_uc* decoded = stbi_load_from_memory(
            encoded.data(), static_cast<int>(encoded.size()), &width, &height, &channels, 4);
        if (!decoded || width <= 0 || height <= 0)
        {
            if (decoded)
                stbi_image_free(decoded);
            continue;
        }

        std::vector<uint8_t> pixels(
            decoded, decoded + (static_cast<size_t>(width) * static_cast<size_t>(height) * 4u));
        stbi_image_free(decoded);

        out = CreateRgbaImage(std::string(image.name.empty() ? "glTF baseColorTexture" : image.name),
            static_cast<uint32_t>(width), static_cast<uint32_t>(height), std::move(pixels));
        LogFormat("[GLTF-TEX] decoded baseColorTexture material='%s' image='%s' (%ux%u, source channels=%d)",
            material.name.c_str(),
            out.filename.c_str(),
            out.width,
            out.height,
            channels);
        return true;
    }

    return false;
}

bool LoadRgbaTextureFile(const std::filesystem::path& path, DdsImage& out)
{
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* decoded = stbi_load(path.string().c_str(), &width, &height, &channels, 4);
    if (!decoded || width <= 0 || height <= 0)
    {
        if (decoded)
            stbi_image_free(decoded);
        return false;
    }

    std::vector<uint8_t> pixels(
        decoded, decoded + (static_cast<size_t>(width) * static_cast<size_t>(height) * 4u));
    stbi_image_free(decoded);

    out = CreateRgbaImage(path.filename().generic_string(),
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height),
        std::move(pixels));
    LogFormat("[FBX-IMPORT] skinned diffuse texture loaded path=%s (%ux%u, source channels=%d)",
        path.generic_string().c_str(),
        out.width,
        out.height,
        channels);
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

VkCommandBuffer BeginOneTimeCommands(VkDevice vkDevice, uint32_t queueFamily, VkCommandPool& pool);
void EndOneTimeCommands(VkDevice vkDevice, VkQueue queue, VkCommandPool pool, VkCommandBuffer cmd);

bool CreateHostVisibleBuffer(VulkanDevice& device, VkDevice vkDevice, VkDeviceSize size,
    VkBufferUsageFlags usage, const void* initialData, SkinnedMeshRenderer::Buffer& out)
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
    VkBufferUsageFlags usage, const void* initialData, SkinnedMeshRenderer::Buffer& out)
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
        SkinnedMeshRenderer::Buffer staging{};
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
    // TODO: derive this from the RmlUi lobby preview-frame bounds instead of mirroring the current fixed layout.
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

void TransformPointRowVector(const float* matrix, const float in[3], float out[3])
{
    out[0] = in[0] * matrix[0] + in[1] * matrix[4] + in[2] * matrix[8] + matrix[12];
    out[1] = in[0] * matrix[1] + in[1] * matrix[5] + in[2] * matrix[9] + matrix[13];
    out[2] = in[0] * matrix[2] + in[1] * matrix[6] + in[2] * matrix[10] + matrix[14];
}

void TransformVectorRowVector(const float* matrix, const float in[3], float out[3])
{
    out[0] = in[0] * matrix[0] + in[1] * matrix[4] + in[2] * matrix[8];
    out[1] = in[0] * matrix[1] + in[1] * matrix[5] + in[2] * matrix[9];
    out[2] = in[0] * matrix[2] + in[1] * matrix[6] + in[2] * matrix[10];
}

void SkinVertex(const SkinnedMeshRenderer::SourceVertex& source,
    const std::vector<Mat4>& bonePalette, float outPosition[3],
    float outNormal[3], float outUv[2], int modelBones[4])
{
    outPosition[0] = outPosition[1] = outPosition[2] = 0.0f;
    outNormal[0] = outNormal[1] = outNormal[2] = 0.0f;
    outUv[0] = outUv[1] = 0.0f;
    modelBones[0] = modelBones[1] = modelBones[2] = modelBones[3] = -1;

    int totalWeight = 0;
    for (int i = 0; i < 4; ++i)
        totalWeight += source.boneWeights[i];

    if (totalWeight <= 0 || bonePalette.empty())
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

        const int modelBone = source.boneIndices[influence];
        modelBones[influence] = modelBone;
        if (modelBone < 0 || modelBone >= static_cast<int>(bonePalette.size()))
            continue;

        const float weight = static_cast<float>(weightByte) / static_cast<float>(totalWeight);
        const float* matrix = bonePalette[modelBone].m;

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

template <typename T>
bool ReadOzzObject(client::asset::IAssetReader& assets, const std::string& path, T& out)
{
    auto bytes = assets.ReadAll(path);
    if (!bytes)
    {
        LogFormat("[OZZ] missing archive: %s", path.c_str());
        return false;
    }

    ozz::io::MemoryStream stream;
    if (stream.Write(bytes->data(), bytes->size()) != bytes->size())
    {
        LogFormat("[OZZ] failed to stage archive in memory: %s", path.c_str());
        return false;
    }
    stream.Seek(0, ozz::io::Stream::kSet);
    ozz::io::IArchive archive(&stream);
    if (!archive.TestTag<T>())
    {
        LogFormat("[OZZ] archive type mismatch: %s", path.c_str());
        return false;
    }
    archive >> out;
    return true;
}

xm::Mat4 IdentityPaletteMatrix()
{
    return xm::Mat4Identity();
}

xm::Mat4 ToRowMajorMatrix(const ozz::math::Float4x4& matrix)
{
    float columns[4][4]{};
    for (int col = 0; col < 4; ++col)
        ozz::math::StorePtrU(matrix.cols[col], columns[col]);

    xm::Mat4 out{};
    for (int row = 0; row < 4; ++row)
    {
        for (int col = 0; col < 4; ++col)
            out.m[row * 4 + col] = columns[col][row];
    }
    return out;
}

xm::Mat4 ToRowVectorPaletteMatrix(const ozz::math::Float4x4& matrix)
{
    float columns[4][4]{};
    for (int col = 0; col < 4; ++col)
        ozz::math::StorePtrU(matrix.cols[col], columns[col]);

    xm::Mat4 out{};
    for (int row = 0; row < 4; ++row)
    {
        for (int col = 0; col < 4; ++col)
            out.m[row * 4 + col] = columns[row][col];
    }
    return out;
}

ozz::math::Float4x4 ToOzzMatrix(const fastgltf::math::fmat4x4& matrix)
{
    ozz::math::Float4x4 out{};
    for (int col = 0; col < 4; ++col)
    {
        out.cols[col] = ozz::math::simd_float4::Load(
            matrix[col][0], matrix[col][1], matrix[col][2], matrix[col][3]);
    }
    return out;
}

uint8_t ToWeightByte(float weight)
{
    const float clamped = std::clamp(weight, 0.0f, 1.0f);
    return static_cast<uint8_t>(std::lround(clamped * 255.0f));
}

std::string DirectoryOf(const std::string& path)
{
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

uint32_t PackBytes(uint32_t b0, uint32_t b1, uint32_t b2, uint32_t b3)
{
    return (b0 & 0xffu) |
        ((b1 & 0xffu) << 8u) |
        ((b2 & 0xffu) << 16u) |
        ((b3 & 0xffu) << 24u);
}
}

SkinnedMeshRenderer::SkinnedMeshRenderer() = default;

SkinnedMeshRenderer::~SkinnedMeshRenderer()
{
    Destroy();
}

bool SkinnedMeshRenderer::Create(VulkanDevice& device, client::asset::IAssetReader& assets,
    const std::string& modelPath)
{
    Destroy();
    m_device = device.GetDevice();
    m_assets = &assets;

    std::string ext = std::filesystem::path(modelPath).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    const bool loaded = ext == ".fbx" ? LoadFbxMesh(modelPath) : LoadGltfMesh(modelPath);
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

bool SkinnedMeshRenderer::RecreatePipeline(VulkanDevice& device)
{
    if (!m_device)
        return true;

    DestroyPipeline();
    if ((m_mainRenderPass ? m_mainRenderPass : device.GetRenderPass()) == VK_NULL_HANDLE)
        return true;

    return CreatePipeline(device);
}

void SkinnedMeshRenderer::SetMainRenderPass(VkRenderPass renderPass)
{
    m_mainRenderPass = renderPass;
}

void SkinnedMeshRenderer::Skin(VulkanDevice& device, double timeSeconds)
{
    SkinInstance(device, 0, m_motionState, static_cast<float>(timeSeconds));
}

void SkinnedMeshRenderer::SkinInstance(VulkanDevice& device, uint32_t skinSlot, MotionState state, float animTimeSeconds)
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

void SkinnedMeshRenderer::Render(VulkanDevice& device, double timeSeconds)
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
        LogFormat("[MESH] Render: drawing skinnedMesh in rect x=%d y=%d w=%u h=%u (swapchain %ux%u)",
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
                draw.textureIndex < m_textures.size() ? m_textures[draw.textureIndex].name.c_str() : "<invalid>");
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

void SkinnedMeshRenderer::RenderInWorld(VulkanDevice& device,
    double timeSeconds,
    const WorldCamera& camera,
    WorldVec3 position,
    float yawRadians,
    uint32_t skinSlot,
    std::array<float, 4> tint,
    VkExtent2D targetExtent)
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

    const VkExtent2D extent = (targetExtent.width > 0 && targetExtent.height > 0)
        ? targetExtent
        : device.GetSwapchainExtent();
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
    UpdateWorldUniform(frameIndex, uniformSlot, camera, position, yawRadians, timeSeconds, tint);

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
        LogFormat("[MESH] World render: skinnedMesh pos=(%.2f,%.2f,%.2f) yaw=%.2f camera eye=(%.2f,%.2f,%.2f) target=(%.2f,%.2f,%.2f)",
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

void SkinnedMeshRenderer::RenderInWorldReflection(VulkanDevice& device,
    const WorldCamera& camera,
    VkExtent2D extent,
    VkRenderPass renderPass,
    float waterLevelY,
    WorldVec3 position,
    float yawRadians,
    uint32_t skinSlot,
    std::array<float, 4> tint)
{
    if (!m_pipelineLayout || !renderPass || m_indexCount == 0 || !device.IsFrameActive())
        return;

    if (!m_reflectionPipeline || m_reflectionRenderPass != renderPass)
    {
        DestroyReflectionPipeline();
        if (!CreateReflectionPipeline(device, renderPass))
            return;
    }

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
    UpdateWorldUniform(frameIndex, uniformSlot, camera, position, yawRadians, 0.0, tint, true, waterLevelY);

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
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_reflectionPipeline);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m_skinnedOutputBuffers[frameIndex][skinSlot].buffer, &offset);
    vkCmdBindIndexBuffer(cmd, m_indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);

    for (const MeshDraw& draw : m_draws)
    {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
            0, 1, &m_descriptorSets[frameIndex][uniformSlot][draw.textureIndex], 0, nullptr);
        vkCmdDrawIndexed(cmd, draw.indexCount, 1, draw.firstIndex, 0, 0);
    }
}

void SkinnedMeshRenderer::Destroy()
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
    m_assets = nullptr;
}

bool SkinnedMeshRenderer::LoadGltfMesh(const std::string& modelPath)
{
    DestroyAnimation();
    m_importedDiffuseTexturePath.clear();
    if (!m_assets)
        return false;

    if (!LoadOzzPose(modelPath))
        return false;
    const std::string dir = DirectoryOf(modelPath);

    auto modelBytes = m_assets->ReadAll(modelPath);
    if (!modelBytes)
    {
        LogFormat("[GLTF] failed to read model: %s", modelPath.c_str());
        return false;
    }

    auto data = fastgltf::GltfDataBuffer::FromBytes(
        reinterpret_cast<const std::byte*>(modelBytes->data()), modelBytes->size());
    if (data.error() != fastgltf::Error::None)
    {
        LogFormat("[GLTF] data buffer error for %s: %s", modelPath.c_str(),
            fastgltf::getErrorMessage(data.error()).data());
        return false;
    }

    fastgltf::Parser parser;
    auto assetResult = parser.loadGltf(data.get(), std::filesystem::path(dir),
        fastgltf::Options::DecomposeNodeMatrices);
    if (assetResult.error() != fastgltf::Error::None)
    {
        LogFormat("[GLTF] parse error for %s: %s", modelPath.c_str(),
            fastgltf::getErrorMessage(assetResult.error()).data());
        return false;
    }
    fastgltf::Asset asset = std::move(assetResult.get());

    std::unordered_map<std::string, uint32_t> ozzJointByName;
    auto jointNames = m_ozz->skeleton.joint_names();
    for (int i = 0; i < m_ozz->skeleton.num_joints(); ++i)
        ozzJointByName.emplace(jointNames[i], static_cast<uint32_t>(i));

    std::vector<std::vector<uint32_t>> skinJointRemaps(asset.skins.size());
    for (size_t skinIndex = 0; skinIndex < asset.skins.size(); ++skinIndex)
    {
        const auto& skin = asset.skins[skinIndex];
        auto& remaps = skinJointRemaps[skinIndex];
        remaps.assign(skin.joints.size(), 255u);
        for (size_t localJoint = 0; localJoint < skin.joints.size(); ++localJoint)
        {
            const auto& node = asset.nodes[skin.joints[localJoint]];
            auto it = ozzJointByName.find(std::string(node.name));
            if (it != ozzJointByName.end())
                remaps[localJoint] = it->second;
        }

        if (skin.inverseBindMatrices.has_value())
        {
            const auto& accessor = asset.accessors[skin.inverseBindMatrices.value()];
            size_t matrixIndex = 0;
            fastgltf::iterateAccessor<fastgltf::math::fmat4x4>(
                asset, accessor, [&](fastgltf::math::fmat4x4 matrix)
                {
                    if (matrixIndex < remaps.size() && remaps[matrixIndex] < m_inverseBindMatrices.size())
                    {
                        const auto ozzMatrix = ToOzzMatrix(matrix);
                        m_inverseBindMatrices[remaps[matrixIndex]] = ToRowMajorMatrix(ozzMatrix);
                    }
                    ++matrixIndex;
                });
        }
    }

    m_vertices.clear();
    m_indices.clear();
    m_rawMeshes.clear();
    m_restVerticesGpu.clear();
    m_draws.clear();

    uint32_t invalidRemappedInfluences = 0;
    uint32_t primitiveIndex = 0;
    int observedIndexBytes = 0;

    for (size_t nodeIndex = 0; nodeIndex < asset.nodes.size(); ++nodeIndex)
    {
        const auto& node = asset.nodes[nodeIndex];
        if (!node.meshIndex.has_value())
            continue;
        LogFormat("[GLTF] loading skinned mesh '%s' from node '%s' in mesh-local coordinates",
            modelPath.c_str(),
            std::string(node.name).c_str());
        const std::vector<uint32_t>* remaps = nullptr;
        if (node.skinIndex.has_value() && node.skinIndex.value() < skinJointRemaps.size())
            remaps = &skinJointRemaps[node.skinIndex.value()];

        const auto& mesh = asset.meshes[node.meshIndex.value()];
        for (const auto& primitive : mesh.primitives)
        {
            if (primitive.type != fastgltf::PrimitiveType::Triangles)
                continue;

            auto posIt = primitive.findAttribute("POSITION");
            auto normalIt = primitive.findAttribute("NORMAL");
            auto uvIt = primitive.findAttribute("TEXCOORD_0");
            auto jointsIt = primitive.findAttribute("JOINTS_0");
            auto weightsIt = primitive.findAttribute("WEIGHTS_0");
            if (posIt == primitive.attributes.end() || !primitive.indicesAccessor.has_value())
                continue;

            const auto& positionAccessor = asset.accessors[posIt->accessorIndex];
            const size_t vertexCount = positionAccessor.count;
            std::vector<SourceVertex> sourceVertices(vertexCount);
            fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
                asset, positionAccessor, [&](fastgltf::math::fvec3 value, size_t index)
                {
                    sourceVertices[index].position[0] = value.x();
                    sourceVertices[index].position[1] = value.y();
                    sourceVertices[index].position[2] = value.z();
                });

            if (normalIt != primitive.attributes.end())
            {
                const auto& accessor = asset.accessors[normalIt->accessorIndex];
                fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
                    asset, accessor, [&](fastgltf::math::fvec3 value, size_t index)
                    {
                        sourceVertices[index].normal[0] = value.x();
                        sourceVertices[index].normal[1] = value.y();
                        sourceVertices[index].normal[2] = value.z();
                    });
            }
            if (uvIt != primitive.attributes.end())
            {
                const auto& accessor = asset.accessors[uvIt->accessorIndex];
                fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec2>(
                    asset, accessor, [&](fastgltf::math::fvec2 value, size_t index)
                    {
                        sourceVertices[index].uv[0] = value.x();
                        sourceVertices[index].uv[1] = value.y();
                    });
            }
            if (jointsIt != primitive.attributes.end() && remaps)
            {
                const auto& accessor = asset.accessors[jointsIt->accessorIndex];
                if (accessor.componentType == fastgltf::ComponentType::UnsignedByte)
                {
                    fastgltf::iterateAccessorWithIndex<fastgltf::math::u8vec4>(
                        asset, accessor, [&](fastgltf::math::u8vec4 value, size_t index)
                        {
                            for (int i = 0; i < 4; ++i)
                            {
                                const uint32_t local = value[i];
                                sourceVertices[index].boneIndices[i] =
                                    local < remaps->size() ? static_cast<uint8_t>((*remaps)[local]) : 255u;
                            }
                        });
                }
                else
                {
                    fastgltf::iterateAccessorWithIndex<fastgltf::math::u16vec4>(
                        asset, accessor, [&](fastgltf::math::u16vec4 value, size_t index)
                        {
                            for (int i = 0; i < 4; ++i)
                            {
                                const uint32_t local = value[i];
                                sourceVertices[index].boneIndices[i] =
                                    local < remaps->size() ? static_cast<uint8_t>((*remaps)[local]) : 255u;
                            }
                        });
                }
            }
            if (weightsIt != primitive.attributes.end())
            {
                const auto& accessor = asset.accessors[weightsIt->accessorIndex];
                fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec4>(
                    asset, accessor, [&](fastgltf::math::fvec4 value, size_t index)
                    {
                        sourceVertices[index].boneWeights[0] = ToWeightByte(value.x());
                        sourceVertices[index].boneWeights[1] = ToWeightByte(value.y());
                        sourceVertices[index].boneWeights[2] = ToWeightByte(value.z());
                        sourceVertices[index].boneWeights[3] = ToWeightByte(value.w());
                    });
            }

            const uint32_t baseVertex = static_cast<uint32_t>(m_vertices.size());
            m_vertices.resize(m_vertices.size() + sourceVertices.size());
            RawMesh rawMesh{};
            rawMesh.meshIndex = primitiveIndex++;
            rawMesh.baseVertex = baseVertex;
            rawMesh.vertexCount = static_cast<uint32_t>(sourceVertices.size());
            rawMesh.sourceVertices = std::move(sourceVertices);

            m_restVerticesGpu.reserve(m_restVerticesGpu.size() + rawMesh.sourceVertices.size());
            for (const SourceVertex& source : rawMesh.sourceVertices)
            {
                RestVertexGpu gpu{};
                gpu.position[0] = source.position[0];
                gpu.position[1] = source.position[1];
                gpu.position[2] = source.position[2];
                gpu.normal[0] = source.normal[0];
                gpu.normal[1] = source.normal[1];
                gpu.normal[2] = source.normal[2];
                gpu.uv[0] = source.uv[0];
                gpu.uv[1] = source.uv[1];
                gpu.packedWeights = PackBytes(
                    source.boneWeights[0], source.boneWeights[1],
                    source.boneWeights[2], source.boneWeights[3]);
                gpu.packedBones = PackBytes(
                    source.boneIndices[0], source.boneIndices[1],
                    source.boneIndices[2], source.boneIndices[3]);
                for (uint32_t i = 0; i < 4; ++i)
                {
                    if (source.boneWeights[i] != 0 && source.boneIndices[i] >= m_boneCount)
                        ++invalidRemappedInfluences;
                }
                m_restVerticesGpu.push_back(gpu);
            }

            const uint32_t firstIndex = static_cast<uint32_t>(m_indices.size());
            const auto& indexAccessor = asset.accessors[primitive.indicesAccessor.value()];
            observedIndexBytes = std::max(observedIndexBytes,
                static_cast<int>(fastgltf::getComponentByteSize(indexAccessor.componentType)));
            fastgltf::iterateAccessor<std::uint32_t>(
                asset, indexAccessor, [&](std::uint32_t index)
                {
                    m_indices.push_back(baseVertex + index);
                });

            MeshDraw draw{};
            draw.firstIndex = firstIndex;
            draw.indexCount = static_cast<uint32_t>(m_indices.size() - firstIndex);
            draw.textureIndex = 0;
            m_draws.push_back(draw);
            LogFormat("[GLTF] extracted primitive[%u] mesh='%s': verts=%zu indices=%u skin=%s",
                rawMesh.meshIndex,
                mesh.name.c_str(),
                rawMesh.sourceVertices.size(),
                draw.indexCount,
                remaps ? "yes" : "no");
            m_rawMeshes.push_back(std::move(rawMesh));
        }
    }

    if (m_vertices.empty() || m_indices.empty())
    {
        Log("[GLTF] no renderable mesh data extracted");
        DestroyAnimation();
        return false;
    }

    if (!SkinPose(0.0f, true, true))
        return false;

    m_indexCount = static_cast<uint32_t>(m_indices.size());
    LogFormat("[MESH] total verts=%zu indices=%zu", m_vertices.size(), m_indices.size());
    LogFormat("[MESH] skinned bbox min=(%.3f, %.3f, %.3f) max=(%.3f, %.3f, %.3f)",
        m_bounds.min[0], m_bounds.min[1], m_bounds.min[2],
        m_bounds.max[0], m_bounds.max[1], m_bounds.max[2]);
    LogFormat("[MESH] skinned bbox center=(%.3f, %.3f, %.3f) displayFitScale=%.3f",
        m_bounds.center[0], m_bounds.center[1], m_bounds.center[2], m_bounds.fitScale);
    LogFormat("[MESH] index size=%d bit, output index buffer=32 bit", observedIndexBytes * 8);
    LogFormat("[COMPUTE] rest-mesh SSBO vertices=%zu stride=%zu invalidRemapSentinels=%u",
        m_restVerticesGpu.size(),
        sizeof(RestVertexGpu),
        invalidRemappedInfluences);
    return true;
}

bool SkinnedMeshRenderer::LoadFbxMesh(const std::string& modelPath)
{
    DestroyAnimation();
    m_importedDiffuseTexturePath.clear();
    if (!m_assets)
        return false;

    std::filesystem::path fbxPath(modelPath);
    if (fbxPath.is_relative())
    {
        if (auto root = m_assets->RootPath())
            fbxPath = *root / fbxPath;
        else
            fbxPath = std::filesystem::absolute(fbxPath);
    }
    std::error_code ec;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(fbxPath, ec);
    if (!ec)
        fbxPath = canonical;

    AssimpImporter importer;
    const AssimpImporter::ImportResult result = importer.importFile(fbxPath);
    if (!result.success)
    {
        LogFormat("[FBX-IMPORT] skinned load failed path=%s error=%s",
            fbxPath.generic_string().c_str(),
            result.errorMessage.c_str());
        return false;
    }
    if (!result.skeleton || result.skeleton->bones.empty())
    {
        LogFormat("[FBX-IMPORT] skinned load skipped path=%s reason=no_skeleton",
            fbxPath.generic_string().c_str());
        return false;
    }

    const std::filesystem::path stemSkeleton = fbxPath.parent_path() /
        (fbxPath.stem().string() + "_skeleton.ozz");
    std::vector<std::filesystem::path> animationPaths;
    for (std::size_t i = 0; i < result.animations.size(); ++i)
        animationPaths.push_back(fbxPath.parent_path() / (fbxPath.stem().string() + "_anim_" + std::to_string(i) + ".ozz"));
    if (!std::filesystem::exists(stemSkeleton))
    {
        std::string ozzError;
        if (!importer.writeOzzSidecars(result, stemSkeleton, animationPaths, ozzError))
        {
            LogFormat("[FBX-IMPORT] skinned sidecar generation failed path=%s error=%s",
                fbxPath.generic_string().c_str(),
                ozzError.c_str());
            return false;
        }
    }

    if (!LoadOzzPose(modelPath))
        return false;

    m_inverseBindMatrices.assign(m_boneCount, IdentityPaletteMatrix());
    for (std::size_t i = 0; i < result.skeleton->bones.size() && i < m_inverseBindMatrices.size(); ++i)
        m_inverseBindMatrices[i] = result.skeleton->bones[i].inverseBindPose;

    m_vertices.clear();
    m_indices.clear();
    m_rawMeshes.clear();
    m_restVerticesGpu.clear();
    m_draws.clear();
    for (const GltfMaterialSource& material : result.materials)
    {
        if (!material.baseColorTexturePath.empty())
        {
            m_importedDiffuseTexturePath = material.baseColorTexturePath;
            break;
        }
    }

    std::unordered_map<int, const AssimpImporter::SkinningData*> skinByMesh;
    for (const AssimpImporter::SkinningData& skin : result.skinning)
        skinByMesh[skin.meshIndex] = &skin;

    uint32_t invalidInfluences = 0;
    uint32_t meshIndex = 0;
    for (std::size_t sourceMeshIndex = 0; sourceMeshIndex < result.meshes.size(); ++sourceMeshIndex)
    {
        const AssimpImporter::StaticMeshData& mesh = result.meshes[sourceMeshIndex];
        if (mesh.vertices.empty() || mesh.indices.empty())
            continue;
        const uint32_t baseVertex = static_cast<uint32_t>(m_vertices.size());
        m_vertices.resize(m_vertices.size() + mesh.vertices.size());

        RawMesh rawMesh{};
        rawMesh.meshIndex = meshIndex++;
        rawMesh.baseVertex = baseVertex;
        rawMesh.vertexCount = static_cast<uint32_t>(mesh.vertices.size());
        rawMesh.sourceVertices.resize(mesh.vertices.size());

        const AssimpImporter::SkinningData* skin = nullptr;
        const auto skinIt = skinByMesh.find(static_cast<int>(sourceMeshIndex));
        if (skinIt != skinByMesh.end())
            skin = skinIt->second;

        for (std::size_t vertexIndex = 0; vertexIndex < mesh.vertices.size(); ++vertexIndex)
        {
            const AssimpImporter::Vertex& source = mesh.vertices[vertexIndex];
            SourceVertex& vertex = rawMesh.sourceVertices[vertexIndex];
            vertex.position[0] = source.position[0];
            vertex.position[1] = source.position[1];
            vertex.position[2] = source.position[2];
            vertex.normal[0] = source.normal[0];
            vertex.normal[1] = source.normal[1];
            vertex.normal[2] = source.normal[2];
            vertex.uv[0] = source.uv[0];
            vertex.uv[1] = source.uv[1];
            if (skin && vertexIndex < skin->influences.size())
            {
                std::memcpy(vertex.boneIndices, skin->influences[vertexIndex].boneIndices, sizeof(vertex.boneIndices));
                std::memcpy(vertex.boneWeights, skin->influences[vertexIndex].boneWeights, sizeof(vertex.boneWeights));
            }
            else
            {
                vertex.boneIndices[0] = 0;
                vertex.boneWeights[0] = 255;
            }

            RestVertexGpu gpu{};
            gpu.position[0] = vertex.position[0];
            gpu.position[1] = vertex.position[1];
            gpu.position[2] = vertex.position[2];
            gpu.normal[0] = vertex.normal[0];
            gpu.normal[1] = vertex.normal[1];
            gpu.normal[2] = vertex.normal[2];
            gpu.uv[0] = vertex.uv[0];
            gpu.uv[1] = vertex.uv[1];
            gpu.packedWeights = PackBytes(vertex.boneWeights[0], vertex.boneWeights[1], vertex.boneWeights[2], vertex.boneWeights[3]);
            gpu.packedBones = PackBytes(vertex.boneIndices[0], vertex.boneIndices[1], vertex.boneIndices[2], vertex.boneIndices[3]);
            for (uint32_t influence = 0; influence < 4; ++influence)
            {
                if (vertex.boneWeights[influence] != 0 && vertex.boneIndices[influence] >= m_boneCount)
                    ++invalidInfluences;
            }
            m_restVerticesGpu.push_back(gpu);
        }

        const uint32_t firstIndex = static_cast<uint32_t>(m_indices.size());
        for (uint32_t index : mesh.indices)
            m_indices.push_back(baseVertex + index);
        MeshDraw draw{};
        draw.firstIndex = firstIndex;
        draw.indexCount = static_cast<uint32_t>(m_indices.size() - firstIndex);
        draw.textureIndex = 0;
        m_draws.push_back(draw);
        m_rawMeshes.push_back(std::move(rawMesh));
        LogFormat("[FBX-IMPORT] extracted skinned mesh[%zu] name=%s verts=%zu indices=%u skin=%s",
            sourceMeshIndex,
            mesh.name.c_str(),
            mesh.vertices.size(),
            draw.indexCount,
            skin ? "yes" : "fallback-root");
    }

    if (m_vertices.empty() || m_indices.empty())
    {
        Log("[FBX-IMPORT] no renderable skinned FBX mesh data extracted");
        DestroyAnimation();
        return false;
    }

    if (!SkinPose(0.0f, true, true))
        return false;

    m_indexCount = static_cast<uint32_t>(m_indices.size());
    LogFormat("[FBX-IMPORT] skinned loaded path=%s verts=%zu indices=%zu bones=%u animations=%zu invalidInfluences=%u",
        fbxPath.generic_string().c_str(),
        m_vertices.size(),
        m_indices.size(),
        m_boneCount,
        result.animations.size(),
        invalidInfluences);
    return true;
}

bool SkinnedMeshRenderer::LoadOzzPose(const std::string& modelPath)
{
    if (!m_assets)
        return false;

    m_ozz = std::make_unique<OzzRuntime>();
    const std::string dir = DirectoryOf(modelPath);
    const std::string stem = std::filesystem::path(modelPath).stem().string();
    const std::string stemSkeletonPath = dir + "/" + stem + "_skeleton.ozz";
    const std::string legacySkeletonPath = dir + "/skeleton.ozz";
    const std::string skeletonPath = m_assets->ReadAll(stemSkeletonPath).has_value()
        ? stemSkeletonPath
        : legacySkeletonPath;
    if (!ReadOzzObject(*m_assets, skeletonPath, m_ozz->skeleton))
    {
        DestroyAnimation();
        return false;
    }

    m_boneCount = static_cast<uint32_t>(m_ozz->skeleton.num_joints());
    m_inverseBindMatrices.assign(m_boneCount, IdentityPaletteMatrix());
    m_bonePaletteCpu.assign(m_boneCount, IdentityPaletteMatrix());
    m_ozz->locals.resize(static_cast<size_t>(m_ozz->skeleton.num_soa_joints()));
    m_ozz->models.resize(static_cast<size_t>(m_ozz->skeleton.num_joints()));
    m_ozz->context.Resize(m_ozz->skeleton.num_joints());

    const std::string stemIdlePath = dir + "/" + stem + "_anim_0.ozz";
    const std::string legacyIdlePath = dir + "/idle.ozz";
    const std::string idlePath = m_assets->ReadAll(stemIdlePath).has_value()
        ? stemIdlePath
        : legacyIdlePath;
    if (ReadOzzObject(*m_assets, idlePath, m_ozz->idle) &&
        m_ozz->idle.num_tracks() == m_ozz->skeleton.num_joints())
    {
        m_ozz->hasIdle = true;
        LogFormat("[OZZ] loaded skeleton=%s bones=%u idle=%s duration=%.3f",
            skeletonPath.c_str(), m_boneCount, idlePath.c_str(), m_ozz->idle.duration());
    }
    else
    {
        m_ozz->hasIdle = false;
        LogFormat("[OZZ] loaded skeleton=%s bones=%u; idle.ozz missing or incompatible, using rest pose",
            skeletonPath.c_str(), m_boneCount);
    }
    return true;
}

void SkinnedMeshRenderer::SetMotionState(MotionState state)
{
    const bool changed = m_motionState != state;
    m_motionState = state;
    if (changed)
        LogFormat("[ANIM-PREVIEW] state=%s", MotionStateName(state));
}

float SkinnedMeshRenderer::GroundOffsetY() const
{
    return -m_bounds.min[1];
}

bool SkinnedMeshRenderer::SkinPose(float animTimeSeconds, bool updateBounds, bool logSamples)
{
    if (!m_ozz || m_boneCount == 0)
        return false;

    if (m_ozz->hasIdle)
    {
        const float duration = m_ozz->idle.duration();
        const float ratio = duration > 0.0f
            ? std::fmod(std::max(animTimeSeconds, 0.0f), duration) / duration
            : 0.0f;
        ozz::animation::SamplingJob samplingJob;
        samplingJob.animation = &m_ozz->idle;
        samplingJob.context = &m_ozz->context;
        samplingJob.ratio = ratio;
        samplingJob.output = ozz::make_span(m_ozz->locals);
        if (!samplingJob.Run())
            return false;
    }
    else
    {
        const auto rest = m_ozz->skeleton.joint_rest_poses();
        std::copy(rest.begin(), rest.end(), m_ozz->locals.begin());
    }

    ozz::animation::LocalToModelJob localToModel;
    localToModel.skeleton = &m_ozz->skeleton;
    localToModel.input = ozz::make_span(m_ozz->locals);
    localToModel.output = ozz::make_span(m_ozz->models);
    if (!localToModel.Run())
        return false;

    for (uint32_t bone = 0; bone < m_boneCount; ++bone)
    {
        ozz::math::Float4x4 inverseBind = ozz::math::Float4x4::identity();
        for (int col = 0; col < 4; ++col)
        {
            inverseBind.cols[col] = ozz::math::simd_float4::Load(
                m_inverseBindMatrices[bone].m[0 * 4 + col],
                m_inverseBindMatrices[bone].m[1 * 4 + col],
                m_inverseBindMatrices[bone].m[2 * 4 + col],
                m_inverseBindMatrices[bone].m[3 * 4 + col]);
        }
        m_bonePaletteCpu[bone] = ToRowVectorPaletteMatrix(m_ozz->models[bone] * inverseBind);
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
        for (uint32_t vertexIndex = 0; vertexIndex < rawMesh.vertexCount; ++vertexIndex)
        {
            const SourceVertex& source = rawMesh.sourceVertices[vertexIndex];
            Vertex out{};
            int modelBones[4]{};
            SkinVertex(source, m_bonePaletteCpu, out.position, out.normal, out.uv, modelBones);

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
            LogFormat("[SKIN] mesh[%u]: skinned %u verts, bones=%u",
                rawMesh.meshIndex,
                rawMesh.vertexCount,
                m_boneCount);
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
        const float maxDimension = std::max(sizeX, std::max(sizeY, sizeZ));
        m_bounds.fitScale = maxDimension > 0.0001f ? (2.35f / maxDimension) : 1.0f;
    }

    return true;
}

bool SkinnedMeshRenderer::UploadBonePalette(float animTimeSeconds, uint32_t frameIndex)
{
    return UploadBonePalette(m_motionState, animTimeSeconds, frameIndex, 0);
}

bool SkinnedMeshRenderer::UploadBonePalette(MotionState state, float animTimeSeconds, uint32_t frameIndex, uint32_t skinSlot)
{
    if (frameIndex >= kFramesInFlight || skinSlot >= kSkinSlots || !m_bonePaletteBuffers[frameIndex][skinSlot].memory ||
        !m_ozz || m_bonePaletteCpu.empty())
    {
        return false;
    }

    (void)state;
    if (!SkinPose(animTimeSeconds, false, false))
        return false;

    const VkDeviceSize size = sizeof(Mat4) * m_bonePaletteCpu.size();
    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(m_device, m_bonePaletteBuffers[frameIndex][skinSlot].memory, 0, size, 0, &mapped));
    std::memcpy(mapped, m_bonePaletteCpu.data(), static_cast<size_t>(size));
    vkUnmapMemory(m_device, m_bonePaletteBuffers[frameIndex][skinSlot].memory);
    return true;
}

void SkinnedMeshRenderer::DispatchSkin(VkCommandBuffer cmd, uint32_t frameIndex)
{
    DispatchSkin(cmd, frameIndex, 0);
}

void SkinnedMeshRenderer::DispatchSkin(VkCommandBuffer cmd, uint32_t frameIndex, uint32_t skinSlot)
{
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_computePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_computePipelineLayout,
        0, 1, &m_computeDescriptorSets[frameIndex][skinSlot], 0, nullptr);

    SkinPushConstants push{};
    push.vertexCount = static_cast<uint32_t>(m_vertices.size());
    push.boneCount = m_boneCount;
    vkCmdPushConstants(cmd, m_computePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
        0, sizeof(push), &push);

    const uint32_t groupCount = (push.vertexCount + 63u) / 64u;
    vkCmdDispatch(cmd, groupCount, 1, 1);
}

bool SkinnedMeshRenderer::VerifyComputeSkin(VulkanDevice& device)
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

bool SkinnedMeshRenderer::CreateBuffers(VulkanDevice& device)
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

bool SkinnedMeshRenderer::CreateComputeResources(VulkanDevice& device)
{
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    vkGetDeviceQueue(m_device, device.GetGraphicsQueueFamily(), 0, &graphicsQueue);

    const VkDeviceSize restSize = sizeof(RestVertexGpu) * m_restVerticesGpu.size();
    const VkDeviceSize vertexSize = sizeof(Vertex) * m_vertices.size();
    const VkDeviceSize paletteSize = sizeof(Mat4) * static_cast<size_t>(m_boneCount);
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

bool SkinnedMeshRenderer::CreateTextures(VulkanDevice& device, const std::string& modelPath)
{
    const std::array<std::string, kTextureCount> textureFiles = {
        modelPath + "#baseColor",
        modelPath + "#fallback"};

    VkQueue graphicsQueue = VK_NULL_HANDLE;
    vkGetDeviceQueue(m_device, device.GetGraphicsQueueFamily(), 0, &graphicsQueue);

    for (uint32_t textureIndex = 0; textureIndex < kTextureCount; ++textureIndex)
    {
        DdsImage dds{};
        bool loaded = false;
        if (textureIndex == 0)
        {
            if (!m_importedDiffuseTexturePath.empty())
                loaded = LoadRgbaTextureFile(m_importedDiffuseTexturePath, dds);
            if (!loaded && m_assets)
                loaded = LoadGltfBaseColorTexture(*m_assets, modelPath, dds);
        }
        if (!loaded)
        {
            LogFormat("[DDS] Falling back to 4x4 white RGBA8888 texture for %s",
                textureFiles[textureIndex].c_str());
            dds = CreateFallbackWhiteDdsImage(textureFiles[textureIndex]);
        }

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
            LogFormat("[DDS] Falling back to 4x4 white RGBA8888 texture for %s",
                dds.filename.c_str());
            dds = CreateFallbackWhiteDdsImage(dds.filename);
            vkGetPhysicalDeviceFormatProperties(device.GetPhysicalDevice(), dds.format, &props);
            if ((props.optimalTilingFeatures & required) != required)
            {
                LogFormat("[DDS] fallback texture format unsupported for %s format=%s features=0x%08x",
                    dds.filename.c_str(),
                    VkFormatName(dds.format),
                    props.optimalTilingFeatures);
                return false;
            }
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
        sampler.mipLodBias = -0.25f;
        if (device.SupportsSamplerAnisotropy())
        {
            sampler.anisotropyEnable = VK_TRUE;
            sampler.maxAnisotropy = device.GetMaxSamplerAnisotropy();
        }
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

bool SkinnedMeshRenderer::CreateDescriptors()
{
    VkDescriptorSetLayoutBinding ubo{};
    ubo.binding = 0;
    ubo.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    ubo.descriptorCount = 1;
    ubo.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

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

bool SkinnedMeshRenderer::CreateComputeDescriptors()
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
            bonesInfo.range = sizeof(Mat4) * static_cast<size_t>(m_boneCount);

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

bool SkinnedMeshRenderer::CreateComputePipeline()
{
    if (!m_assets)
        return false;

    VkShaderModule cs = CreateShaderModule(m_device, *m_assets, "assets/shaders/skinned_mesh_cs.spv");

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

bool SkinnedMeshRenderer::CreatePipeline(VulkanDevice& device)
{
    if (!m_assets)
        return false;

    VkShaderModule vs = CreateShaderModule(m_device, *m_assets, "assets/shaders/skinned_mesh_vs.spv");
    VkShaderModule ps = CreateShaderModule(m_device, *m_assets, "assets/shaders/skinned_mesh_ps.spv");

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
    pipeline.renderPass = m_mainRenderPass ? m_mainRenderPass : device.GetRenderPass();
    pipeline.subpass = 0;
    VK_CHECK(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &m_pipeline));

    vkDestroyShaderModule(m_device, ps, nullptr);
    vkDestroyShaderModule(m_device, vs, nullptr);
    return true;
}

bool SkinnedMeshRenderer::CreateReflectionPipeline(VulkanDevice& device, VkRenderPass renderPass)
{
    if (!m_assets || !m_pipelineLayout || renderPass == VK_NULL_HANDLE)
        return false;

    VkShaderModule vs = CreateShaderModule(m_device, *m_assets, "assets/shaders/skinned_mesh_vs.spv");
    VkShaderModule ps = CreateShaderModule(m_device, *m_assets, "assets/shaders/skinned_mesh_ps.spv");

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
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(std::size(attributes));
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
    raster.cullMode = VK_CULL_MODE_FRONT_BIT;
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
    pipeline.renderPass = renderPass;
    pipeline.subpass = 0;
    VK_CHECK(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &m_reflectionPipeline));
    m_reflectionRenderPass = renderPass;

    vkDestroyShaderModule(m_device, ps, nullptr);
    vkDestroyShaderModule(m_device, vs, nullptr);
    Log("[WATER-3] Skinned mesh reflection pipeline created");
    return true;
}

void SkinnedMeshRenderer::DestroyPipeline()
{
    DestroyReflectionPipeline();

    if (m_pipeline)
        vkDestroyPipeline(m_device, m_pipeline, nullptr);
    m_pipeline = VK_NULL_HANDLE;

    if (m_pipelineLayout)
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    m_pipelineLayout = VK_NULL_HANDLE;
}

void SkinnedMeshRenderer::DestroyReflectionPipeline()
{
    if (m_reflectionPipeline)
        vkDestroyPipeline(m_device, m_reflectionPipeline, nullptr);
    m_reflectionPipeline = VK_NULL_HANDLE;
    m_reflectionRenderPass = VK_NULL_HANDLE;
}

void SkinnedMeshRenderer::DestroyBuffer(Buffer& buffer)
{
    if (buffer.buffer)
        vkDestroyBuffer(m_device, buffer.buffer, nullptr);
    if (buffer.memory)
        vkFreeMemory(m_device, buffer.memory, nullptr);
    buffer = {};
}

void SkinnedMeshRenderer::DestroyComputeResources()
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

void SkinnedMeshRenderer::DestroyAnimation()
{
    m_ozz.reset();
    m_inverseBindMatrices.clear();
    m_bonePaletteCpu.clear();
    m_importedDiffuseTexturePath.clear();
    m_boneCount = 0;
    m_motionState = MotionState::Idle;
    m_lastAnimationLogTime = -1000.0;
}

void SkinnedMeshRenderer::DestroyTexture(Texture& texture)
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

void SkinnedMeshRenderer::UpdateUniform(uint32_t frameIndex, uint32_t uniformSlot, double timeSeconds, float aspect)
{
    static bool loggedMvp = false;

    const Mat4 center = Translation(-m_bounds.center[0], -m_bounds.center[1], -m_bounds.center[2]);
    const Mat4 display = Identity();
    const Mat4 fit = Scale(m_bounds.fitScale);
    const Mat4 spin = RotationY(static_cast<float>(timeSeconds) * 0.55f);
    const Mat4 place = Translation(0.0f, 0.0f, 4.0f);
    const Mat4 model = Multiply(Multiply(Multiply(Multiply(center, display), fit), spin), place);
    const Mat4 projection = Perspective(xm::DegreesToRadians(45.0f), aspect, 0.1f, 50.0f);
    const Mat4 mvp = Multiply(model, projection);

    if (!loggedMvp)
    {
        LogFormat("[MESH] MVP: ozz/glTF mesh-local render transform, fitScale=%.3f, translateZ=4.0, fov=45, near=0.1 far=50, aspect=%.3f",
            m_bounds.fitScale,
            aspect);
        loggedMvp = true;
    }

    UniformBlock uniform{mvp, model, {1.0f, 1.0f, 1.0f, 1.0f}};
    FillLightingUniform(m_lightingState, uniform);
    WaterConfig noWater{};
    noWater.enabled = false;
    noWater.foamEnabled = false;
    noWater.causticMode = WaterConfig::CausticMode::Off;
    FillWaterUniform(noWater, timeSeconds, uniform);

    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(m_device, m_uniformBuffers[frameIndex][uniformSlot].memory, 0, sizeof(uniform), 0, &mapped));
    std::memcpy(mapped, &uniform, sizeof(uniform));
    vkUnmapMemory(m_device, m_uniformBuffers[frameIndex][uniformSlot].memory);
}

void SkinnedMeshRenderer::UpdateWorldUniform(uint32_t frameIndex,
    uint32_t uniformSlot,
    const WorldCamera& camera,
    WorldVec3 position,
    float yawRadians,
    double timeSeconds,
    std::array<float, 4> tint,
    bool reflectionPass,
    float waterLevelY)
{
    static bool loggedMvp = false;

    const Mat4 model = Multiply(RotationY(-yawRadians),
        Translation(position.x, position.y, position.z));
    const Mat4 viewProjection = ToLocalMat4(camera.viewProjection);
    const Mat4 mvp = Multiply(model, viewProjection);

    if (!loggedMvp)
    {
        Log("[MESH] World MVP: ozz/glTF mesh-local render transform, spawn translated to local origin, shared full-screen camera");
        loggedMvp = true;
    }

    UniformBlock uniform{mvp, model, {tint[0], tint[1], tint[2], tint[3]}};
    if (reflectionPass)
    {
        LightingState reflectionLighting = m_lightingState;
        reflectionLighting.numPointLights = 0;
        reflectionLighting.numSpotLights = 0;
        FillLightingUniform(reflectionLighting, uniform);
        WaterConfig noWater{};
        noWater.enabled = false;
        noWater.foamEnabled = false;
        noWater.causticMode = WaterConfig::CausticMode::Off;
        FillWaterUniform(noWater, timeSeconds, uniform);
        uniform.lightPadding[0] = 1.0f;
        uniform.lightPadding[1] = waterLevelY;
    }
    else
    {
        FillLightingUniform(m_lightingState, uniform);
        WaterConfig noWater{};
        noWater.enabled = false;
        noWater.foamEnabled = false;
        noWater.causticMode = WaterConfig::CausticMode::Off;
        FillWaterUniform(noWater, timeSeconds, uniform);
    }

    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(m_device, m_uniformBuffers[frameIndex][uniformSlot].memory, 0, sizeof(uniform), 0, &mapped));
    std::memcpy(mapped, &uniform, sizeof(uniform));
    vkUnmapMemory(m_device, m_uniformBuffers[frameIndex][uniformSlot].memory);
}
