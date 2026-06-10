#include "TerrainRenderer.h"

#include "Debug.h"
#include "WaterBodyIO.h"
#include "asset/IAssetReader.h"
#include "map/MapData.h"

#include <stb_image.h>

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
#include <optional>
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
constexpr uint32_t kMaxWaterBodyDraws = 64;

bool HasStencilAspect(VkFormat format)
{
    return format == VK_FORMAT_D24_UNORM_S8_UINT ||
           format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
           format == VK_FORMAT_S8_UINT;
}

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

float ApplyWaterEdgeCurve(float t, WaterConfig::EdgeFadeCurve curve)
{
    t = std::clamp(t, 0.0f, 1.0f);
    switch (curve)
    {
    case WaterConfig::EdgeFadeCurve::Linear:
        return t;
    case WaterConfig::EdgeFadeCurve::Exponential:
        return 1.0f - std::exp(-3.0f * t);
    case WaterConfig::EdgeFadeCurve::Smooth:
    default:
        return t * t * (3.0f - 2.0f * t);
    }
}

void ComputeWaterBodyDistanceField(const WaterBody& body,
                                   float cellX,
                                   float cellZ,
                                   float maxDistanceMeters,
                                   std::vector<float>& outDistanceField)
{
    const std::uint32_t width = body.maskWidth;
    const std::uint32_t height = body.maskHeight;
    outDistanceField.assign(static_cast<std::size_t>(width) * height, 0.0f);
    const float searchCell = std::max(0.001f, std::min(cellX, cellZ));
    const int searchRadius = std::max(1, static_cast<int>(std::ceil(maxDistanceMeters / searchCell)) + 1);
    std::uint32_t waterCells = 0;
    std::uint32_t terrainCells = 0;
    float maxFoundDistance = 0.0f;

    for (std::uint32_t y = 0; y < height; ++y)
    {
        for (std::uint32_t x = 0; x < width; ++x)
        {
            const std::size_t index = static_cast<std::size_t>(y) * width + x;
            const bool isWater = body.shapeMask[index] != 0;
            waterCells += isWater ? 1u : 0u;
            terrainCells += isWater ? 0u : 1u;
            float minDistSq = std::numeric_limits<float>::max();

            if (isWater)
            {
                const float edgeX = std::min(static_cast<float>(x) + 0.5f,
                    static_cast<float>(width) - (static_cast<float>(x) + 0.5f)) * cellX;
                const float edgeZ = std::min(static_cast<float>(y) + 0.5f,
                    static_cast<float>(height) - (static_cast<float>(y) + 0.5f)) * cellZ;
                const float outsideDistance = std::min(edgeX, edgeZ);
                minDistSq = outsideDistance * outsideDistance;
            }

            for (int dy = -searchRadius; dy <= searchRadius; ++dy)
            {
                const int ny = static_cast<int>(y) + dy;
                if (ny < 0 || ny >= static_cast<int>(height))
                    continue;
                for (int dx = -searchRadius; dx <= searchRadius; ++dx)
                {
                    const int nx = static_cast<int>(x) + dx;
                    if (nx < 0 || nx >= static_cast<int>(width))
                        continue;
                    const bool neighborWater =
                        body.shapeMask[static_cast<std::size_t>(ny) * width + static_cast<std::size_t>(nx)] != 0;
                    if (neighborWater == isWater)
                        continue;
                    const float distX = static_cast<float>(dx) * cellX;
                    const float distZ = static_cast<float>(dy) * cellZ;
                    minDistSq = std::min(minDistSq, distX * distX + distZ * distZ);
                }
            }

            if (!std::isfinite(minDistSq) || minDistSq == std::numeric_limits<float>::max())
                minDistSq = maxDistanceMeters * maxDistanceMeters;
            const float distanceMeters = std::sqrt(minDistSq);
            maxFoundDistance = std::max(maxFoundDistance, distanceMeters);
            outDistanceField[index] = isWater ? distanceMeters : -distanceMeters;
        }
    }

    Tracenf("[WATER-OBJ-6] Distance field computed: body_id=%u water_cells=%u terrain_cells=%u max_dist=%.2fm",
        body.id, waterCells, terrainCells, maxFoundDistance);
}

float BilinearSampleWaterDistance(const std::vector<float>& distanceField,
                                  std::uint32_t width,
                                  std::uint32_t height,
                                  float u,
                                  float v)
{
    if (distanceField.empty() || width == 0 || height == 0)
        return 0.0f;
    const float sx = std::clamp(u * static_cast<float>(width) - 0.5f, 0.0f, static_cast<float>(width - 1u));
    const float sy = std::clamp(v * static_cast<float>(height) - 0.5f, 0.0f, static_cast<float>(height - 1u));
    const std::uint32_t x0 = static_cast<std::uint32_t>(std::floor(sx));
    const std::uint32_t y0 = static_cast<std::uint32_t>(std::floor(sy));
    const std::uint32_t x1 = std::min(width - 1u, x0 + 1u);
    const std::uint32_t y1 = std::min(height - 1u, y0 + 1u);
    const float tx = sx - static_cast<float>(x0);
    const float ty = sy - static_cast<float>(y0);
    const float d00 = distanceField[static_cast<std::size_t>(y0) * width + x0];
    const float d10 = distanceField[static_cast<std::size_t>(y0) * width + x1];
    const float d01 = distanceField[static_cast<std::size_t>(y1) * width + x0];
    const float d11 = distanceField[static_cast<std::size_t>(y1) * width + x1];
    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    return lerp(lerp(d00, d10, tx), lerp(d01, d11, tx), ty);
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

template <typename UniformBlockT>
void FillDynamicLightingUniforms(const LightingState& lighting, UniformBlockT& uniform)
{
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
        out.direction[3] = std::cos(spot.innerConeDegrees * 3.1415926535f / 180.0f);
        const float intensity = spot.enabled ? std::max(0.0f, spot.intensity) : 0.0f;
        out.color[0] = std::max(0.0f, spot.r) * intensity;
        out.color[1] = std::max(0.0f, spot.g) * intensity;
        out.color[2] = std::max(0.0f, spot.b) * intensity;
        out.color[3] = std::cos(spot.outerConeDegrees * 3.1415926535f / 180.0f);
        out.direction[3] = std::max(out.direction[3], out.color[3]);
    }
}

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

std::string NormalizeTerrainAssetPath(std::string_view path)
{
    std::string normalized(path);
    for (char& c : normalized)
    {
        if (c == '\\')
            c = '/';
    }
    while (!normalized.empty() && normalized.front() == '/')
        normalized.erase(normalized.begin());
    return normalized;
}

std::optional<std::vector<std::uint8_t>> ReadTerrainAssetBytes(
    client::asset::IAssetReader& assets,
    const std::string& path,
    const std::vector<std::filesystem::path>* additionalRoots = nullptr)
{
    if (auto bytes = assets.ReadAll(path))
        return bytes;

    if (!additionalRoots)
        return std::nullopt;

    const std::filesystem::path relative = std::filesystem::path(NormalizeTerrainAssetPath(path));
    for (const std::filesystem::path& root : *additionalRoots)
    {
        if (root.empty())
            continue;
        const std::filesystem::path resolved = relative.is_absolute() ? relative : (root / relative);
        std::ifstream file(resolved, std::ios::binary | std::ios::ate);
        if (!file)
            continue;
        const auto end = file.tellg();
        if (end < 0)
            continue;
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
        file.seekg(0);
        if (!bytes.empty())
        {
            file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!file)
                continue;
        }
        Tracenf("[TERRAIN-PALETTE] loaded project asset: %s", resolved.generic_string().c_str());
        return bytes;
    }
    return std::nullopt;
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
    case VK_FORMAT_R8G8B8A8_UNORM: return "VK_FORMAT_R8G8B8A8_UNORM";
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

struct RgbaImage
{
    std::string filename;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels;
};

bool LoadDdsImage(client::asset::IAssetReader& assets,
    const std::string& path,
    DdsImage& out,
    const std::vector<std::filesystem::path>* additionalRoots = nullptr)
{
    auto maybeBytes = ReadTerrainAssetBytes(assets, path, additionalRoots);
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

void DecodeColor565(uint16_t value, uint8_t& r, uint8_t& g, uint8_t& b)
{
    r = static_cast<uint8_t>(((value >> 11) & 31u) * 255u / 31u);
    g = static_cast<uint8_t>(((value >> 5) & 63u) * 255u / 63u);
    b = static_cast<uint8_t>((value & 31u) * 255u / 31u);
}

bool DecodeDxtToRgba(const DdsImage& image, RgbaImage& out)
{
    if (!image.compressed || image.pixels.empty() || image.width == 0 || image.height == 0)
        return false;

    out = {};
    out.filename = image.filename;
    out.width = image.width;
    out.height = image.height;
    out.pixels.assign(static_cast<size_t>(out.width) * out.height * 4u, 255);

    const uint32_t blocksWide = std::max(1u, (image.width + 3u) / 4u);
    const uint32_t blocksHigh = std::max(1u, (image.height + 3u) / 4u);
    size_t offset = 0;
    for (uint32_t by = 0; by < blocksHigh; ++by)
    {
        for (uint32_t bx = 0; bx < blocksWide; ++bx)
        {
            if (offset + image.blockBytes > image.pixels.size())
                return false;

            const uint8_t* block = image.pixels.data() + offset;
            uint8_t alpha[16];
            std::fill(std::begin(alpha), std::end(alpha), 255);
            size_t colorOffset = 0;
            if (image.blockBytes == 16)
            {
                if (image.format == VK_FORMAT_BC2_SRGB_BLOCK)
                {
                    for (uint32_t i = 0; i < 16; ++i)
                    {
                        const uint8_t nibble = (block[i / 2] >> ((i % 2) * 4)) & 0x0f;
                        alpha[i] = static_cast<uint8_t>(nibble * 17u);
                    }
                }
                else
                {
                    const uint8_t a0 = block[0];
                    const uint8_t a1 = block[1];
                    uint8_t table[8] = {a0, a1, 0, 0, 0, 0, 0, 0};
                    if (a0 > a1)
                    {
                        for (uint32_t i = 2; i < 8; ++i)
                            table[i] = static_cast<uint8_t>(((8u - i) * a0 + (i - 1u) * a1) / 7u);
                    }
                    else
                    {
                        for (uint32_t i = 2; i < 6; ++i)
                            table[i] = static_cast<uint8_t>(((6u - i) * a0 + (i - 1u) * a1) / 5u);
                        table[6] = 0;
                        table[7] = 255;
                    }
                    uint64_t bits = 0;
                    for (uint32_t i = 0; i < 6; ++i)
                        bits |= static_cast<uint64_t>(block[2 + i]) << (8u * i);
                    for (uint32_t i = 0; i < 16; ++i)
                        alpha[i] = table[(bits >> (3u * i)) & 0x07u];
                }
                colorOffset = 8;
            }

            const uint16_t c0 = static_cast<uint16_t>(block[colorOffset] | (block[colorOffset + 1] << 8));
            const uint16_t c1 = static_cast<uint16_t>(block[colorOffset + 2] | (block[colorOffset + 3] << 8));
            uint8_t colors[4][4]{};
            DecodeColor565(c0, colors[0][0], colors[0][1], colors[0][2]);
            DecodeColor565(c1, colors[1][0], colors[1][1], colors[1][2]);
            colors[0][3] = colors[1][3] = 255;
            if (c0 > c1 || image.blockBytes == 16)
            {
                for (uint32_t c = 0; c < 3; ++c)
                {
                    colors[2][c] = static_cast<uint8_t>((2u * colors[0][c] + colors[1][c]) / 3u);
                    colors[3][c] = static_cast<uint8_t>((colors[0][c] + 2u * colors[1][c]) / 3u);
                }
                colors[2][3] = colors[3][3] = 255;
            }
            else
            {
                for (uint32_t c = 0; c < 3; ++c)
                    colors[2][c] = static_cast<uint8_t>((colors[0][c] + colors[1][c]) / 2u);
                colors[2][3] = 255;
                colors[3][3] = 0;
            }

            const uint32_t indices = static_cast<uint32_t>(block[colorOffset + 4]) |
                (static_cast<uint32_t>(block[colorOffset + 5]) << 8) |
                (static_cast<uint32_t>(block[colorOffset + 6]) << 16) |
                (static_cast<uint32_t>(block[colorOffset + 7]) << 24);
            for (uint32_t py = 0; py < 4; ++py)
            {
                for (uint32_t px = 0; px < 4; ++px)
                {
                    const uint32_t x = bx * 4u + px;
                    const uint32_t y = by * 4u + py;
                    if (x >= image.width || y >= image.height)
                        continue;
                    const uint32_t src = py * 4u + px;
                    const uint32_t colorIndex = (indices >> (2u * src)) & 0x03u;
                    const size_t dst = (static_cast<size_t>(y) * image.width + x) * 4u;
                    out.pixels[dst + 0] = colors[colorIndex][0];
                    out.pixels[dst + 1] = colors[colorIndex][1];
                    out.pixels[dst + 2] = colors[colorIndex][2];
                    out.pixels[dst + 3] = std::min(colors[colorIndex][3], alpha[src]);
                }
            }
            offset += image.blockBytes;
        }
    }
    return true;
}

bool DdsToRgba(const DdsImage& dds, RgbaImage& out)
{
    if (dds.compressed)
        return DecodeDxtToRgba(dds, out);
    if (dds.format != VK_FORMAT_R8G8B8A8_SRGB || dds.pixels.size() < static_cast<size_t>(dds.width) * dds.height * 4u)
        return false;
    out = {};
    out.filename = dds.filename;
    out.width = dds.width;
    out.height = dds.height;
    out.pixels.assign(dds.pixels.begin(), dds.pixels.begin() + static_cast<size_t>(dds.width) * dds.height * 4u);
    return true;
}

bool LoadStbImage(client::asset::IAssetReader& assets,
    const std::string& path,
    RgbaImage& out,
    const std::vector<std::filesystem::path>* additionalRoots = nullptr)
{
    auto bytes = ReadTerrainAssetBytes(assets, path, additionalRoots);
    if (!bytes)
        return false;
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* decoded = stbi_load_from_memory(bytes->data(), static_cast<int>(bytes->size()), &width, &height, &channels, 4);
    if (!decoded || width <= 0 || height <= 0)
    {
        if (decoded)
            stbi_image_free(decoded);
        return false;
    }
    out = {};
    out.filename = path.substr(path.find_last_of("\\/") + 1);
    out.width = static_cast<uint32_t>(width);
    out.height = static_cast<uint32_t>(height);
    out.pixels.assign(decoded, decoded + static_cast<size_t>(width) * height * 4u);
    stbi_image_free(decoded);
    return true;
}

bool LoadAnyTerrainImage(client::asset::IAssetReader& assets,
    const std::string& path,
    RgbaImage& out,
    const std::vector<std::filesystem::path>* additionalRoots = nullptr)
{
    DdsImage dds{};
    if (LoadDdsImage(assets, path, dds, additionalRoots))
        return DdsToRgba(dds, out);
    return LoadStbImage(assets, path, out, additionalRoots);
}

RgbaImage ResizeNearest(const RgbaImage& src, uint32_t width, uint32_t height)
{
    if (src.width == width && src.height == height)
        return src;
    RgbaImage out;
    out.filename = src.filename;
    out.width = width;
    out.height = height;
    out.pixels.resize(static_cast<size_t>(width) * height * 4u);
    for (uint32_t y = 0; y < height; ++y)
    {
        const uint32_t sy = std::min(src.height - 1u, static_cast<uint32_t>((static_cast<uint64_t>(y) * src.height) / height));
        for (uint32_t x = 0; x < width; ++x)
        {
            const uint32_t sx = std::min(src.width - 1u, static_cast<uint32_t>((static_cast<uint64_t>(x) * src.width) / width));
            const size_t dst = (static_cast<size_t>(y) * width + x) * 4u;
            const size_t srcIndex = (static_cast<size_t>(sy) * src.width + sx) * 4u;
            std::memcpy(out.pixels.data() + dst, src.pixels.data() + srcIndex, 4);
        }
    }
    return out;
}

std::vector<uint8_t> ExtractR8Channel(const RgbaImage& image)
{
    std::vector<uint8_t> out(static_cast<size_t>(image.width) * image.height, 0);
    for (uint32_t y = 0; y < image.height; ++y)
    {
        for (uint32_t x = 0; x < image.width; ++x)
        {
            const size_t pixel = static_cast<size_t>(y) * image.width + x;
            out[pixel] = image.pixels[pixel * 4u];
        }
    }
    return out;
}

std::vector<std::uint8_t> GenerateWaterNormalPixels(uint32_t width,
                                                    uint32_t height,
                                                    float frequencyA,
                                                    float frequencyB,
                                                    float amplitude)
{
    std::vector<std::uint8_t> pixels(static_cast<size_t>(width) * height * 4u, 255);
    constexpr float pi = 3.1415926535f;
    for (uint32_t y = 0; y < height; ++y)
    {
        for (uint32_t x = 0; x < width; ++x)
        {
            const float u = static_cast<float>(x) / static_cast<float>(width);
            const float v = static_cast<float>(y) / static_cast<float>(height);
            const float h0 = std::sin((u * frequencyA + v * 0.35f) * pi * 2.0f);
            const float h1 = std::cos((v * frequencyB - u * 0.28f) * pi * 2.0f);
            const float dx = amplitude * (std::cos((u * frequencyA + v * 0.35f) * pi * 2.0f) * frequencyA -
                std::sin((v * frequencyB - u * 0.28f) * pi * 2.0f) * 0.28f * frequencyB);
            const float dz = amplitude * (std::cos((u * frequencyA + v * 0.35f) * pi * 2.0f) * 0.35f * frequencyA +
                -std::sin((v * frequencyB - u * 0.28f) * pi * 2.0f) * frequencyB);
            const float ripple = (h0 + h1) * 0.04f;
            WorldVec3 n = WorldNormalize({-dx + ripple, 1.0f, -dz - ripple});
            const size_t offset = (static_cast<size_t>(y) * width + x) * 4u;
            pixels[offset + 0] = static_cast<std::uint8_t>(std::clamp(n.x * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f);
            pixels[offset + 1] = static_cast<std::uint8_t>(std::clamp(n.y * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f);
            pixels[offset + 2] = static_cast<std::uint8_t>(std::clamp(n.z * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f);
            pixels[offset + 3] = 255;
        }
    }
    return pixels;
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

bool CreateDeviceLocalAttachmentImage(VulkanDevice& device, VkDevice vkDevice, uint32_t width, uint32_t height,
    VkFormat format, VkImageUsageFlags usage, VkImage& image, VkDeviceMemory& memory)
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
    create.usage = usage;
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

uint32_t FullMipCount(uint32_t width, uint32_t height)
{
    uint32_t levels = 1;
    uint32_t size = std::max(width, height);
    while (size > 1)
    {
        size >>= 1u;
        ++levels;
    }
    return levels;
}

float SrgbToLinear(float value)
{
    value = std::clamp(value, 0.0f, 1.0f);
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

float LinearToSrgb(float value)
{
    value = std::clamp(value, 0.0f, 1.0f);
    return value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

uint8_t QuantizeByte(float value)
{
    return static_cast<uint8_t>(std::clamp(std::lround(value * 255.0f), 0l, 255l));
}

struct ArrayMipUpload
{
    std::vector<uint8_t> pixels;
    std::vector<VkBufferImageCopy> regions;
    uint32_t mipLevels = 1;
};

ArrayMipUpload BuildRgbaArrayMipUpload(uint32_t width,
                                       uint32_t height,
                                       uint32_t layers,
                                       const std::vector<uint8_t>& basePixels,
                                       bool srgbColor,
                                       bool normalMap)
{
    ArrayMipUpload upload{};
    upload.mipLevels = FullMipCount(width, height);
    upload.regions.reserve(static_cast<size_t>(layers) * upload.mipLevels);

    const size_t baseLayerBytes = static_cast<size_t>(width) * height * 4u;
    for (uint32_t layer = 0; layer < layers; ++layer)
    {
        std::vector<uint8_t> current(baseLayerBytes);
        std::memcpy(current.data(), basePixels.data() + static_cast<size_t>(layer) * baseLayerBytes, baseLayerBytes);
        uint32_t mipWidth = width;
        uint32_t mipHeight = height;

        for (uint32_t mip = 0; mip < upload.mipLevels; ++mip)
        {
            VkBufferImageCopy region{};
            region.bufferOffset = static_cast<VkDeviceSize>(upload.pixels.size());
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = mip;
            region.imageSubresource.baseArrayLayer = layer;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = {mipWidth, mipHeight, 1};
            upload.regions.push_back(region);
            upload.pixels.insert(upload.pixels.end(), current.begin(), current.end());

            if (mip + 1u >= upload.mipLevels)
                break;

            const uint32_t nextWidth = std::max(1u, mipWidth >> 1u);
            const uint32_t nextHeight = std::max(1u, mipHeight >> 1u);
            std::vector<uint8_t> next(static_cast<size_t>(nextWidth) * nextHeight * 4u, 255);
            for (uint32_t y = 0; y < nextHeight; ++y)
            {
                for (uint32_t x = 0; x < nextWidth; ++x)
                {
                    float accum[4] = {};
                    float normal[3] = {};
                    float samples = 0.0f;
                    for (uint32_t oy = 0; oy < 2; ++oy)
                    {
                        for (uint32_t ox = 0; ox < 2; ++ox)
                        {
                            const uint32_t sx = std::min(mipWidth - 1u, x * 2u + ox);
                            const uint32_t sy = std::min(mipHeight - 1u, y * 2u + oy);
                            const size_t src = (static_cast<size_t>(sy) * mipWidth + sx) * 4u;
                            if (normalMap)
                            {
                                normal[0] += static_cast<float>(current[src + 0]) / 255.0f * 2.0f - 1.0f;
                                normal[1] += static_cast<float>(current[src + 1]) / 255.0f * 2.0f - 1.0f;
                                normal[2] += static_cast<float>(current[src + 2]) / 255.0f * 2.0f - 1.0f;
                                accum[3] += static_cast<float>(current[src + 3]) / 255.0f;
                            }
                            else if (srgbColor)
                            {
                                accum[0] += SrgbToLinear(static_cast<float>(current[src + 0]) / 255.0f);
                                accum[1] += SrgbToLinear(static_cast<float>(current[src + 1]) / 255.0f);
                                accum[2] += SrgbToLinear(static_cast<float>(current[src + 2]) / 255.0f);
                                accum[3] += static_cast<float>(current[src + 3]) / 255.0f;
                            }
                            else
                            {
                                accum[0] += static_cast<float>(current[src + 0]) / 255.0f;
                                accum[1] += static_cast<float>(current[src + 1]) / 255.0f;
                                accum[2] += static_cast<float>(current[src + 2]) / 255.0f;
                                accum[3] += static_cast<float>(current[src + 3]) / 255.0f;
                            }
                            samples += 1.0f;
                        }
                    }

                    const size_t dst = (static_cast<size_t>(y) * nextWidth + x) * 4u;
                    if (normalMap)
                    {
                        const float len2 = normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2];
                        const float invLen = len2 > 0.000001f ? 1.0f / std::sqrt(len2) : 1.0f;
                        next[dst + 0] = QuantizeByte(normal[0] * invLen * 0.5f + 0.5f);
                        next[dst + 1] = QuantizeByte(normal[1] * invLen * 0.5f + 0.5f);
                        next[dst + 2] = QuantizeByte(normal[2] * invLen * 0.5f + 0.5f);
                        next[dst + 3] = QuantizeByte(accum[3] / samples);
                    }
                    else if (srgbColor)
                    {
                        next[dst + 0] = QuantizeByte(LinearToSrgb(accum[0] / samples));
                        next[dst + 1] = QuantizeByte(LinearToSrgb(accum[1] / samples));
                        next[dst + 2] = QuantizeByte(LinearToSrgb(accum[2] / samples));
                        next[dst + 3] = QuantizeByte(accum[3] / samples);
                    }
                    else
                    {
                        next[dst + 0] = QuantizeByte(accum[0] / samples);
                        next[dst + 1] = QuantizeByte(accum[1] / samples);
                        next[dst + 2] = QuantizeByte(accum[2] / samples);
                        next[dst + 3] = QuantizeByte(accum[3] / samples);
                    }
                }
            }
            current = std::move(next);
            mipWidth = nextWidth;
            mipHeight = nextHeight;
        }
    }

    return upload;
}

ArrayMipUpload BuildR8ArrayMipUpload(uint32_t width,
                                     uint32_t height,
                                     uint32_t layers,
                                     const std::vector<uint8_t>& basePixels)
{
    ArrayMipUpload upload{};
    upload.mipLevels = FullMipCount(width, height);
    upload.regions.reserve(static_cast<size_t>(layers) * upload.mipLevels);

    const size_t baseLayerBytes = static_cast<size_t>(width) * height;
    for (uint32_t layer = 0; layer < layers; ++layer)
    {
        std::vector<uint8_t> current(baseLayerBytes);
        std::memcpy(current.data(), basePixels.data() + static_cast<size_t>(layer) * baseLayerBytes, baseLayerBytes);
        uint32_t mipWidth = width;
        uint32_t mipHeight = height;

        for (uint32_t mip = 0; mip < upload.mipLevels; ++mip)
        {
            VkBufferImageCopy region{};
            region.bufferOffset = static_cast<VkDeviceSize>(upload.pixels.size());
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = mip;
            region.imageSubresource.baseArrayLayer = layer;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = {mipWidth, mipHeight, 1};
            upload.regions.push_back(region);
            upload.pixels.insert(upload.pixels.end(), current.begin(), current.end());

            if (mip + 1u >= upload.mipLevels)
                break;

            const uint32_t nextWidth = std::max(1u, mipWidth >> 1u);
            const uint32_t nextHeight = std::max(1u, mipHeight >> 1u);
            std::vector<uint8_t> next(static_cast<size_t>(nextWidth) * nextHeight, 0);
            for (uint32_t y = 0; y < nextHeight; ++y)
            {
                for (uint32_t x = 0; x < nextWidth; ++x)
                {
                    uint32_t sum = 0;
                    uint32_t samples = 0;
                    for (uint32_t oy = 0; oy < 2; ++oy)
                    {
                        for (uint32_t ox = 0; ox < 2; ++ox)
                        {
                            const uint32_t sx = std::min(mipWidth - 1u, x * 2u + ox);
                            const uint32_t sy = std::min(mipHeight - 1u, y * 2u + oy);
                            sum += current[static_cast<size_t>(sy) * mipWidth + sx];
                            ++samples;
                        }
                    }
                    next[static_cast<size_t>(y) * nextWidth + x] =
                        static_cast<uint8_t>((sum + samples / 2u) / samples);
                }
            }
            current = std::move(next);
            mipWidth = nextWidth;
            mipHeight = nextHeight;
        }
    }

    return upload;
}

double EstimateAverageActiveSplatLayers(const std::vector<uint8_t>& splatA,
                                        const std::vector<uint8_t>& splatB,
                                        uint32_t width,
                                        uint32_t height)
{
    const size_t texelCount = static_cast<size_t>(width) * height;
    if (texelCount == 0 ||
        splatA.size() < texelCount * 4u ||
        splatB.size() < texelCount * 4u)
    {
        return 1.0;
    }

    uint64_t activeTotal = 0;
    for (size_t texel = 0; texel < texelCount; ++texel)
    {
        const size_t byte = texel * 4u;
        uint32_t active = 0;
        for (uint32_t i = 0; i < 4; ++i)
        {
            active += splatA[byte + i] > 0 ? 1u : 0u;
            active += splatB[byte + i] > 0 ? 1u : 0u;
        }
        activeTotal += std::max(active, 1u);
    }

    return static_cast<double>(activeTotal) / static_cast<double>(texelCount);
}

float Smoothstep(float edge0, float edge1, float x)
{
    const float denom = std::max(edge1 - edge0, 0.0001f);
    const float t = std::clamp((x - edge0) / denom, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

double EstimateAverageSlopeAxes(const std::vector<float>& heightCmGrid,
                                uint32_t widthVertices,
                                uint32_t heightVertices,
                                float cellScaleMeters,
                                float slopeThreshold,
                                float slopeTransition)
{
    if (widthVertices < 2 || heightVertices < 2 ||
        heightCmGrid.size() < static_cast<size_t>(widthVertices) * heightVertices ||
        cellScaleMeters <= 0.0f)
    {
        return 1.0;
    }

    const auto heightMeters = [&](uint32_t x, uint32_t z) -> float {
        return heightCmGrid[static_cast<size_t>(z) * widthVertices + x] * 0.01f;
    };

    double axesTotal = 0.0;
    size_t samples = 0;
    for (uint32_t z = 0; z < heightVertices; ++z)
    {
        const uint32_t z0 = z == 0 ? z : z - 1u;
        const uint32_t z1 = std::min(heightVertices - 1u, z + 1u);
        const float dzDenom = static_cast<float>(std::max(1u, z1 - z0)) * cellScaleMeters;
        for (uint32_t x = 0; x < widthVertices; ++x)
        {
            const uint32_t x0 = x == 0 ? x : x - 1u;
            const uint32_t x1 = std::min(widthVertices - 1u, x + 1u);
            const float dxDenom = static_cast<float>(std::max(1u, x1 - x0)) * cellScaleMeters;
            const float dhdx = (heightMeters(x1, z) - heightMeters(x0, z)) / std::max(dxDenom, 0.0001f);
            const float dhdz = (heightMeters(x, z1) - heightMeters(x, z0)) / std::max(dzDenom, 0.0001f);
            const float normalY = 1.0f / std::sqrt(1.0f + dhdx * dhdx + dhdz * dhdz);
            const float slope = 1.0f - std::clamp(normalY, 0.0f, 1.0f);
            const float blend = Smoothstep(slopeThreshold, slopeThreshold + slopeTransition, slope);
            axesTotal += 1.0 + 2.0 * static_cast<double>(blend);
            ++samples;
        }
    }

    return samples == 0 ? 1.0 : axesTotal / static_cast<double>(samples);
}

bool CreateDepthImageArray(VulkanDevice& device, VkDevice vkDevice, uint32_t width, uint32_t height,
    uint32_t arrayLayers, VkFormat format, VkImage& image, VkDeviceMemory& memory)
{
    VkImageCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    create.imageType = VK_IMAGE_TYPE_2D;
    create.format = format;
    create.extent = {width, height, 1};
    create.mipLevels = 1;
    create.arrayLayers = arrayLayers;
    create.samples = VK_SAMPLE_COUNT_1_BIT;
    create.tiling = VK_IMAGE_TILING_OPTIMAL;
    create.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
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

void TransitionDepthArrayLayout(VkCommandBuffer cmd, VkImage image, uint32_t arrayLayers,
    VkImageLayout oldLayout, VkImageLayout newLayout)
{
    if (oldLayout == newLayout)
        return;

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = arrayLayers;

    VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    if (oldLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL &&
        newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
    {
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dstStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    }
    else if (newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL)
    {
        barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }
    else
    {
        barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }

    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

WorldMat4 WorldOrthographicOffCenter(float left, float right, float bottom, float top, float zNear, float zFar)
{
    WorldMat4 r{};
    r.m[0] = 2.0f / (right - left);
    r.m[5] = 2.0f / (top - bottom);
    r.m[10] = 1.0f / (zFar - zNear);
    r.m[12] = -(right + left) / (right - left);
    r.m[13] = -(top + bottom) / (top - bottom);
    r.m[14] = -zNear / (zFar - zNear);
    r.m[15] = 1.0f;
    return r;
}

WorldVec3 TransformPoint(const WorldMat4& m, WorldVec3 p)
{
    return {
        p.x * m.m[0] + p.y * m.m[4] + p.z * m.m[8] + m.m[12],
        p.x * m.m[1] + p.y * m.m[5] + p.z * m.m[9] + m.m[13],
        p.x * m.m[2] + p.y * m.m[6] + p.z * m.m[10] + m.m[14]};
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
        const std::string lowerRelative = LowerCopy(relative);
        const size_t slash = relative.find_last_of('/');
        const std::string fileName = slash == std::string::npos ? relative : relative.substr(slash + 1);
        const std::string lowerFileName = LowerCopy(fileName);

        std::string category = "egyeb";
        if (lowerRelative.find("flame area/") != std::string::npos ||
            lowerFileName.find("magma") != std::string::npos ||
            lowerFileName.find("valcano") != std::string::npos ||
            lowerFileName.find("volcano") != std::string::npos)
            category = "lava";
        else if (lowerFileName.find("water") != std::string::npos)
            category = "viz";
        else if (lowerFileName.find("ice") != std::string::npos)
            category = "jeges";
        else if (lowerFileName.find("snow") != std::string::npos)
            category = "ho";
        else if (lowerFileName.find("tile") != std::string::npos)
            category = "kavicsos";
        else if (lowerFileName.find("sand") != std::string::npos ||
                 lowerRelative.rfind("b/beach/", 0) == 0 ||
                 lowerRelative.rfind("n/desert/sand/", 0) == 0)
            category = "homokos";
        else if (lowerFileName.find("grass") != std::string::npos)
            category = "fu";
        else if (lowerFileName.find("field") != std::string::npos)
            category = "foldes";
        else if (lowerFileName.find("stone") != std::string::npos ||
                 lowerFileName.find("rock") != std::string::npos)
            category = "szikla";

        std::string movedName = fileName;
        if (lowerRelative.rfind("n/desert/field/", 0) == 0)
            movedName = "n_desert_field_" + fileName;
        else if (lowerRelative.rfind("n/desert/grass/", 0) == 0)
            movedName = "n_desert_grass_" + fileName;
        else if (lowerRelative.rfind("n/desert/stone/", 0) == 0)
            movedName = "n_desert_stone_" + fileName;
        else if (lowerRelative.rfind("n/desert/tile/", 0) == 0)
            movedName = "n_desert_tile_" + fileName;
        else if (lowerRelative.rfind("n/snow.m/", 0) == 0 &&
                 (lowerFileName.find("field") != std::string::npos ||
                  lowerFileName.find("grass") != std::string::npos ||
                  lowerFileName.find("stone") != std::string::npos ||
                  lowerFileName.find("tile") != std::string::npos))
            movedName = "n_snow_m_" + fileName;

        return "assets/Textures/" + category + "/" + movedName;
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

const char* TeditToolModeName(MapEditorToolMode mode)
{
    switch (mode)
    {
    case MapEditorToolMode::WaterSculpt: return "water";
    case MapEditorToolMode::Heightmap: return "sculpt";
    case MapEditorToolMode::SplatPaint: return "splat";
    case MapEditorToolMode::None:
    default: return "none";
    }
}

const char* TeditToolName(MapEditorTool tool)
{
    switch (tool)
    {
    case MapEditorTool::Raise: return "raise";
    case MapEditorTool::Lower: return "lower";
    case MapEditorTool::Smooth: return "smooth";
    case MapEditorTool::Flatten: return "flatten";
    case MapEditorTool::Paint: return "splat";
    default: return "unknown";
    }
}
}

bool TerrainRenderer::Create(VulkanDevice& device, client::asset::IAssetReader& assets)
{
    Destroy();
    m_device = device.GetDevice();
    m_deviceOwner = &device;
    m_assets = &assets;
    LoadEditorConfig();

    const bool texture = CreateFallbackTexture(device);
    const bool mask = texture ? CreateFallbackSplatTextures(device) : false;
    const bool buffers = mask ? CreateBuffers(device) : false;
    const bool shadows = buffers ? CreateShadowResources(device) : false;
    const bool descriptors = shadows ? CreateDescriptors() : false;
    const bool pipeline = descriptors ? CreatePipeline(device) : false;
    const bool water = pipeline ? CreateWaterResources(device) : false;
    m_sceneTerrainActive = false;
    m_sceneTerrain = {};
    Tracenf("[TERRAIN] Create: texture=%d mask=%d buffers=%d shadows=%d descriptors=%d pipeline=%d",
        texture ? 1 : 0,
        mask ? 1 : 0,
        buffers ? 1 : 0,
        shadows ? 1 : 0,
        descriptors ? 1 : 0,
        pipeline ? 1 : 0);

    if (texture && mask && buffers && shadows && descriptors && pipeline && water)
        return true;

    Destroy();
    return false;
}

void TerrainRenderer::SetAdditionalAssetRoots(std::vector<std::filesystem::path> roots)
{
    m_additionalAssetRoots.clear();
    for (std::filesystem::path& root : roots)
    {
        if (!root.empty())
            m_additionalAssetRoots.push_back(std::move(root));
    }
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
    DestroyBuffer(m_selectedWaterBodyVertexBuffer);
    DestroyBuffer(m_selectedWaterBodyIndexBuffer);
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
    m_selectedWaterBodyIndexCount = 0;
    m_selectedWaterBodyId = 0;
    m_zoneFillDebugRanges.clear();
    m_zoneBorderDebugRanges.clear();
    m_zoneLabelDebugRanges.clear();
    m_walkabilityDebug = false;
    m_mapEditorOpen = false;
    m_editorBrushVisible = false;
    m_editorRaiseHeld = false;
    m_editorLowerHeld = false;

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
        LoadWaterBodies(device, mapDirectory);
        CreateDescriptors();
        return flat;
    }

    LoadWaterBodies(device, mapDirectory);
    CreateDescriptors();
    m_sceneTerrainActive = true;
    m_sceneTerrain.exists = true;
    m_sceneTerrain.name = "Terrain";
    m_sceneTerrain.widthMeters = static_cast<float>(m_mapSizeX) * m_cellScaleMeters;
    m_sceneTerrain.depthMeters = static_cast<float>(m_mapSizeY) * m_cellScaleMeters;
    m_sceneTerrain.cellSizeMeters = m_cellScaleMeters;
    m_sceneTerrain.cellsX = m_mapSizeX;
    m_sceneTerrain.cellsZ = m_mapSizeY;
    return true;
}

bool TerrainRenderer::CreateFlatTerrain(VulkanDevice& device, const TerrainSceneData& terrain)
{
    if (!m_device)
        return false;

    TerrainSceneData next = terrain;
    next.exists = true;
    if (next.name.empty())
        next.name = "Terrain";
    next.cellSizeMeters = std::max(0.01f, next.cellSizeMeters);
    next.cellsX = std::max(1u, next.cellsX);
    next.cellsZ = std::max(1u, next.cellsZ);
    next.chunkSizeCells = std::clamp(next.chunkSizeCells == 0 ? 64u : next.chunkSizeCells, 32u, 256u);
    if (next.widthMeters <= 0.0f)
        next.widthMeters = static_cast<float>(next.cellsX) * next.cellSizeMeters;
    if (next.depthMeters <= 0.0f)
        next.depthMeters = static_cast<float>(next.cellsZ) * next.cellSizeMeters;
    next.triplanarSharpness = std::clamp(next.triplanarSharpness, 1.0f, 16.0f);
    next.triplanarSlopeThreshold = std::clamp(next.triplanarSlopeThreshold, 0.0f, 1.0f);
    next.triplanarSlopeTransition = std::clamp(next.triplanarSlopeTransition, 0.001f, 1.0f);

    device.WaitIdle();
    DestroyBuffer(m_vertexBuffer);
    DestroyBuffer(m_indexBuffer);
    DestroyBuffer(m_debugVertexBuffer);
    DestroyBuffer(m_debugIndexBuffer);
    DestroyBuffer(m_logicVertexBuffer);
    DestroyBuffer(m_logicIndexBuffer);
    DestroyBuffer(m_selectedWaterBodyVertexBuffer);
    DestroyBuffer(m_selectedWaterBodyIndexBuffer);
    DestroyTerrainLayers();
    DestroyWaterBodyResources();
    DestroyTexture(m_splatA);
    DestroyTexture(m_splatB);
    m_tileIndices.clear();
    m_tileGridWidth = 0;
    m_tileGridHeight = 0;
    m_indexCount = 0;
    m_debugIndexCount = 0;
    m_selectedWaterBodyIndexCount = 0;
    m_selectedWaterBodyId = 0;
    m_waterBodies.clear();
    m_heightCmGrid.clear();
    m_attributes.clear();
    m_splatABytes.clear();
    m_splatBBytes.clear();
    m_dirtyChunkTexels.clear();
    m_undoStack.clear();

    m_flatTerrainWidthMeters = next.widthMeters;
    m_flatTerrainDepthMeters = next.depthMeters;
    m_cellScaleMeters = next.cellSizeMeters;
    m_mapSizeX = next.cellsX;
    m_mapSizeY = next.cellsZ;
    m_chunkSizeCells = next.chunkSizeCells;
    m_heightGridWidth = next.cellsX + 1u;
    m_heightGridHeight = next.cellsZ + 1u;
    m_spawnLocalXcm = next.widthMeters * 50.0f;
    m_spawnLocalYcm = next.depthMeters * 50.0f;
    m_spawnHeightCm = 0.0f;
    const size_t expectedHeightCount = static_cast<size_t>(m_heightGridWidth) * m_heightGridHeight;
    if (next.heightCmGrid.size() == expectedHeightCount)
        m_heightCmGrid = next.heightCmGrid;
    else
        m_heightCmGrid.assign(expectedHeightCount, 0.0f);
    m_heightUndoRecorded.assign(m_heightCmGrid.size(), 0);
    m_splatWidth = std::max(1u, next.cellsX);
    m_splatHeight = std::max(1u, next.cellsZ);
    m_chunkSplatWidth = m_chunkSizeCells;
    m_chunkSplatHeight = m_chunkSizeCells;
    const size_t expectedSplatBytes = static_cast<size_t>(m_splatWidth) * m_splatHeight * 4u;
    if (next.splatABytes.size() == expectedSplatBytes)
        m_splatABytes = next.splatABytes;
    else
        m_splatABytes.assign(expectedSplatBytes, 0);
    if (next.splatBBytes.size() == expectedSplatBytes)
        m_splatBBytes = next.splatBBytes;
    else
        m_splatBBytes.assign(expectedSplatBytes, 0);
    m_splatUndoRecorded.assign(m_splatABytes.size() + m_splatBBytes.size(), 0);
    const uint32_t chunksX = (m_mapSizeX + m_chunkSizeCells - 1u) / m_chunkSizeCells;
    const uint32_t chunksY = (m_mapSizeY + m_chunkSizeCells - 1u) / m_chunkSizeCells;
    m_dirtyChunkTexels.assign(static_cast<size_t>(chunksX) * chunksY, 0);

    if (!CreateFlatBuffers(device))
        return false;
    if (!CreateSceneSplatTextures(device))
        return false;
    CreateDescriptors();

    m_mapLoaded = true;
    m_sceneTerrainActive = true;
    m_sceneTerrain = next;
    m_triplanarParamsDirty = true;
    Tracenf("[TERRAIN-CREATE] dims=%.2fx%.2f m, cellSize=%.2f m, cells=%ux%u, verts=%zu, pos=(0.00,0.00,0.00)",
        next.widthMeters,
        next.depthMeters,
        next.cellSizeMeters,
        next.cellsX,
        next.cellsZ,
        m_heightCmGrid.size());
    const uint32_t chunkGridX = (m_mapSizeX + m_chunkSizeCells - 1u) / m_chunkSizeCells;
    const uint32_t chunkGridY = (m_mapSizeY + m_chunkSizeCells - 1u) / m_chunkSizeCells;
    Tracenf("[TCHUNK] create dims=%.2fx%.2f m cellSize=%.2f chunkSize=%u cells=%ux%u chunkGrid=%ux%u",
        next.widthMeters,
        next.depthMeters,
        next.cellSizeMeters,
        m_chunkSizeCells,
        m_mapSizeX,
        m_mapSizeY,
        chunkGridX,
        chunkGridY);
    Tracenf("[TEDIT-DIAG] terrain created id=%p registeredAsEditTarget=%s activeTerrain=%p dims=%ux%u cells heightBuffer=%p splatTarget=%p/%p",
        static_cast<void*>(this),
        (m_sceneTerrainActive && m_mapLoaded && m_heightGridWidth > 1 && m_heightGridHeight > 1) ? "yes" : "no",
        m_sceneTerrainActive ? static_cast<void*>(this) : nullptr,
        m_mapSizeX,
        m_mapSizeY,
        m_heightCmGrid.empty() ? nullptr : static_cast<void*>(m_heightCmGrid.data()),
        m_splatABytes.empty() ? nullptr : static_cast<void*>(m_splatABytes.data()),
        m_splatBBytes.empty() ? nullptr : static_cast<void*>(m_splatBBytes.data()));
    Tracenf("[TEDIT-DIAG] tool terrain==created terrain ? %s",
        m_sceneTerrainActive ? "yes" : "no");
    return true;
}

void TerrainRenderer::ClearTerrain(VulkanDevice& device)
{
    if (!m_device)
        return;
    device.WaitIdle();
    DestroyBuffer(m_vertexBuffer);
    DestroyBuffer(m_indexBuffer);
    DestroyBuffer(m_debugVertexBuffer);
    DestroyBuffer(m_debugIndexBuffer);
    DestroyBuffer(m_logicVertexBuffer);
    DestroyBuffer(m_logicIndexBuffer);
    DestroyBuffer(m_selectedWaterBodyVertexBuffer);
    DestroyBuffer(m_selectedWaterBodyIndexBuffer);
    DestroyTerrainLayers();
    DestroyWaterBodyResources();
    DestroyTexture(m_splatA);
    DestroyTexture(m_splatB);
    m_waterBodies.clear();
    m_tileIndices.clear();
    m_heightCmGrid.clear();
    m_attributes.clear();
    m_splatABytes.clear();
    m_splatBBytes.clear();
    m_dirtyChunkTexels.clear();
    m_heightUndoRecorded.clear();
    m_splatUndoRecorded.clear();
    m_indexCount = 0;
    m_debugIndexCount = 0;
    m_heightGridWidth = 0;
    m_heightGridHeight = 0;
    m_mapSizeX = 0;
    m_mapSizeY = 0;
    m_mapLoaded = false;
    m_sceneTerrainActive = false;
    m_sceneTerrain = {};
    CreateDescriptors();
    Tracenf("[TEDIT-DIAG] active terrain cleared id=%p activeTerrain=NULL", static_cast<void*>(this));
}

TerrainSceneData TerrainRenderer::GetTerrainSceneData() const
{
    TerrainSceneData data = m_sceneTerrain;
    data.exists = m_sceneTerrainActive;
    if (data.exists)
    {
        data.cellSizeMeters = m_cellScaleMeters;
        data.cellsX = m_mapSizeX;
        data.cellsZ = m_mapSizeY;
        data.chunkSizeCells = m_chunkSizeCells == 0 ? data.chunkSizeCells : m_chunkSizeCells;
        data.widthMeters = m_flatTerrainWidthMeters > 0.0f
            ? m_flatTerrainWidthMeters
            : static_cast<float>(m_mapSizeX) * m_cellScaleMeters;
        data.depthMeters = m_flatTerrainDepthMeters > 0.0f
            ? m_flatTerrainDepthMeters
            : static_cast<float>(m_mapSizeY) * m_cellScaleMeters;
        data.heightCmGrid = m_heightCmGrid;
        data.splatABytes = m_splatABytes;
        data.splatBBytes = m_splatBBytes;
    }
    return data;
}

void TerrainRenderer::SetTerrainSceneData(const TerrainSceneData& terrain)
{
    if (!m_sceneTerrainActive)
        return;
    m_sceneTerrain = terrain;
    m_sceneTerrain.exists = true;
    m_sceneTerrain.triplanarSharpness = std::clamp(m_sceneTerrain.triplanarSharpness, 1.0f, 16.0f);
    m_sceneTerrain.triplanarSlopeThreshold = std::clamp(m_sceneTerrain.triplanarSlopeThreshold, 0.0f, 1.0f);
    m_sceneTerrain.triplanarSlopeTransition = std::clamp(m_sceneTerrain.triplanarSlopeTransition, 0.001f, 1.0f);
    m_triplanarParamsDirty = true;
}

bool TerrainRenderer::RecreatePipeline(VulkanDevice& device)
{
    if (!m_device)
        return true;

    DestroyPipeline();
    DestroyWaterReflectionPipeline();
    if ((m_mainRenderPass ? m_mainRenderPass : device.GetRenderPass()) == VK_NULL_HANDLE)
        return true;

    const bool terrainPipeline = CreatePipeline(device);
    const bool reflectionResources = terrainPipeline ? CreateOrRecreateWaterReflectionResources(device, true) : false;
    const bool reflectionPipeline = reflectionResources ? CreateWaterReflectionPipeline(device) : false;
    if (reflectionPipeline)
        UpdateWaterDescriptors();
    const bool waterPipeline = reflectionPipeline ? CreateWaterPipeline(device) : false;
    return terrainPipeline && reflectionResources && reflectionPipeline && waterPipeline;
}

void TerrainRenderer::SetMainRenderPass(VkRenderPass renderPass)
{
    m_mainRenderPass = renderPass;
}

void TerrainRenderer::SetWaterRefractionInputs(VkImageView colorView,
                                               VkImageView depthView,
                                               VkSampler sampler,
                                               VkExtent2D extent)
{
    if (m_waterSceneColorView == colorView &&
        m_waterSceneDepthView == depthView &&
        m_waterSceneSampler == sampler &&
        m_waterSceneExtent.width == extent.width &&
        m_waterSceneExtent.height == extent.height)
    {
        return;
    }

    m_waterSceneColorView = colorView;
    m_waterSceneDepthView = depthView;
    m_waterSceneSampler = sampler;
    m_waterSceneExtent = extent;
    UpdateWaterDescriptors();
}

void TerrainRenderer::UpdateShadowCascades(const WorldCamera& camera)
{
    const WorldVec3 forward = WorldNormalize(WorldSub(camera.target, camera.eye));
    WorldVec3 right = WorldNormalize(WorldCross({0.0f, 1.0f, 0.0f}, forward));
    if (WorldDot(right, right) <= 0.0001f)
        right = {1.0f, 0.0f, 0.0f};
    const WorldVec3 up = WorldNormalize(WorldCross(forward, right));
    const float aspect = 16.0f / 9.0f;
    const float tanHalfFov = std::tan(45.0f * 3.1415926535f / 180.0f * 0.5f);
    const float nearPlane = 0.1f;
    const float farPlane = 200.0f;
    constexpr float lambda = 0.7f;

    float splitPlanes[kShadowCascadeCount + 1]{};
    splitPlanes[0] = nearPlane;
    for (uint32_t i = 1; i < kShadowCascadeCount; ++i)
    {
        const float p = static_cast<float>(i) / static_cast<float>(kShadowCascadeCount);
        const float logSplit = nearPlane * std::pow(farPlane / nearPlane, p);
        const float uniformSplit = nearPlane + (farPlane - nearPlane) * p;
        splitPlanes[i] = uniformSplit * (1.0f - lambda) + logSplit * lambda;
    }
    splitPlanes[kShadowCascadeCount] = farPlane;

    const DirectionalLight& sun = m_lightingState.directional;
    const float azimuthRadians = std::clamp(sun.azimuthDegrees, 0.0f, 360.0f) * 3.1415926535f / 180.0f;
    const float elevationRadians = std::clamp(sun.elevationDegrees, 0.0f, 90.0f) * 3.1415926535f / 180.0f;
    const float cosElevation = std::cos(elevationRadians);
    WorldVec3 sunDir = WorldNormalize({
        cosElevation * std::sin(azimuthRadians),
        std::sin(elevationRadians),
        cosElevation * std::cos(azimuthRadians)});
    if (WorldDot(sunDir, sunDir) <= 0.0001f)
        sunDir = {0.0f, 1.0f, 0.0f};

    for (uint32_t cascade = 0; cascade < kShadowCascadeCount; ++cascade)
    {
        const float zn = splitPlanes[cascade];
        const float zf = splitPlanes[cascade + 1];
        const float nearH = 2.0f * tanHalfFov * zn;
        const float nearW = nearH * aspect;
        const float farH = 2.0f * tanHalfFov * zf;
        const float farW = farH * aspect;
        const WorldVec3 nearCenter = WorldAdd(camera.eye, WorldScale(forward, zn));
        const WorldVec3 farCenter = WorldAdd(camera.eye, WorldScale(forward, zf));
        std::array<WorldVec3, 8> corners = {
            WorldAdd(WorldAdd(nearCenter, WorldScale(up, nearH * 0.5f)), WorldScale(right, -nearW * 0.5f)),
            WorldAdd(WorldAdd(nearCenter, WorldScale(up, nearH * 0.5f)), WorldScale(right, nearW * 0.5f)),
            WorldAdd(WorldAdd(nearCenter, WorldScale(up, -nearH * 0.5f)), WorldScale(right, -nearW * 0.5f)),
            WorldAdd(WorldAdd(nearCenter, WorldScale(up, -nearH * 0.5f)), WorldScale(right, nearW * 0.5f)),
            WorldAdd(WorldAdd(farCenter, WorldScale(up, farH * 0.5f)), WorldScale(right, -farW * 0.5f)),
            WorldAdd(WorldAdd(farCenter, WorldScale(up, farH * 0.5f)), WorldScale(right, farW * 0.5f)),
            WorldAdd(WorldAdd(farCenter, WorldScale(up, -farH * 0.5f)), WorldScale(right, -farW * 0.5f)),
            WorldAdd(WorldAdd(farCenter, WorldScale(up, -farH * 0.5f)), WorldScale(right, farW * 0.5f)),
        };

        WorldVec3 center{};
        for (WorldVec3 corner : corners)
            center = WorldAdd(center, corner);
        center = WorldScale(center, 1.0f / static_cast<float>(corners.size()));

        const WorldVec3 lightEye = WorldSub(center, WorldScale(sunDir, 120.0f));
        WorldMat4 lightView = WorldLookAt(lightEye, center, {0.0f, 1.0f, 0.0f});

        WorldVec3 minBound{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
        WorldVec3 maxBound{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
        for (WorldVec3 corner : corners)
        {
            const WorldVec3 p = TransformPoint(lightView, corner);
            minBound.x = std::min(minBound.x, p.x);
            minBound.y = std::min(minBound.y, p.y);
            minBound.z = std::min(minBound.z, p.z);
            maxBound.x = std::max(maxBound.x, p.x);
            maxBound.y = std::max(maxBound.y, p.y);
            maxBound.z = std::max(maxBound.z, p.z);
        }

        constexpr float padding = 20.0f;
        WorldMat4 lightProj = WorldOrthographicOffCenter(
            minBound.x - padding, maxBound.x + padding,
            minBound.y - padding, maxBound.y + padding,
            minBound.z - 80.0f, maxBound.z + 80.0f);
        m_shadowCascadeViewProj[cascade] = WorldMultiply(lightView, lightProj);
        m_shadowCascadeSplits[cascade] = zf;
    }
}

void TerrainRenderer::RenderSunShadowMap(VulkanDevice& device, const WorldCamera& camera)
{
    if (!m_sceneTerrainActive || m_sceneTerrain.editorHidden || !m_lightingState.sunShadowsEnabled || !m_shadowPipeline || !m_shadowImage ||
        !m_vertexBuffer.buffer || !m_indexBuffer.buffer || m_indexCount == 0 || !device.IsFrameActive())
        return;

    UpdateShadowCascades(camera);

    VkCommandBuffer cmd = device.GetCommandBuffer();
    TransitionDepthArrayLayout(cmd, m_shadowImage, kShadowCascadeCount, m_shadowLayout,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    m_shadowLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(kShadowResolution);
    viewport.height = static_cast<float>(kShadowResolution);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{{0, 0}, {kShadowResolution, kShadowResolution}};
    VkClearValue clear{};
    clear.depthStencil = {1.0f, 0};
    VkDeviceSize offset = 0;

    if (m_sceneTerrain.triplanarEnabled && !m_triPerfShadowPassLogged)
    {
        Tracenf("[TRI-PERF] terrain pipeline bound in pass=shadow-cascade0..%u triplanar=no shader=depth-only extent=%ux%u",
            kShadowCascadeCount - 1u,
            kShadowResolution,
            kShadowResolution);
        m_triPerfShadowPassLogged = true;
    }

    for (uint32_t cascade = 0; cascade < kShadowCascadeCount; ++cascade)
    {
        VkRenderPassBeginInfo pass{};
        pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        pass.renderPass = m_shadowRenderPass;
        pass.framebuffer = m_shadowFramebuffers[cascade];
        pass.renderArea.offset = {0, 0};
        pass.renderArea.extent = {kShadowResolution, kShadowResolution};
        pass.clearValueCount = 1;
        pass.pClearValues = &clear;
        vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipeline);
        vkCmdPushConstants(cmd, m_shadowPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
            0, sizeof(WorldMat4), &m_shadowCascadeViewProj[cascade]);
        vkCmdBindVertexBuffers(cmd, 0, 1, &m_vertexBuffer.buffer, &offset);
        vkCmdBindIndexBuffer(cmd, m_indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, m_indexCount, 1, 0, 0, 0);
        vkCmdEndRenderPass(cmd);
    }

    TransitionDepthArrayLayout(cmd, m_shadowImage, kShadowCascadeCount,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    m_shadowLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
}

WorldCamera TerrainRenderer::ComputeMirrorCamera(const WorldCamera& camera, VkExtent2D extent, float waterLevelY) const
{
    const float waterY = waterLevelY;
    WorldCamera mirror{};
    mirror.eye = {camera.eye.x, 2.0f * waterY - camera.eye.y, camera.eye.z};
    mirror.target = {camera.target.x, 2.0f * waterY - camera.target.y, camera.target.z};
    const float aspect = extent.height != 0 ? static_cast<float>(extent.width) / static_cast<float>(extent.height) : 1.0f;
    const WorldMat4 view = WorldLookAt(mirror.eye, mirror.target, {0.0f, 1.0f, 0.0f});
    mirror.nearPlane = camera.nearPlane;
    mirror.farPlane = camera.farPlane;
    const WorldMat4 projection = WorldPerspective(45.0f * 3.1415926535f / 180.0f, aspect, mirror.nearPlane, mirror.farPlane);
    mirror.viewProjection = WorldMultiply(view, projection);
    return mirror;
}

const TerrainRenderer::WaterBodyGpu* TerrainRenderer::FindClosestWaterBody(const WorldCamera& camera,
                                                                           float* outDistanceMeters) const
{
    const WaterBodyGpu* closest = nullptr;
    float closestDistanceSq = std::numeric_limits<float>::max();
    for (const WaterBodyGpu& waterBody : m_waterBodies)
    {
        if (!ResolveWaterConfig(waterBody.body).enabled || waterBody.indexCount == 0)
            continue;

        const float centerX = (waterBody.body.bboxMin[0] + waterBody.body.bboxMax[0]) * 0.5f;
        const float centerZ = (waterBody.body.bboxMin[1] + waterBody.body.bboxMax[1]) * 0.5f;
        const float dx = camera.eye.x - centerX;
        const float dz = camera.eye.z - centerZ;
        const float distanceSq = dx * dx + dz * dz;
        if (distanceSq < closestDistanceSq)
        {
            closestDistanceSq = distanceSq;
            closest = &waterBody;
        }
    }

    if (outDistanceMeters)
        *outDistanceMeters = closest ? std::sqrt(closestDistanceSq) : 0.0f;
    return closest;
}

void TerrainRenderer::RenderWaterReflection(VulkanDevice& device,
    const WorldCamera& camera,
    double timeSeconds,
    const std::function<void(const WorldCamera&, VkExtent2D, VkRenderPass, float)>& renderEntities)
{
    const WaterBodyGpu* reflectionBody = nullptr;
    float reflectionDistanceMeters = 0.0f;
    if (!m_sceneTerrainActive || m_sceneTerrain.editorHidden || m_waterBodies.empty())
        return;

    reflectionBody = FindClosestWaterBody(camera, &reflectionDistanceMeters);
    if (!reflectionBody || !ResolveWaterConfig(reflectionBody->body).reflectionEnabled)
    {
        if (timeSeconds - m_lastWaterDiagTimeSeconds >= 1.0)
        {
            if (reflectionBody)
            {
                Tracenf("[WATER-OBJ] diag: bodies=%zu reflection_target=id=%u name=\"%s\" distance=%.1fm reflection=disabled",
                    m_waterBodies.size(),
                    reflectionBody->body.id,
                    reflectionBody->body.name.c_str(),
                    reflectionDistanceMeters);
            }
            else
            {
                Tracenf("[WATER-OBJ] diag: bodies=%zu reflection_target=<none>", m_waterBodies.size());
            }
            m_lastWaterDiagTimeSeconds = timeSeconds;
        }
        return;
    }

    const WaterConfig& reflectionConfig = ResolveWaterConfig(reflectionBody->body);
    const float reflectionWaterLevelY = reflectionBody->body.waterLevelY;
    if (!reflectionConfig.enabled || !reflectionConfig.reflectionEnabled || !m_waterReflectionPipeline ||
        !m_waterReflection.framebuffer || !m_indexCount || !device.IsFrameActive())
        return;

    const VkExtent2D swapExtent = device.GetSwapchainExtent();
    if (swapExtent.width == 0 || swapExtent.height == 0)
        return;

    if (m_waterReflection.quality != reflectionConfig.reflectionQuality ||
        m_waterReflection.width == 0 || m_waterReflection.height == 0 ||
        m_waterReflection.width > swapExtent.width || m_waterReflection.height > swapExtent.height)
    {
        CreateOrRecreateWaterReflectionResources(device, true, reflectionConfig.reflectionQuality);
        CreateWaterReflectionPipeline(device);
        UpdateWaterDescriptors();
    }

    if (!m_waterReflection.framebuffer || !m_waterReflectionPipeline)
        return;

    const uint32_t frameIndex = device.GetFrameIndex();
    const WorldCamera mirror = ComputeMirrorCamera(camera, {m_waterReflection.width, m_waterReflection.height}, reflectionWaterLevelY);
    m_reflectionClipWaterLevelY = reflectionWaterLevelY;
    UpdateUniform(frameIndex, mirror, true);

    VkCommandBuffer cmd = device.GetCommandBuffer();
    std::array<VkClearValue, 2> clears{};
    clears[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clears[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = m_waterReflection.renderPass;
    pass.framebuffer = m_waterReflection.framebuffer;
    pass.renderArea.offset = {0, 0};
    pass.renderArea.extent = {m_waterReflection.width, m_waterReflection.height};
    pass.clearValueCount = static_cast<uint32_t>(clears.size());
    pass.pClearValues = clears.data();

    vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(m_waterReflection.width);
    viewport.height = static_cast<float>(m_waterReflection.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{{0, 0}, {m_waterReflection.width, m_waterReflection.height}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_waterReflectionPipeline);
    if (m_sceneTerrain.triplanarEnabled && !m_triPerfReflectionPassLogged)
    {
        Tracenf("[TRI-PERF] terrain pipeline bound in pass=water-reflection triplanar=yes extent=%ux%u shader=terrain_ps",
            m_waterReflection.width,
            m_waterReflection.height);
        m_triPerfReflectionPassLogged = true;
    }

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
            vkCmdPushConstants(cmd, m_waterReflectionPipelineLayout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(push), &push);
            const VkDescriptorSet descriptorSet =
                m_layerDescriptorSets[layerIndex * kFramesInFlight + frameIndex];
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_waterReflectionPipelineLayout,
                0, 1, &descriptorSet, 0, nullptr);
            vkCmdDrawIndexed(cmd, m_indexCount, 1, 0, 0, 0);
        }
    }
    else
    {
        TerrainPushConstants push{{1.0f, 1.0f, 0.0f, 0.0f}};
        vkCmdPushConstants(cmd, m_waterReflectionPipelineLayout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(push), &push);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_waterReflectionPipelineLayout,
            0, 1, &m_descriptorSets[frameIndex], 0, nullptr);
        vkCmdDrawIndexed(cmd, m_indexCount, 1, 0, 0, 0);
    }

    if (renderEntities)
        renderEntities(mirror, {m_waterReflection.width, m_waterReflection.height}, m_waterReflection.renderPass, reflectionWaterLevelY);

    vkCmdEndRenderPass(cmd);

    m_reflectionClipWaterLevelY = std::numeric_limits<float>::quiet_NaN();
}

void TerrainRenderer::Render(VulkanDevice& device, const WorldCamera& camera)
{
    static bool loggedDraw = false;
    static bool loggedSkip = false;

    if (!m_sceneTerrainActive || m_sceneTerrain.editorHidden || !m_pipeline || m_indexCount == 0 || !device.IsFrameActive())
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
    if (m_sceneTerrain.triplanarEnabled && !m_triPerfMainPassLogged)
    {
        Tracenf("[TRI-PERF] terrain pipeline bound in pass=main triplanar=yes extent=%ux%u shader=terrain_ps",
            extent.width,
            extent.height);
        Tracenf("[TRI-PERF] offscreen target extent=%ux%u fragment-bound-cost-scales-with-pixels",
            extent.width,
            extent.height);
        m_triPerfMainPassLogged = true;
    }

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

    if (m_mapLoaded && m_mapEditorOpen && m_editorBrushVisible)
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

    if (m_mapLoaded && m_mapEditorOpen && m_waterSculptBrushVisible)
    {
        TerrainPushConstants brushPush{{m_waterSculptBrushWorldX,
                                        m_waterSculptBrushWorldZ,
                                        m_waterSculptBrushAddMode ? 8.0f : 9.0f,
                                        m_waterSculptBrushRadiusMeters}};
        vkCmdPushConstants(cmd, m_pipelineLayout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(brushPush), &brushPush);
        vkCmdBindVertexBuffers(cmd, 0, 1, &m_vertexBuffer.buffer, &offset);
        vkCmdBindIndexBuffer(cmd, m_indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
            0, 1, &m_descriptorSets[frameIndex], 0, nullptr);
        vkCmdDrawIndexed(cmd, m_indexCount, 1, 0, 0, 0);
    }

    if (!loggedDraw)
    {
        const size_t paletteLayers = m_baseTexture.view && m_normalTexture.view &&
            m_aoTexture.view && m_roughnessTexture.view && m_metallicTexture.view && m_heightTexture.view
                ? m_paletteSlots.size()
                : 0u;
        Tracenf("[TERRAIN] Render: %s indexCount=%u debugIndexCount=%u layers=%zu camera eye=(%.2f,%.2f,%.2f) target=(%.2f,%.2f,%.2f)",
            m_mapLoaded ? "clean-room heightmap" : "flat 100m ground",
            m_indexCount,
            m_debugIndexCount,
            paletteLayers,
            camera.eye.x,
            camera.eye.y,
            camera.eye.z,
            camera.target.x,
            camera.target.y,
            camera.target.z);
        const uint32_t chunkSize = m_chunkSizeCells == 0 ? 64u : m_chunkSizeCells;
        const uint32_t chunksX = m_mapSizeX == 0 ? 0u : (m_mapSizeX + chunkSize - 1u) / chunkSize;
        const uint32_t chunksY = m_mapSizeY == 0 ? 0u : (m_mapSizeY + chunkSize - 1u) / chunkSize;
        Tracenf("[TCHUNK] render chunksDrawn=%u culled=0 chunkGrid=%ux%u policy=resident-all",
            chunksX * chunksY,
            chunksX,
            chunksY);
        loggedDraw = true;
    }
}

void TerrainRenderer::RenderSelectedWaterBodyHighlight(VulkanDevice& device, const WorldCamera& camera)
{
    if (!m_sceneTerrainActive || m_sceneTerrain.editorHidden || !m_mapEditorOpen || !m_pipeline || !m_selectedWaterBodyIndexCount ||
        !m_selectedWaterBodyVertexBuffer.buffer || !m_selectedWaterBodyIndexBuffer.buffer ||
        !device.IsFrameActive())
    {
        return;
    }

    const VkExtent2D extent = device.GetSwapchainExtent();
    if (extent.width == 0 || extent.height == 0)
        return;

    const uint32_t frameIndex = device.GetFrameIndex();
    UpdateUniform(frameIndex, camera);

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

    struct TerrainPushConstants
    {
        float layerParams[4];
    };
    TerrainPushConstants push{{1.0f, 1.0f, 7.0f, 0.0f}};
    vkCmdPushConstants(cmd, m_pipelineLayout,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(push), &push);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m_selectedWaterBodyVertexBuffer.buffer, &offset);
    vkCmdBindIndexBuffer(cmd, m_selectedWaterBodyIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
        0, 1, &m_descriptorSets[frameIndex], 0, nullptr);
    vkCmdDrawIndexed(cmd, m_selectedWaterBodyIndexCount, 1, 0, 0, 0);
}

void TerrainRenderer::RenderWater(VulkanDevice& device, const WorldCamera& camera, double timeSeconds)
{
    m_latestWaterTimeSeconds = timeSeconds;
    if (!m_sceneTerrainActive || m_sceneTerrain.editorHidden || m_waterBodies.empty() || !m_waterPipeline || !device.IsFrameActive())
        return;

    const VkExtent2D extent = device.GetSwapchainExtent();
    if (extent.width == 0 || extent.height == 0)
        return;

    const uint32_t frameIndex = device.GetFrameIndex();
    float reflectionTargetDistance = 0.0f;
    const WaterBodyGpu* reflectionTarget = FindClosestWaterBody(camera, &reflectionTargetDistance);
    if (timeSeconds - m_lastWaterDiagTimeSeconds >= 1.0)
    {
        if (reflectionTarget)
        {
            Tracenf("[WATER-OBJ] diag: bodies=%zu reflection_target=id=%u name=\"%s\" distance=%.1fm reflection=%s",
                m_waterBodies.size(),
                reflectionTarget->body.id,
                reflectionTarget->body.name.c_str(),
                reflectionTargetDistance,
                ResolveWaterConfig(reflectionTarget->body).reflectionEnabled ? "enabled" : "disabled");
        }
        else
        {
            Tracenf("[WATER-OBJ] diag: bodies=%zu reflection_target=<none>", m_waterBodies.size());
        }
        m_lastWaterDiagTimeSeconds = timeSeconds;
    }

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
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_waterPipeline);

    VkDeviceSize offset = 0;
    for (WaterBodyGpu& waterBody : m_waterBodies)
    {
        if (!ResolveWaterConfig(waterBody.body).enabled || !waterBody.indexCount ||
            !waterBody.vertexBuffer.buffer || !waterBody.indexBuffer.buffer ||
            !waterBody.descriptorSets[frameIndex])
        {
            continue;
        }
        const bool isReflectionTarget = (&waterBody == reflectionTarget);
        UpdateWaterBodyUniform(frameIndex, camera, timeSeconds, waterBody, isReflectionTarget);
        vkCmdBindVertexBuffers(cmd, 0, 1, &waterBody.vertexBuffer.buffer, &offset);
        vkCmdBindIndexBuffer(cmd, waterBody.indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_waterPipelineLayout,
            0, 1, &waterBody.descriptorSets[frameIndex], 0, nullptr);
        vkCmdDrawIndexed(cmd, waterBody.indexCount, 1, 0, 0, 0);
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
    const MapEditorToolMode previousMode = m_editorToolMode;
    const MapEditorTool previousTool = m_editorTool;
    m_editorToolMode = settings.toolMode;
    m_editorTerrainToolActive =
        settings.toolMode == MapEditorToolMode::Heightmap ||
        settings.toolMode == MapEditorToolMode::SplatPaint;
    m_editorTool = settings.tool;
    m_editorBrushRadiusMeters = std::clamp(settings.brushRadiusMeters, 0.5f, 50.0f);
    m_editorBrushStrength = std::clamp(settings.brushStrength, 0.1f, 5.0f);
    m_editorTextureSlot = std::min<std::uint32_t>(settings.textureSlot, 7u);
    m_editorPaintMode = settings.paintMode;
    if ((previousMode != m_editorToolMode || previousTool != m_editorTool) &&
        (m_editorToolMode == MapEditorToolMode::Heightmap ||
         m_editorToolMode == MapEditorToolMode::SplatPaint))
    {
        Tracenf("[TEDIT-DIAG] tool=%s activeTerrain=%p dims=%ux%u cells mapLoaded=%d heightBuffer=%p splatTarget=%p/%p",
            TeditToolModeName(m_editorToolMode),
            m_sceneTerrainActive ? static_cast<void*>(this) : nullptr,
            m_mapSizeX,
            m_mapSizeY,
            m_mapLoaded ? 1 : 0,
            m_heightCmGrid.empty() ? nullptr : static_cast<void*>(m_heightCmGrid.data()),
            m_splatABytes.empty() ? nullptr : static_cast<void*>(m_splatABytes.data()),
            m_splatBBytes.empty() ? nullptr : static_cast<void*>(m_splatBBytes.data()));
        Tracenf("[TEDIT-DIAG] tool terrain==created terrain ? %s",
            m_sceneTerrainActive ? "yes" : "no");
    }
}

void TerrainRenderer::SetWaterSculptBrush(bool visible,
                                          float worldX,
                                          float worldZ,
                                          float radiusMeters,
                                          bool addMode)
{
    m_waterSculptBrushVisible = visible;
    m_waterSculptBrushWorldX = worldX;
    m_waterSculptBrushWorldZ = worldZ;
    m_waterSculptBrushRadiusMeters = std::clamp(radiusMeters, 0.5f, 20.0f);
    m_waterSculptBrushAddMode = addMode;
}

std::vector<WaterBody> TerrainRenderer::GetWaterBodies() const
{
    std::vector<WaterBody> bodies;
    if (!m_sceneTerrainActive)
        return bodies;
    bodies.reserve(m_waterBodies.size());
    for (const WaterBodyGpu& waterBody : m_waterBodies)
        bodies.push_back(waterBody.body);
    return bodies;
}

bool TerrainRenderer::SetWaterBodies(VulkanDevice& device, const std::vector<WaterBody>& bodies)
{
    device.WaitIdle();
    DestroyWaterBodyResources();
    if (!m_sceneTerrainActive)
        return bodies.empty();

    std::vector<WaterBody> limited = bodies;
    if (limited.size() > kMaxWaterBodyDraws)
    {
        Tracenf("[WATER-OBJ] editor body count %zu exceeds renderer cap %u", limited.size(), kMaxWaterBodyDraws);
        limited.resize(kMaxWaterBodyDraws);
    }

    m_waterBodies.reserve(limited.size());
    for (WaterBody& body : limited)
    {
        WaterBodyGpu gpu;
        gpu.body = std::move(body);
        if (!CreateWaterBodyUniformBuffers(device, gpu) || !CreateWaterBodyMesh(device, gpu))
        {
            DestroyWaterBodyResources(gpu);
            Tracen("[WATER-OBJ] skipped invalid editor water body");
            continue;
        }
        m_waterBodies.push_back(std::move(gpu));
    }

    if (m_waterDescriptorPool)
        CreateWaterDescriptors();
    SetSelectedWaterBodyHighlight(device, m_selectedWaterBodyId);
    Tracenf("[WATER-OBJ] editor water bodies applied: %zu", m_waterBodies.size());
    return m_waterBodies.size() == limited.size();
}

void TerrainRenderer::SetWaterMaterials(const std::vector<std::pair<std::string, WaterMaterialData>>& materials)
{
    m_waterMaterials.clear();
    std::vector<std::string> textureSignatureParts;
    std::vector<std::string> edgeSignatureParts;
    for (const auto& material : materials)
    {
        if (!material.first.empty())
        {
            m_waterMaterials[material.first] = material.second;
            textureSignatureParts.push_back(material.first + "|" + material.second.normalMapA + "|" +
                material.second.normalMapB + "|" + material.second.diffuseMap);
            edgeSignatureParts.push_back(material.first + "|" +
                std::to_string(material.second.config.edgeFadeDistance) + "|" +
                std::to_string(static_cast<int>(material.second.config.edgeFadeCurve)));
        }
    }
    std::sort(textureSignatureParts.begin(), textureSignatureParts.end());
    std::sort(edgeSignatureParts.begin(), edgeSignatureParts.end());
    std::string textureSignature;
    for (const std::string& part : textureSignatureParts)
    {
        textureSignature += part;
        textureSignature.push_back('\n');
    }
    std::string edgeSignature;
    for (const std::string& part : edgeSignatureParts)
    {
        edgeSignature += part;
        edgeSignature.push_back('\n');
    }
    if (textureSignature != m_waterMaterialTextureSignature && m_deviceOwner && m_assets)
    {
        m_deviceOwner->WaitIdle();
        DestroyWaterMaterialTextureCache();
        m_waterMaterialTextureSignature = textureSignature;
        for (const auto& [id, material] : m_waterMaterials)
        {
            WaterMaterialTextureSet textureSet{};
            if (LoadWaterMaterialTextureSet(*m_deviceOwner, id, material, textureSet))
                m_waterMaterialTextures[id] = std::move(textureSet);
        }
        UpdateWaterDescriptors();
    }
    if (edgeSignature != m_waterMaterialEdgeSignature && m_deviceOwner)
    {
        m_deviceOwner->WaitIdle();
        m_waterMaterialEdgeSignature = edgeSignature;
        for (WaterBodyGpu& waterBody : m_waterBodies)
            CreateWaterBodyMesh(*m_deviceOwner, waterBody);
        Tracenf("[WATER-OBJ-6] Material edge fade updated: %zu water bodies rebuilt", m_waterBodies.size());
    }
    static bool logged = false;
    if (!logged)
    {
        Tracenf("[WATER-OBJ-4] material cache connected: %zu water materials", m_waterMaterials.size());
        logged = true;
    }
}

void TerrainRenderer::DestroyWaterMaterialTextureCache()
{
    for (auto& [id, textureSet] : m_waterMaterialTextures)
    {
        (void)id;
        DestroyTexture(textureSet.normalA);
        DestroyTexture(textureSet.normalB);
        DestroyTexture(textureSet.diffuse);
    }
    m_waterMaterialTextures.clear();
    m_waterMaterialTextureSignature.clear();
}

bool TerrainRenderer::LoadWaterMaterialTextureSet(VulkanDevice& device,
                                                  const std::string& id,
                                                  const WaterMaterialData& material,
                                                  WaterMaterialTextureSet& out)
{
    if (!m_assets)
        return false;

    const std::string normalAPath = !material.normalMapA.empty() ? material.normalMapA : material.normalMapB;
    const std::string normalBPath = !material.normalMapB.empty() ? material.normalMapB : normalAPath;
    const std::string diffusePath = material.diffuseMap;
    if (normalAPath.empty() && normalBPath.empty() && diffusePath.empty())
        return false;

    auto loadTexture = [&](const std::string& path, const std::string& label, Texture& texture, VkFormat format) -> bool {
        if (path.empty())
            return false;
        RgbaImage image{};
        if (!LoadAnyTerrainImage(*m_assets, path, image))
        {
            Tracenf("[WATER-MAT] failed to load %s texture for %s: %s",
                label.c_str(),
                id.c_str(),
                path.c_str());
            return false;
        }
        return UploadRgbaTexture2D(device, "watermat_" + id + "_" + label, image.width, image.height, image.pixels,
            VK_SAMPLER_ADDRESS_MODE_REPEAT, texture, format);
    };

    const bool loadedA = loadTexture(normalAPath, "normal_a", out.normalA, VK_FORMAT_R8G8B8A8_UNORM);
    const bool loadedB = loadTexture(normalBPath, "normal_b", out.normalB, VK_FORMAT_R8G8B8A8_UNORM);
    const bool loadedDiffuse = loadTexture(diffusePath, "diffuse", out.diffuse, VK_FORMAT_R8G8B8A8_SRGB);
    if (!loadedA && !loadedB && !loadedDiffuse)
        return false;
    if (!loadedA)
    {
        DestroyTexture(out.normalA);
        out.normalA = {};
    }
    if (!loadedB)
    {
        DestroyTexture(out.normalB);
        out.normalB = {};
    }
    out.normalAPath = loadedA ? normalAPath : std::string{};
    out.normalBPath = loadedB ? normalBPath : std::string{};
    out.diffusePath = loadedDiffuse ? diffusePath : std::string{};
    Tracenf("[WATER-OBJ-TEX] material id=%s textures: normal_a=%s normal_b=%s diffuse=%s",
        id.c_str(),
        out.normalAPath.empty() ? "<default>" : out.normalAPath.c_str(),
        out.normalBPath.empty() ? "<default>" : out.normalBPath.c_str(),
        out.diffusePath.empty() ? "<none>" : out.diffusePath.c_str());
    return true;
}

const TerrainRenderer::WaterMaterialTextureSet* TerrainRenderer::ResolveWaterMaterialTextures(const WaterBody& body) const
{
    if (!body.materialId.empty())
    {
        auto it = m_waterMaterialTextures.find(body.materialId);
        if (it != m_waterMaterialTextures.end())
            return &it->second;
    }
    return nullptr;
}

bool TerrainRenderer::SetSelectedWaterBodyHighlight(VulkanDevice& device, std::uint32_t selectedWaterBodyId)
{
    const WaterBody* selectedBody = nullptr;
    for (const WaterBodyGpu& waterBody : m_waterBodies)
    {
        if (waterBody.body.id == selectedWaterBodyId)
        {
            selectedBody = &waterBody.body;
            break;
        }
    }

    if (!selectedBody)
    {
        if (m_selectedWaterBodyIndexCount == 0 && m_selectedWaterBodyId == 0)
            return true;
        m_selectedWaterBodyId = 0;
        std::fill(std::begin(m_selectedWaterBodySignature), std::end(m_selectedWaterBodySignature), 0.0f);
        return RebuildSelectedWaterBodyHighlight(device, nullptr);
    }

    const float signature[5] = {
        selectedBody->bboxMin[0],
        selectedBody->bboxMin[1],
        selectedBody->bboxMax[0],
        selectedBody->bboxMax[1],
        selectedBody->waterLevelY,
    };
    if (m_selectedWaterBodyId == selectedBody->id &&
        std::equal(std::begin(signature), std::end(signature), std::begin(m_selectedWaterBodySignature),
            [](float a, float b) { return std::abs(a - b) < 0.001f; }))
    {
        return true;
    }

    m_selectedWaterBodyId = selectedBody->id;
    std::copy(std::begin(signature), std::end(signature), std::begin(m_selectedWaterBodySignature));
    return RebuildSelectedWaterBodyHighlight(device, selectedBody);
}

bool TerrainRenderer::RebuildSelectedWaterBodyHighlight(VulkanDevice& device, const WaterBody* body)
{
    device.WaitIdle();
    DestroyBuffer(m_selectedWaterBodyVertexBuffer);
    DestroyBuffer(m_selectedWaterBodyIndexBuffer);
    m_selectedWaterBodyIndexCount = 0;

    if (!body)
        return true;

    const float minX = std::min(body->bboxMin[0], body->bboxMax[0]);
    const float maxX = std::max(body->bboxMin[0], body->bboxMax[0]);
    const float minZ = std::min(body->bboxMin[1], body->bboxMax[1]);
    const float maxZ = std::max(body->bboxMin[1], body->bboxMax[1]);
    if ((maxX - minX) < 0.01f || (maxZ - minZ) < 0.01f)
        return true;

    constexpr float kLineThickness = 0.16f;
    constexpr float kLift = 0.08f;
    const float y = body->waterLevelY + kLift;

    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    vertices.reserve(16);
    indices.reserve(24);

    auto addQuad = [&](float x0, float z0, float x1, float z1) {
        const uint32_t base = static_cast<uint32_t>(vertices.size());
        const Vertex quad[4] = {
            {{x0, y, z0}, {0.0f, 0.0f}, {0.0f, 0.0f}},
            {{x1, y, z0}, {0.0f, 0.0f}, {0.0f, 0.0f}},
            {{x1, y, z1}, {0.0f, 0.0f}, {0.0f, 0.0f}},
            {{x0, y, z1}, {0.0f, 0.0f}, {0.0f, 0.0f}},
        };
        vertices.insert(vertices.end(), std::begin(quad), std::end(quad));
        indices.push_back(base + 0);
        indices.push_back(base + 1);
        indices.push_back(base + 2);
        indices.push_back(base + 0);
        indices.push_back(base + 2);
        indices.push_back(base + 3);
    };

    addQuad(minX, minZ - kLineThickness * 0.5f, maxX, minZ + kLineThickness * 0.5f);
    addQuad(minX, maxZ - kLineThickness * 0.5f, maxX, maxZ + kLineThickness * 0.5f);
    addQuad(minX - kLineThickness * 0.5f, minZ, minX + kLineThickness * 0.5f, maxZ);
    addQuad(maxX - kLineThickness * 0.5f, minZ, maxX + kLineThickness * 0.5f, maxZ);

    CreateHostVisibleBuffer(device, m_device, sizeof(Vertex) * vertices.size(),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertices.data(), m_selectedWaterBodyVertexBuffer);
    CreateHostVisibleBuffer(device, m_device, sizeof(uint32_t) * indices.size(),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indices.data(), m_selectedWaterBodyIndexBuffer);
    m_selectedWaterBodyIndexCount = static_cast<uint32_t>(indices.size());
    return m_selectedWaterBodyVertexBuffer.buffer && m_selectedWaterBodyIndexBuffer.buffer;
}

void TerrainRenderer::SetPaletteSlots(const std::array<MapEditorPaletteSlot, 8>& slots)
{
    m_paletteSlots = slots;
    m_materialParamsDirty = true;
}

bool TerrainRenderer::ApplyPaletteSlotChange(VulkanDevice& device, const MapEditorPaletteSlot& slot)
{
    if (slot.slot >= m_paletteSlots.size() || slot.texturePath.empty())
        return false;

    auto next = m_paletteSlots;
    next[slot.slot] = slot;
    if (!LoadTerrainPaletteFromPaths(device, next))
        return false;
    m_paletteSlots = next;
    return true;
}

bool TerrainRenderer::ApplyPaletteSlots(VulkanDevice& device, const std::array<MapEditorPaletteSlot, 8>& slots)
{
    return LoadTerrainPaletteFromPaths(device, slots);
}

bool TerrainRenderer::ApplyPaletteSlotParams(const MapEditorPaletteSlot& slot)
{
    if (slot.slot >= m_paletteSlots.size())
        return false;

    MapEditorPaletteSlot& dst = m_paletteSlots[slot.slot];
    dst.tilingScaleX = std::clamp(slot.tilingScaleX, 0.01f, 64.0f);
    dst.tilingScaleY = std::clamp(slot.tilingScaleY, 0.01f, 64.0f);
    dst.colorTint[0] = std::clamp(slot.colorTint[0], 0.0f, 8.0f);
    dst.colorTint[1] = std::clamp(slot.colorTint[1], 0.0f, 8.0f);
    dst.colorTint[2] = std::clamp(slot.colorTint[2], 0.0f, 8.0f);
    dst.normalStrength = std::clamp(slot.normalStrength, 0.0f, 4.0f);
    dst.roughnessStrength = std::clamp(slot.roughnessStrength, 0.0f, 4.0f);
    dst.metallicStrength = std::clamp(slot.metallicStrength, 0.0f, 1.0f);
    dst.aoStrength = std::clamp(slot.aoStrength, 0.0f, 1.0f);
    dst.uvOffset[0] = slot.uvOffset[0];
    dst.uvOffset[1] = slot.uvOffset[1];
    dst.uvRotationDegrees = slot.uvRotationDegrees;
    m_materialParamsDirty = true;
    Tracenf("[TMAT] layer=%u tiling=%.3f,%.3f normalStrength=%.3f roughness=%.3f (changed)",
        slot.slot,
        dst.tilingScaleX,
        dst.tilingScaleY,
        dst.normalStrength,
        dst.roughnessStrength);
    Tracenf("[TMAT] layer=%u tint=(%.3f,%.3f,%.3f) metallic=%.3f ao=%.3f uvOffset=(%.3f,%.3f) uvRot=%.3f (changed)",
        slot.slot,
        dst.colorTint[0],
        dst.colorTint[1],
        dst.colorTint[2],
        dst.metallicStrength,
        dst.aoStrength,
        dst.uvOffset[0],
        dst.uvOffset[1],
        dst.uvRotationDegrees);
    return true;
}

bool TerrainRenderer::SetTriplanarSettings(bool enabled, float sharpness, float slopeThreshold, float slopeTransition)
{
    if (!m_sceneTerrainActive)
        return false;

    const float clampedSharpness = std::clamp(sharpness, 1.0f, 16.0f);
    const float clampedSlopeThreshold = std::clamp(slopeThreshold, 0.0f, 1.0f);
    const float clampedSlopeTransition = std::clamp(slopeTransition, 0.001f, 1.0f);
    const bool changed = m_sceneTerrain.triplanarEnabled != enabled ||
        std::abs(m_sceneTerrain.triplanarSharpness - clampedSharpness) > 0.0001f ||
        std::abs(m_sceneTerrain.triplanarSlopeThreshold - clampedSlopeThreshold) > 0.0001f ||
        std::abs(m_sceneTerrain.triplanarSlopeTransition - clampedSlopeTransition) > 0.0001f;
    m_sceneTerrain.triplanarEnabled = enabled;
    m_sceneTerrain.triplanarSharpness = clampedSharpness;
    m_sceneTerrain.triplanarSlopeThreshold = clampedSlopeThreshold;
    m_sceneTerrain.triplanarSlopeTransition = clampedSlopeTransition;
    if (changed)
    {
        m_materialParamsDirty = true;
        m_triplanarParamsDirty = true;
        m_triPerfStaticLogged = false;
        Tracenf("[TRIPLANAR] enabled=%s scope=terrain sharpness=%.2f slopeThreshold=%.3f transition=%.3f",
            enabled ? "yes" : "no",
            clampedSharpness,
            clampedSlopeThreshold,
            clampedSlopeTransition);
        if (enabled)
            Tracen("[TRIPLANAR] sample mode active, layers=8");
    }
    return true;
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
        if (m_mapEditorOpen && m_editorTerrainToolActive && m_editorLmbHeld)
        {
            Tracenf("[TEDIT-DIAG] brush input tool=%s mouse=(%d,%d) activeTerrain=%p",
                TeditToolModeName(m_editorToolMode),
                event.x,
                event.y,
                m_sceneTerrainActive ? static_cast<void*>(this) : nullptr);
        }
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
            {
                m_editorLmbHeld = true;
                if (m_editorTerrainToolActive)
                {
                    Tracenf("[TEDIT-DIAG] brush input tool=%s mouse=(%d,%d) activeTerrain=%p",
                        TeditToolModeName(m_editorToolMode),
                        event.x,
                        event.y,
                        m_sceneTerrainActive ? static_cast<void*>(this) : nullptr);
                }
            }
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
    const bool editorBrushActive = m_walkabilityDebug || m_editorTerrainToolActive;
    if (m_mapEditorOpen && editorBrushActive && m_mapLoaded)
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
        SaveWorldPalette();
        SaveWaterBodies();
    }

    if (m_editorUndoRequested)
    {
        m_editorUndoRequested = false;
        UndoLastEditorStroke(device);
    }

    if (!editorBrushActive || !m_mapLoaded)
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
    DestroyShadowResources();
    DestroyWaterResources();

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
    DestroyBuffer(m_selectedWaterBodyVertexBuffer);
    DestroyBuffer(m_selectedWaterBodyIndexBuffer);
    for (Buffer& buffer : m_uniformBuffers)
        DestroyBuffer(buffer);
    for (Buffer& buffer : m_waterUniformBuffers)
        DestroyBuffer(buffer);
    DestroyTerrainLayers();
    DestroyTexture(m_baseTexture);
    DestroyTexture(m_normalTexture);
    DestroyTexture(m_aoTexture);
    DestroyTexture(m_roughnessTexture);
    DestroyTexture(m_metallicTexture);
    DestroyTexture(m_heightTexture);
    DestroyTexture(m_fallbackMask);
    DestroyTexture(m_splatA);
    DestroyTexture(m_splatB);
    DestroyWaterMaterialTextureCache();
    DestroyTexture(m_waterNormalSmall);
    DestroyTexture(m_waterNormalLarge);

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
    m_selectedWaterBodyIndexCount = 0;
    m_selectedWaterBodyId = 0;
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
    m_sceneTerrainActive = false;
    m_sceneTerrain = {};
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
    m_deviceOwner = nullptr;
    m_assets = nullptr;
}

bool TerrainRenderer::CreateBuffers(VulkanDevice& device)
{
    m_mapSizeX = 100;
    m_mapSizeY = 100;
    m_cellScaleMeters = 1.0f;
    m_heightCmGrid.assign(static_cast<size_t>(m_mapSizeX + 1u) * (m_mapSizeY + 1u), 0.0f);
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
    const uint32_t cellsX = std::max(1u, m_mapSizeX == 0 ? 100u : m_mapSizeX);
    const uint32_t cellsZ = std::max(1u, m_mapSizeY == 0 ? 100u : m_mapSizeY);
    const float cellSize = std::max(0.01f, m_cellScaleMeters);
    m_flatTerrainWidthMeters = static_cast<float>(cellsX) * cellSize;
    m_flatTerrainDepthMeters = static_cast<float>(cellsZ) * cellSize;
    m_heightGridWidth = cellsX + 1u;
    m_heightGridHeight = cellsZ + 1u;
    const float halfWidth = m_flatTerrainWidthMeters * 0.5f;
    const float halfDepth = m_flatTerrainDepthMeters * 0.5f;

    std::vector<Vertex> vertices;
    vertices.reserve(static_cast<size_t>(m_heightGridWidth) * m_heightGridHeight);
    for (uint32_t z = 0; z < m_heightGridHeight; ++z)
    {
        for (uint32_t x = 0; x < m_heightGridWidth; ++x)
        {
            const size_t index = static_cast<size_t>(z) * m_heightGridWidth + x;
            const float heightMeters = index < m_heightCmGrid.size() ? m_heightCmGrid[index] * 0.01f : 0.0f;
            const float px = static_cast<float>(x) * cellSize - halfWidth;
            const float pz = halfDepth - static_cast<float>(z) * cellSize;
            vertices.push_back({{px, heightMeters, pz},
                {static_cast<float>(x) / 10.0f, static_cast<float>(z) / 10.0f},
                {static_cast<float>(x) / static_cast<float>(cellsX), static_cast<float>(z) / static_cast<float>(cellsZ)}});
        }
    }

    std::vector<uint32_t> indices;
    indices.reserve(static_cast<size_t>(cellsX) * cellsZ * 6u);
    for (uint32_t z = 0; z < cellsZ; ++z)
    {
        for (uint32_t x = 0; x < cellsX; ++x)
        {
            const uint32_t i0 = z * m_heightGridWidth + x;
            const uint32_t i1 = i0 + 1u;
            const uint32_t i2 = i0 + m_heightGridWidth + 1u;
            const uint32_t i3 = i0 + m_heightGridWidth;
            indices.push_back(i0);
            indices.push_back(i1);
            indices.push_back(i2);
            indices.push_back(i0);
            indices.push_back(i2);
            indices.push_back(i3);
        }
    }
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
    Texture& out,
    VkFormat format)
{
    if (width == 0 || height == 0 || pixels.size() != static_cast<size_t>(width) * height * 4u)
        return false;

    DestroyTexture(out);
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    vkGetDeviceQueue(m_device, device.GetGraphicsQueueFamily(), 0, &graphicsQueue);

    CreateDeviceLocalImage(device, m_device, width, height, 1, format, out.image, out.memory);
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

    out.format = format;
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
    if (addressMode == VK_SAMPLER_ADDRESS_MODE_REPEAT && device.SupportsSamplerAnisotropy())
    {
        sampler.anisotropyEnable = VK_TRUE;
        sampler.maxAnisotropy = device.GetMaxSamplerAnisotropy();
    }
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
    VkFormat format,
    Texture& out)
{
    if (width == 0 || height == 0 || layers == 0 ||
        pixels.size() != static_cast<size_t>(width) * height * layers * 4u)
        return false;

    DestroyTexture(out);
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    vkGetDeviceQueue(m_device, device.GetGraphicsQueueFamily(), 0, &graphicsQueue);

    const bool srgbColor = format == VK_FORMAT_R8G8B8A8_SRGB;
    const bool normalMap = name.find("normal") != std::string::npos;
    const ArrayMipUpload mipUpload = BuildRgbaArrayMipUpload(width, height, layers, pixels, srgbColor, normalMap);
    CreateDeviceLocalImageArray(device, m_device, width, height, mipUpload.mipLevels, layers,
        format, out.image, out.memory);
    Buffer staging{};
    CreateHostVisibleBuffer(device, m_device, mipUpload.pixels.size(),
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT, mipUpload.pixels.data(), staging);

    VkCommandPool uploadPool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = BeginOneTimeCommands(m_device, device.GetGraphicsQueueFamily(), uploadPool);
    TransitionImageLayoutArray(cmd, out.image, mipUpload.mipLevels, layers, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(cmd, staging.buffer, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        static_cast<uint32_t>(mipUpload.regions.size()), mipUpload.regions.data());
    TransitionImageLayoutArray(cmd, out.image, mipUpload.mipLevels, layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EndOneTimeCommands(m_device, graphicsQueue, uploadPool, cmd);
    DestroyBuffer(staging);

    out.format = format;
    out.width = width;
    out.height = height;
    out.mipLevels = mipUpload.mipLevels;
    out.name = name;

    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = out.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    view.format = out.format;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = out.mipLevels;
    view.subresourceRange.layerCount = layers;
    VK_CHECK(vkCreateImageView(m_device, &view, nullptr, &out.view));

    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = out.mipLevels > 1 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    if (device.SupportsSamplerAnisotropy())
    {
        sampler.anisotropyEnable = VK_TRUE;
        sampler.maxAnisotropy = device.GetMaxSamplerAnisotropy();
    }
    sampler.mipLodBias = 0.0f;
    sampler.maxLod = static_cast<float>(out.mipLevels > 0 ? out.mipLevels - 1u : 0u);
    VK_CHECK(vkCreateSampler(m_device, &sampler, nullptr, &out.sampler));
    Tracenf("[TERRAIN-MIPS] generated array=%s size=%ux%u layers=%u mips=%u format=%s normalRenorm=%s sampler=trilinear aniso=%s",
        name.c_str(),
        width,
        height,
        layers,
        out.mipLevels,
        VkFormatName(format),
        normalMap ? "yes" : "no",
        sampler.anisotropyEnable ? "yes" : "no");
    return true;
}

bool TerrainRenderer::UploadR8TextureArray(VulkanDevice& device,
    const std::string& name,
    uint32_t width,
    uint32_t height,
    uint32_t layers,
    const std::vector<std::uint8_t>& pixels,
    Texture& out)
{
    if (width == 0 || height == 0 || layers == 0 ||
        pixels.size() != static_cast<size_t>(width) * height * layers)
        return false;

    DestroyTexture(out);
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    vkGetDeviceQueue(m_device, device.GetGraphicsQueueFamily(), 0, &graphicsQueue);

    const ArrayMipUpload mipUpload = BuildR8ArrayMipUpload(width, height, layers, pixels);
    CreateDeviceLocalImageArray(device, m_device, width, height, mipUpload.mipLevels, layers,
        VK_FORMAT_R8_UNORM, out.image, out.memory);
    Buffer staging{};
    CreateHostVisibleBuffer(device, m_device, mipUpload.pixels.size(),
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT, mipUpload.pixels.data(), staging);

    VkCommandPool uploadPool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = BeginOneTimeCommands(m_device, device.GetGraphicsQueueFamily(), uploadPool);
    TransitionImageLayoutArray(cmd, out.image, mipUpload.mipLevels, layers, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(cmd, staging.buffer, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        static_cast<uint32_t>(mipUpload.regions.size()), mipUpload.regions.data());
    TransitionImageLayoutArray(cmd, out.image, mipUpload.mipLevels, layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EndOneTimeCommands(m_device, graphicsQueue, uploadPool, cmd);
    DestroyBuffer(staging);

    out.format = VK_FORMAT_R8_UNORM;
    out.width = width;
    out.height = height;
    out.mipLevels = mipUpload.mipLevels;
    out.name = name;

    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = out.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    view.format = out.format;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = out.mipLevels;
    view.subresourceRange.layerCount = layers;
    VK_CHECK(vkCreateImageView(m_device, &view, nullptr, &out.view));

    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = out.mipLevels > 1 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    if (device.SupportsSamplerAnisotropy())
    {
        sampler.anisotropyEnable = VK_TRUE;
        sampler.maxAnisotropy = device.GetMaxSamplerAnisotropy();
    }
    sampler.mipLodBias = 0.0f;
    sampler.maxLod = static_cast<float>(out.mipLevels > 0 ? out.mipLevels - 1u : 0u);
    VK_CHECK(vkCreateSampler(m_device, &sampler, nullptr, &out.sampler));
    Tracenf("[TERRAIN-MIPS] generated array=%s size=%ux%u layers=%u mips=%u format=%s data=linear sampler=trilinear aniso=%s",
        name.c_str(),
        width,
        height,
        layers,
        out.mipLevels,
        VkFormatName(out.format),
        sampler.anisotropyEnable ? "yes" : "no");
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
        if (m_editorLmbHeld && m_editorTerrainToolActive)
        {
            Tracenf("[TEDIT-DIAG] brush raycast hit=no terrain=%p reason=invalid-bounds dims=%ux%u heightGrid=%ux%u",
                m_sceneTerrainActive ? static_cast<void*>(this) : nullptr,
                m_mapSizeX,
                m_mapSizeY,
                m_heightGridWidth,
                m_heightGridHeight);
        }
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
            if (m_editorLmbHeld && m_editorTerrainToolActive)
            {
                const float cellXf = (m_spawnLocalXcm + m_editorBrushLocalX * 100.0f) /
                    std::max(m_cellScaleMeters * 100.0f, 0.001f);
                const float cellYf = (m_spawnLocalYcm - m_editorBrushLocalZ * 100.0f) /
                    std::max(m_cellScaleMeters * 100.0f, 0.001f);
                Tracenf("[TEDIT-DIAG] brush raycast hit=yes terrain=%p worldPos=(%.2f,%.2f,%.2f) cell=(%u,%u)",
                    m_sceneTerrainActive ? static_cast<void*>(this) : nullptr,
                    m_editorBrushLocalX,
                    SampleHeightAt(m_editorBrushLocalX, m_editorBrushLocalZ),
                    m_editorBrushLocalZ,
                    static_cast<unsigned>(std::clamp(cellXf, 0.0f, static_cast<float>(m_heightGridWidth > 0 ? m_heightGridWidth - 1u : 0u))),
                    static_cast<unsigned>(std::clamp(cellYf, 0.0f, static_cast<float>(m_heightGridHeight > 0 ? m_heightGridHeight - 1u : 0u))));
            }
            return true;
        }

        previousT = t;
        previousDelta = delta;
    }

    m_editorBrushVisible = false;
    if (m_editorLmbHeld && m_editorTerrainToolActive)
    {
        Tracenf("[TEDIT-DIAG] brush raycast hit=no terrain=%p bounds=(%.2f,%.2f)-(%.2f,%.2f)",
            m_sceneTerrainActive ? static_cast<void*>(this) : nullptr,
            bounds.minX,
            bounds.minZ,
            bounds.maxX,
            bounds.maxZ);
    }
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
    const uint32_t chunksX = m_chunkSplatWidth > 0 ? (m_splatWidth + m_chunkSplatWidth - 1u) / m_chunkSplatWidth : 0;
    if (chunksX == 0)
        return;
    const uint32_t chunkX = std::min(sx / m_chunkSplatWidth, chunksX - 1u);
    const uint32_t chunksY = static_cast<uint32_t>((m_dirtyChunkTexels.size() + chunksX - 1u) / chunksX);
    const uint32_t chunkY = std::min(sy / m_chunkSplatHeight, chunksY - 1u);
    const size_t dirtyIndex = static_cast<size_t>(chunkY) * chunksX + chunkX;
    if (dirtyIndex < m_dirtyChunkTexels.size())
        ++m_dirtyChunkTexels[dirtyIndex];
}

void TerrainRenderer::ApplyEditorBrush(VulkanDevice& device, double deltaSeconds)
{
    (void)device;
    if (!m_editorBrushVisible || m_heightCmGrid.empty() || !m_vertexBuffer.memory ||
        m_heightGridWidth < 2 || m_heightGridHeight < 2 || m_chunkSizeCells == 0)
    {
        if (m_editorLmbHeld && m_editorTerrainToolActive)
        {
            Tracenf("[TEDIT-DIAG] %s apply skipped terrainId=%p brushVisible=%d heightBuffer=%p vertexBuffer=%p heightGrid=%ux%u chunkCells=%u",
                m_editorTool == MapEditorTool::Paint ? "splat" : "sculpt",
                m_sceneTerrainActive ? static_cast<void*>(this) : nullptr,
                m_editorBrushVisible ? 1 : 0,
                m_heightCmGrid.empty() ? nullptr : static_cast<void*>(m_heightCmGrid.data()),
                m_vertexBuffer.memory,
                m_heightGridWidth,
                m_heightGridHeight,
                m_chunkSizeCells);
        }
        return;
    }

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
    const float radiusCells = m_editorBrushRadiusMeters / std::max(m_cellScaleMeters, 0.001f);
    const float alphaCenter = std::clamp(m_editorBrushStrength * static_cast<float>(deltaSeconds), 0.0f, 1.0f);

    if (m_editorTool == MapEditorTool::Paint)
    {
        if (m_splatABytes.empty() || m_splatBBytes.empty() || m_splatWidth == 0 || m_splatHeight == 0)
        {
            Tracenf("[TEDIT-DIAG] splat apply skipped terrainId=%p splatTarget=%p/%p splatSize=%ux%u layer=%u",
                m_sceneTerrainActive ? static_cast<void*>(this) : nullptr,
                m_splatABytes.empty() ? nullptr : static_cast<void*>(m_splatABytes.data()),
                m_splatBBytes.empty() ? nullptr : static_cast<void*>(m_splatBBytes.data()),
                m_splatWidth,
                m_splatHeight,
                m_editorTextureSlot);
            return;
        }
        const float mapCellsX = std::max(static_cast<float>(m_mapSizeX), 1.0f);
        const float mapCellsY = std::max(static_cast<float>(m_mapSizeY), 1.0f);
        const float splatScaleX = static_cast<float>(m_splatWidth) / mapCellsX;
        const float splatScaleY = static_cast<float>(m_splatHeight) / mapCellsY;
        const float centerSplatX = std::clamp(centerGridX * splatScaleX, 0.0f, static_cast<float>(m_splatWidth - 1u));
        const float centerSplatY = std::clamp(centerGridY * splatScaleY, 0.0f, static_cast<float>(m_splatHeight - 1u));
        const float radiusSplatX = std::max(radiusCells * splatScaleX, 1.0f);
        const float radiusSplatY = std::max(radiusCells * splatScaleY, 1.0f);
        const uint32_t minX = static_cast<uint32_t>(std::max(0.0f, std::floor(centerSplatX - radiusSplatX)));
        const uint32_t minY = static_cast<uint32_t>(std::max(0.0f, std::floor(centerSplatY - radiusSplatY)));
        const uint32_t maxX = std::min(m_splatWidth - 1u, static_cast<uint32_t>(std::ceil(centerSplatX + radiusSplatX)));
        const uint32_t maxY = std::min(m_splatHeight - 1u, static_cast<uint32_t>(std::ceil(centerSplatY + radiusSplatY)));
        bool changed = false;
        std::uint32_t changedCells = 0;
        std::unordered_set<std::uint32_t> touchedChunks;
        for (uint32_t sy = minY; sy <= maxY; ++sy)
        {
            for (uint32_t sx = minX; sx <= maxX; ++sx)
            {
                const float dx = (static_cast<float>(sx) - centerSplatX) / std::max(splatScaleX, 0.001f) * m_cellScaleMeters;
                const float dy = (static_cast<float>(sy) - centerSplatY) / std::max(splatScaleY, 0.001f) * m_cellScaleMeters;
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
                if (m_editorPaintMode == MapEditorPaintMode::Replace)
                {
                    for (uint32_t i = 0; i < 8; ++i)
                    {
                        const float target = i == slot ? 1.0f : 0.0f;
                        weights[i] += (target - weights[i]) * alpha;
                    }
                }
                else
                {
                    weights[slot] += alpha;
                    float total = 0.0f;
                    for (float weight : weights)
                        total += weight;
                    if (total > 0.0001f)
                    {
                        for (float& weight : weights)
                            weight /= total;
                    }
                }

                float total = 0.0f;
                for (float weight : weights)
                    total += weight;
                if (total > 0.0001f)
                {
                    for (float& weight : weights)
                        weight = std::clamp(weight / total, 0.0f, 1.0f);
                }

                std::array<uint8_t, 8> quantized{};
                int quantizedTotal = 0;
                uint32_t strongest = 0;
                for (uint32_t i = 0; i < 8; ++i)
                {
                    quantized[i] = static_cast<uint8_t>(std::clamp(std::lround(weights[i] * 255.0f), 0l, 255l));
                    quantizedTotal += quantized[i];
                    if (quantized[i] > quantized[strongest])
                        strongest = i;
                }
                const int correction = 255 - quantizedTotal;
                quantized[strongest] = static_cast<uint8_t>(
                    std::clamp(static_cast<int>(quantized[strongest]) + correction, 0, 255));

                for (int i = 0; i < 4; ++i)
                    m_splatABytes[byte + i] = quantized[i];
                for (int i = 0; i < 4; ++i)
                    m_splatBBytes[byte + i] = quantized[4 + i];
                MarkSplatDirty(index);
                const std::uint32_t cx = m_chunkSplatWidth > 0 ? sx / m_chunkSplatWidth : 0;
                const std::uint32_t cy = m_chunkSplatHeight > 0 ? sy / m_chunkSplatHeight : 0;
                touchedChunks.insert((cy << 16u) | (cx & 0xffffu));
                changed = true;
                ++changedCells;
            }
        }
        m_editorSplatGpuDirty = m_editorSplatGpuDirty || changed;
        Tracenf("[TEDIT-DIAG] splat apply terrainId=%p splatTarget=%p/%p layer=%u cellRange=(%u,%u)-(%u,%u) maskUpdated=%s changedCells=%u gpuUpload=%s",
            m_sceneTerrainActive ? static_cast<void*>(this) : nullptr,
            m_splatABytes.empty() ? nullptr : static_cast<void*>(m_splatABytes.data()),
            m_splatBBytes.empty() ? nullptr : static_cast<void*>(m_splatBBytes.data()),
            m_editorTextureSlot,
            minX,
            minY,
            maxX,
            maxY,
            changed ? "yes" : "no",
            changedCells,
            m_editorSplatGpuDirty ? "pending" : "no");
        std::ostringstream chunksText;
        bool firstChunk = true;
        for (std::uint32_t packed : touchedChunks)
        {
            if (!firstChunk)
                chunksText << ";";
            firstChunk = false;
            chunksText << (packed & 0xffffu) << "," << (packed >> 16u);
        }
        Tracenf("[TCHUNK] splat stroke chunksTouched=[%s] dirty=%zu gpuUpload=%s remesh=%s",
            chunksText.str().c_str(),
            touchedChunks.size(),
            changed ? "yes" : "no",
            changed ? "yes" : "no");
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

    const uint32_t minX = static_cast<uint32_t>(std::max(0.0f, std::floor(centerGridX - radiusCells)));
    const uint32_t minY = static_cast<uint32_t>(std::max(0.0f, std::floor(centerGridY - radiusCells)));
    const uint32_t maxX = std::min(m_heightGridWidth - 1u, static_cast<uint32_t>(std::ceil(centerGridX + radiusCells)));
    const uint32_t maxY = std::min(m_heightGridHeight - 1u, static_cast<uint32_t>(std::ceil(centerGridY + radiusCells)));
    std::uint32_t changedHeights = 0;
    float maxHeightDeltaCm = 0.0f;
    std::unordered_set<std::uint32_t> touchedChunks;

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
            const float heightDeltaCm = newHeight - m_heightCmGrid[index];
            if (std::abs(heightDeltaCm) <= 0.001f)
                continue;
            if (std::abs(heightDeltaCm) > std::abs(maxHeightDeltaCm))
                maxHeightDeltaCm = heightDeltaCm;
            RecordHeightUndo(index);
            m_heightCmGrid[index] = newHeight;
            vertices[index].position[1] = newHeight * 0.01f;
            MarkHeightDirty(index);
            const std::uint32_t cx = m_chunkSizeCells > 0 ? std::min(gx / m_chunkSizeCells, chunksX - 1u) : 0;
            const std::uint32_t cy = m_chunkSizeCells > 0 ? std::min(gy / m_chunkSizeCells, chunksY - 1u) : 0;
            touchedChunks.insert((cy << 16u) | (cx & 0xffffu));
            ++changedHeights;
        }
    }
    vkUnmapMemory(m_device, m_vertexBuffer.memory);
    Tracenf("[TEDIT-DIAG] sculpt apply terrainId=%p tool=%s heightBuffer=%p vertexBuffer=%p cellRange=(%u,%u)-(%u,%u) changedCells=%u gpuUpload=%s remesh=no",
        m_sceneTerrainActive ? static_cast<void*>(this) : nullptr,
        TeditToolName(m_editorTool),
        m_heightCmGrid.empty() ? nullptr : static_cast<void*>(m_heightCmGrid.data()),
        m_vertexBuffer.memory,
        minX,
        minY,
        maxX,
        maxY,
        changedHeights,
        changedHeights > 0 ? "mapped-buffer-write" : "no");
    const uint32_t brushCellX = static_cast<uint32_t>(std::clamp(
        centerGridX,
        0.0f,
        static_cast<float>(m_heightGridWidth > 0 ? m_heightGridWidth - 1u : 0u)));
    const uint32_t brushCellY = static_cast<uint32_t>(std::clamp(
        centerGridY,
        0.0f,
        static_cast<float>(m_heightGridHeight > 0 ? m_heightGridHeight - 1u : 0u)));
    Tracenf("[SCULPT-DIAG] sculpt APPLY terrainId=%p worldPos=(%.2f,%.2f,%.2f) cell=(%u,%u) radius=%.2f strength=%.2f dir=%s cellsModified=%u heightDelta=%.4f gpuUpload=%s remesh=%s",
        m_sceneTerrainActive ? static_cast<void*>(this) : nullptr,
        m_editorBrushLocalX,
        SampleHeightAt(m_editorBrushLocalX, m_editorBrushLocalZ),
        m_editorBrushLocalZ,
        brushCellX,
        brushCellY,
        m_editorBrushRadiusMeters,
        m_editorBrushStrength,
        TeditToolName(m_editorTool),
        changedHeights,
        maxHeightDeltaCm * 0.01f,
        changedHeights > 0 ? "yes" : "no",
        changedHeights > 0 ? "yes" : "no");
    std::ostringstream chunksText;
    bool firstChunk = true;
    for (std::uint32_t packed : touchedChunks)
    {
        if (!firstChunk)
            chunksText << ";";
        firstChunk = false;
        chunksText << (packed & 0xffffu) << "," << (packed >> 16u);
    }
    Tracenf("[TCHUNK] sculpt stroke chunksTouched=[%s] dirty=%zu gpuUpload=%s remesh=%s",
        chunksText.str().c_str(),
        touchedChunks.size(),
        changedHeights > 0 ? "yes" : "no",
        changedHeights > 0 ? "yes" : "no");
}

bool TerrainRenderer::RefreshSplatTextures(VulkanDevice& device)
{
    if (m_splatABytes.empty() || m_splatBBytes.empty())
        return false;
    const bool okA = UpdateRgbaTexture2D(device, m_splatA, m_splatABytes);
    const bool okB = UpdateRgbaTexture2D(device, m_splatB, m_splatBBytes);
    m_editorSplatGpuDirty = !(okA && okB);
    Tracenf("[TEDIT-DIAG] splat gpu upload terrainId=%p targetImages=%p/%p textureSize=%ux%u cpuSize=%ux%u okA=%d okB=%d gpuUpload=%s",
        m_sceneTerrainActive ? static_cast<void*>(this) : nullptr,
        m_splatA.image,
        m_splatB.image,
        m_splatA.width,
        m_splatA.height,
        m_splatWidth,
        m_splatHeight,
        okA ? 1 : 0,
        okB ? 1 : 0,
        (okA && okB) ? "yes" : "no");
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

bool TerrainRenderer::SaveWorldPalette() const
{
    if (m_loadedMapDirectory.empty())
        return false;

    const std::filesystem::path path = ResolveWritableMapPath(m_loadedMapDirectory + "/world_palette.json");
    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            Tracenf("[TERRAIN-PALETTE] save failed; cannot open %s", tmp.string().c_str());
            return false;
        }
        out << "{\n  \"version\": 1,\n  \"slots\": [\n";
        for (size_t i = 0; i < m_paletteSlots.size(); ++i)
        {
            const auto& slot = m_paletteSlots[i];
            auto escape = [](const std::string& value) {
                std::string out;
                for (char c : value)
                {
                    if (c == '\\') out += "\\\\";
                    else if (c == '"') out += "\\\"";
                    else out += c;
                }
                return out;
            };
            out << "    { \"slot\": " << i
                << ", \"asset_id\": \"" << escape(slot.assetId) << "\""
                << ", \"display_name\": \"" << escape(slot.displayName) << "\""
                << ", \"texture_path\": \"" << escape(slot.texturePath) << "\""
                << ", \"normal_texture_path\": \"" << escape(slot.normalTexturePath) << "\""
                << ", \"tiling_scale_x\": " << slot.tilingScaleX
                << ", \"tiling_scale_y\": " << slot.tilingScaleY
                << ", \"tint_r\": " << slot.colorTint[0]
                << ", \"tint_g\": " << slot.colorTint[1]
                << ", \"tint_b\": " << slot.colorTint[2]
                << ", \"normal_strength\": " << slot.normalStrength
                << ", \"ao_strength\": " << slot.aoStrength
                << ", \"roughness_strength\": " << slot.roughnessStrength
                << ", \"metallic_strength\": " << slot.metallicStrength
                << ", \"uv_offset_x\": " << slot.uvOffset[0]
                << ", \"uv_offset_y\": " << slot.uvOffset[1]
                << ", \"uv_rotation_degrees\": " << slot.uvRotationDegrees << " }"
                << (i + 1 < m_paletteSlots.size() ? "," : "") << "\n";
        }
        out << "  ]\n}\n";
    }
    if (!AtomicReplace(tmp, path))
        return false;
    Tracenf("[TERRAIN-PALETTE] saved %s", path.string().c_str());
    return true;
}

bool TerrainRenderer::SaveWaterBodies() const
{
    if (m_loadedMapDirectory.empty())
        return false;

    std::vector<WaterBody> bodies;
    bodies.reserve(m_waterBodies.size());
    for (const WaterBodyGpu& waterBody : m_waterBodies)
        bodies.push_back(waterBody.body);

    const std::filesystem::path path =
        ResolveWritableMapPath(m_loadedMapDirectory + "/" + client::render::kWaterBodiesFilename);
    const std::filesystem::path tmp = path.string() + ".tmp";
    std::string error;
    if (!client::render::SaveWaterBodiesBinary(tmp, bodies, &error))
    {
        Tracenf("[WATER-OBJ] save failed: %s", error.c_str());
        return false;
    }
    if (!AtomicReplace(tmp, path))
        return false;
    Tracenf("[WATER-OBJ] saved %zu water bodies to %s", bodies.size(), path.string().c_str());
    return true;
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
    std::vector<uint8_t> normals(static_cast<size_t>(kSize) * kSize * kLayers * 4u);
    for (size_t i = 0; i + 3 < normals.size(); i += 4)
    {
        normals[i + 0] = 128;
        normals[i + 1] = 128;
        normals[i + 2] = 255;
        normals[i + 3] = 255;
    }
    std::vector<uint8_t> ao(static_cast<size_t>(kSize) * kSize * kLayers, 255);
    std::vector<uint8_t> roughness(static_cast<size_t>(kSize) * kSize * kLayers, 128);
    std::vector<uint8_t> metallic(static_cast<size_t>(kSize) * kSize * kLayers, 0);
    std::vector<uint8_t> height(static_cast<size_t>(kSize) * kSize * kLayers, 0);
    return UploadRgbaTextureArray(device, "terrain_palette_fallback", kSize, kSize, kLayers, pixels,
               VK_FORMAT_R8G8B8A8_SRGB, m_baseTexture) &&
           UploadRgbaTextureArray(device, "terrain_normal_fallback", kSize, kSize, kLayers, normals,
               VK_FORMAT_R8G8B8A8_UNORM, m_normalTexture) &&
           UploadR8TextureArray(device, "terrain_ao_fallback", kSize, kSize, kLayers, ao, m_aoTexture) &&
           UploadR8TextureArray(device, "terrain_roughness_fallback", kSize, kSize, kLayers, roughness, m_roughnessTexture) &&
           UploadR8TextureArray(device, "terrain_metallic_fallback", kSize, kSize, kLayers, metallic, m_metallicTexture) &&
           UploadR8TextureArray(device, "terrain_height_fallback", kSize, kSize, kLayers, height, m_heightTexture);
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

bool TerrainRenderer::CreateSceneSplatTextures(VulkanDevice& device)
{
    if (m_splatWidth == 0 || m_splatHeight == 0 ||
        m_splatABytes.size() != static_cast<size_t>(m_splatWidth) * m_splatHeight * 4u ||
        m_splatBBytes.size() != static_cast<size_t>(m_splatWidth) * m_splatHeight * 4u)
    {
        return false;
    }

    const bool okA = UploadRgbaTexture2D(device, "splat_a_scene", m_splatWidth, m_splatHeight, m_splatABytes,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, m_splatA);
    const bool okB = UploadRgbaTexture2D(device, "splat_b_scene", m_splatWidth, m_splatHeight, m_splatBBytes,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, m_splatB);
    Tracenf("[TERRAIN-SPLAT] scene splat textures created size=%ux%u okA=%d okB=%d",
        m_splatWidth,
        m_splatHeight,
        okA ? 1 : 0,
        okB ? 1 : 0);
    return okA && okB;
}

bool TerrainRenderer::LoadTerrainPalette(VulkanDevice& device,
    const mx::map::Manifest& manifest,
    const std::string& mapDirectory)
{
    if (!m_assets || manifest.texture_palette_paths.size() < 8)
        return false;

    std::array<MapEditorPaletteSlot, 8> slots{};
    for (uint32_t i = 0; i < slots.size(); ++i)
    {
        std::string path = manifest.texture_palette_paths[i];
        if (!path.empty() && path.rfind("assets/", 0) != 0 && path.find(':') == std::string::npos)
            path = mapDirectory + "/" + path;
        slots[i] = MapEditorPaletteSlot{i, {}, "Default " + std::to_string(i + 1u), path};
        if (i < manifest.texture_palette_tiling_x.size())
            slots[i].tilingScaleX = manifest.texture_palette_tiling_x[i];
        if (i < manifest.texture_palette_tiling_y.size())
            slots[i].tilingScaleY = manifest.texture_palette_tiling_y[i];
        if (i < manifest.texture_palette_normal_strength.size())
            slots[i].normalStrength = manifest.texture_palette_normal_strength[i];
        if (i < manifest.texture_palette_roughness_strength.size())
            slots[i].roughnessStrength = manifest.texture_palette_roughness_strength[i];
        if (i < manifest.texture_palette_tint_r.size())
            slots[i].colorTint[0] = manifest.texture_palette_tint_r[i];
        if (i < manifest.texture_palette_tint_g.size())
            slots[i].colorTint[1] = manifest.texture_palette_tint_g[i];
        if (i < manifest.texture_palette_tint_b.size())
            slots[i].colorTint[2] = manifest.texture_palette_tint_b[i];
        if (i < manifest.texture_palette_metallic_strength.size())
            slots[i].metallicStrength = manifest.texture_palette_metallic_strength[i];
        if (i < manifest.texture_palette_ao_strength.size())
            slots[i].aoStrength = manifest.texture_palette_ao_strength[i];
        if (i < manifest.texture_palette_uv_offset_x.size())
            slots[i].uvOffset[0] = manifest.texture_palette_uv_offset_x[i];
        if (i < manifest.texture_palette_uv_offset_y.size())
            slots[i].uvOffset[1] = manifest.texture_palette_uv_offset_y[i];
        if (i < manifest.texture_palette_uv_rotation_degrees.size())
            slots[i].uvRotationDegrees = manifest.texture_palette_uv_rotation_degrees[i];
    }

    return LoadTerrainPaletteFromPaths(device, slots);
}

bool TerrainRenderer::LoadTerrainPaletteFromPaths(VulkanDevice& device, const std::array<MapEditorPaletteSlot, 8>& slots)
{
    if (!m_assets)
        return false;

    std::array<RgbaImage, 8> images{};
    std::array<RgbaImage, 8> normalImages{};
    std::array<RgbaImage, 8> aoImages{};
    std::array<RgbaImage, 8> roughnessImages{};
    std::array<RgbaImage, 8> metallicImages{};
    std::array<RgbaImage, 8> heightImages{};
    uint32_t width = 0;
    uint32_t height = 0;
    for (uint32_t i = 0; i < images.size(); ++i)
    {
        if (slots[i].texturePath.empty() || !LoadAnyTerrainImage(*m_assets, slots[i].texturePath, images[i], &m_additionalAssetRoots))
        {
            Tracenf("[TERRAIN-PALETTE] using default diffuse for layer %u path=%s", i, slots[i].texturePath.c_str());
        }
    }
    for (const RgbaImage& image : images)
    {
        if (image.width * image.height > width * height)
        {
            width = image.width;
            height = image.height;
        }
    }
    if (width == 0 || height == 0)
    {
        width = 4;
        height = 4;
    }

    auto fillRgba = [width, height](RgbaImage& image, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        image.width = width;
        image.height = height;
        image.pixels.assign(static_cast<size_t>(width) * height * 4u, 255);
        for (size_t p = 0; p + 3 < image.pixels.size(); p += 4)
        {
            image.pixels[p + 0] = r;
            image.pixels[p + 1] = g;
            image.pixels[p + 2] = b;
            image.pixels[p + 3] = a;
        }
    };

    for (uint32_t i = 0; i < images.size(); ++i)
    {
        if (images[i].width == 0 || images[i].height == 0)
        {
            const uint8_t tone = static_cast<uint8_t>(96u + i * 14u);
            fillRgba(images[i], tone, tone, tone, 255);
        }
        else
        {
            images[i] = ResizeNearest(images[i], width, height);
        }
    }

    for (uint32_t i = 0; i < normalImages.size(); ++i)
    {
        if (!slots[i].normalTexturePath.empty() &&
            LoadAnyTerrainImage(*m_assets, slots[i].normalTexturePath, normalImages[i], &m_additionalAssetRoots))
        {
            normalImages[i] = ResizeNearest(normalImages[i], width, height);
            continue;
        }
        normalImages[i].width = width;
        normalImages[i].height = height;
        normalImages[i].pixels.assign(static_cast<size_t>(width) * height * 4u, 255);
        for (size_t p = 0; p + 3 < normalImages[i].pixels.size(); p += 4)
        {
            normalImages[i].pixels[p + 0] = 128;
            normalImages[i].pixels[p + 1] = 128;
            normalImages[i].pixels[p + 2] = 255;
            normalImages[i].pixels[p + 3] = 255;
        }
    }
    auto loadSingleChannelOrDefault = [&](const std::string& path, RgbaImage& image, uint8_t defaultValue) {
        if (!path.empty() && LoadAnyTerrainImage(*m_assets, path, image, &m_additionalAssetRoots))
        {
            image = ResizeNearest(image, width, height);
            return;
        }
        fillRgba(image, defaultValue, defaultValue, defaultValue, 255);
    };

    for (uint32_t i = 0; i < images.size(); ++i)
    {
        loadSingleChannelOrDefault(slots[i].aoTexturePath, aoImages[i], 255);
        loadSingleChannelOrDefault(slots[i].roughnessTexturePath, roughnessImages[i], 128);
        loadSingleChannelOrDefault(slots[i].metallicTexturePath, metallicImages[i], 0);
        loadSingleChannelOrDefault(slots[i].heightTexturePath, heightImages[i], 0);
    }

    std::vector<uint8_t> pixels;
    std::vector<uint8_t> normalPixels;
    std::vector<uint8_t> aoPixels;
    std::vector<uint8_t> roughnessPixels;
    std::vector<uint8_t> metallicPixels;
    std::vector<uint8_t> heightPixels;
    pixels.reserve(static_cast<size_t>(width) * height * images.size() * 4u);
    normalPixels.reserve(static_cast<size_t>(width) * height * images.size() * 4u);
    aoPixels.reserve(static_cast<size_t>(width) * height * images.size());
    roughnessPixels.reserve(static_cast<size_t>(width) * height * images.size());
    metallicPixels.reserve(static_cast<size_t>(width) * height * images.size());
    heightPixels.reserve(static_cast<size_t>(width) * height * images.size());
    for (uint32_t layer = 0; layer < images.size(); ++layer)
    {
        pixels.insert(pixels.end(), images[layer].pixels.begin(), images[layer].pixels.end());
        normalPixels.insert(normalPixels.end(), normalImages[layer].pixels.begin(), normalImages[layer].pixels.end());
        const std::vector<uint8_t> ao = ExtractR8Channel(aoImages[layer]);
        const std::vector<uint8_t> roughness = ExtractR8Channel(roughnessImages[layer]);
        const std::vector<uint8_t> metallic = ExtractR8Channel(metallicImages[layer]);
        const std::vector<uint8_t> heightLayer = ExtractR8Channel(heightImages[layer]);
        aoPixels.insert(aoPixels.end(), ao.begin(), ao.end());
        roughnessPixels.insert(roughnessPixels.end(), roughness.begin(), roughness.end());
        metallicPixels.insert(metallicPixels.end(), metallic.begin(), metallic.end());
        heightPixels.insert(heightPixels.end(), heightLayer.begin(), heightLayer.end());
    }

    Texture palette{};
    Texture normals{};
    Texture ao{};
    Texture roughness{};
    Texture metallic{};
    Texture heightTex{};
    if (!UploadRgbaTextureArray(device, "terrain_palette", width, height, static_cast<uint32_t>(images.size()), pixels,
            VK_FORMAT_R8G8B8A8_SRGB, palette) ||
        !UploadRgbaTextureArray(device, "terrain_normals", width, height, static_cast<uint32_t>(images.size()), normalPixels,
            VK_FORMAT_R8G8B8A8_UNORM, normals) ||
        !UploadR8TextureArray(device, "terrain_ao", width, height, static_cast<uint32_t>(images.size()), aoPixels, ao) ||
        !UploadR8TextureArray(device, "terrain_roughness", width, height, static_cast<uint32_t>(images.size()), roughnessPixels, roughness) ||
        !UploadR8TextureArray(device, "terrain_metallic", width, height, static_cast<uint32_t>(images.size()), metallicPixels, metallic) ||
        !UploadR8TextureArray(device, "terrain_height", width, height, static_cast<uint32_t>(images.size()), heightPixels, heightTex))
    {
        DestroyTexture(palette);
        DestroyTexture(normals);
        DestroyTexture(ao);
        DestroyTexture(roughness);
        DestroyTexture(metallic);
        DestroyTexture(heightTex);
        return false;
    }

    DestroyTexture(m_baseTexture);
    DestroyTexture(m_normalTexture);
    DestroyTexture(m_aoTexture);
    DestroyTexture(m_roughnessTexture);
    DestroyTexture(m_metallicTexture);
    DestroyTexture(m_heightTexture);
    m_baseTexture = palette;
    m_normalTexture = normals;
    m_aoTexture = ao;
    m_roughnessTexture = roughness;
    m_metallicTexture = metallic;
    m_heightTexture = heightTex;
    m_paletteSlots = slots;
    m_materialParamsDirty = true;
    UpdateDescriptors();
    Tracenf("[TERRAIN-PALETTE] loaded 8-layer PBR palette size=%ux%u diffuse=%s normal=%s orm_height=R8",
        m_baseTexture.width,
        m_baseTexture.height,
        VkFormatName(m_baseTexture.format),
        VkFormatName(m_normalTexture.format));
    if (!m_triPerfPaletteLogged)
    {
        Tracenf("[TRI-PERF] palette size=%ux%u layers=%zu format(diffuse=%s normal=%s ao=%s roughness=%s metallic=%s height=%s) mips(diffuse=%u normal=%u ao=%u roughness=%u metallic=%u height=%u)",
            m_baseTexture.width,
            m_baseTexture.height,
            images.size(),
            VkFormatName(m_baseTexture.format),
            VkFormatName(m_normalTexture.format),
            VkFormatName(m_aoTexture.format),
            VkFormatName(m_roughnessTexture.format),
            VkFormatName(m_metallicTexture.format),
            VkFormatName(m_heightTexture.format),
            m_baseTexture.mipLevels,
            m_normalTexture.mipLevels,
            m_aoTexture.mipLevels,
            m_roughnessTexture.mipLevels,
            m_metallicTexture.mipLevels,
            m_heightTexture.mipLevels);
        m_triPerfPaletteLogged = true;
    }
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
    if (device.SupportsSamplerAnisotropy())
    {
        sampler.anisotropyEnable = VK_TRUE;
        sampler.maxAnisotropy = device.GetMaxSamplerAnisotropy();
    }
    sampler.mipLodBias = -0.5f;
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
        if (device.SupportsSamplerAnisotropy())
        {
            sampler.anisotropyEnable = VK_TRUE;
            sampler.maxAnisotropy = device.GetMaxSamplerAnisotropy();
        }
        sampler.mipLodBias = -0.5f;
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
        ubo.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

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

        VkDescriptorSetLayoutBinding normals{};
        normals.binding = 4;
        normals.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        normals.descriptorCount = 1;
        normals.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutBinding ao{};
        ao.binding = 5;
        ao.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ao.descriptorCount = 1;
        ao.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutBinding roughness{};
        roughness.binding = 6;
        roughness.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        roughness.descriptorCount = 1;
        roughness.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutBinding metallic{};
        metallic.binding = 7;
        metallic.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        metallic.descriptorCount = 1;
        metallic.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutBinding height{};
        height.binding = 8;
        height.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        height.descriptorCount = 1;
        height.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutBinding shadow{};
        shadow.binding = 9;
        shadow.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        shadow.descriptorCount = 1;
        shadow.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo layout{};
        layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        std::array<VkDescriptorSetLayoutBinding, 10> bindings = {
            ubo, palette, splatA, splatB, normals, ao, roughness, metallic, height, shadow
        };
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
    poolSizes[1].descriptorCount = descriptorSetCount * 9u;

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

bool TerrainRenderer::CreateShadowResources(VulkanDevice& device)
{
    if (m_shadowImage)
        return true;

    constexpr VkFormat kShadowFormat = VK_FORMAT_D32_SFLOAT;
    CreateDepthImageArray(device, m_device, kShadowResolution, kShadowResolution,
        kShadowCascadeCount, kShadowFormat, m_shadowImage, m_shadowMemory);

    VkImageViewCreateInfo arrayView{};
    arrayView.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    arrayView.image = m_shadowImage;
    arrayView.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    arrayView.format = kShadowFormat;
    arrayView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    arrayView.subresourceRange.levelCount = 1;
    arrayView.subresourceRange.layerCount = kShadowCascadeCount;
    VK_CHECK(vkCreateImageView(m_device, &arrayView, nullptr, &m_shadowArrayView));

    for (uint32_t i = 0; i < kShadowCascadeCount; ++i)
    {
        VkImageViewCreateInfo layerView = arrayView;
        layerView.viewType = VK_IMAGE_VIEW_TYPE_2D;
        layerView.subresourceRange.baseArrayLayer = i;
        layerView.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(m_device, &layerView, nullptr, &m_shadowLayerViews[i]));
    }

    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampler.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    sampler.compareEnable = VK_TRUE;
    sampler.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    sampler.minLod = 0.0f;
    sampler.maxLod = 0.0f;
    VK_CHECK(vkCreateSampler(m_device, &sampler, nullptr, &m_shadowSampler));

    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = kShadowFormat;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthRef{};
    depthRef.attachment = 0;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.pDepthStencilAttachment = &depthRef;

    VkRenderPassCreateInfo renderPass{};
    renderPass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPass.attachmentCount = 1;
    renderPass.pAttachments = &depthAttachment;
    renderPass.subpassCount = 1;
    renderPass.pSubpasses = &subpass;
    VK_CHECK(vkCreateRenderPass(m_device, &renderPass, nullptr, &m_shadowRenderPass));

    for (uint32_t i = 0; i < kShadowCascadeCount; ++i)
    {
        VkFramebufferCreateInfo fb{};
        fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass = m_shadowRenderPass;
        fb.attachmentCount = 1;
        fb.pAttachments = &m_shadowLayerViews[i];
        fb.width = kShadowResolution;
        fb.height = kShadowResolution;
        fb.layers = 1;
        VK_CHECK(vkCreateFramebuffer(m_device, &fb, nullptr, &m_shadowFramebuffers[i]));
    }

    if (!CreateShadowPipeline())
        return false;

    Tracen("[SHADOW] Cascade Shadow Maps: 4 cascades x 2048x2048 D32_SFLOAT, PCF 5x5");
    return true;
}

void TerrainRenderer::UpdateDescriptors()
{
    if (!m_descriptorPool)
        return;

    auto writeSet = [this](VkDescriptorSet descriptorSet, uint32_t frame)
    {
        if (!descriptorSet || !m_baseTexture.view || !m_baseTexture.sampler ||
            !m_normalTexture.view || !m_normalTexture.sampler ||
            !m_aoTexture.view || !m_aoTexture.sampler ||
            !m_roughnessTexture.view || !m_roughnessTexture.sampler ||
            !m_metallicTexture.view || !m_metallicTexture.sampler ||
            !m_heightTexture.view || !m_heightTexture.sampler ||
            !m_shadowArrayView || !m_shadowSampler ||
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

        VkDescriptorImageInfo normalInfo{};
        normalInfo.sampler = m_normalTexture.sampler;
        normalInfo.imageView = m_normalTexture.view;
        normalInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo aoInfo{};
        aoInfo.sampler = m_aoTexture.sampler;
        aoInfo.imageView = m_aoTexture.view;
        aoInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo roughnessInfo{};
        roughnessInfo.sampler = m_roughnessTexture.sampler;
        roughnessInfo.imageView = m_roughnessTexture.view;
        roughnessInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo metallicInfo{};
        metallicInfo.sampler = m_metallicTexture.sampler;
        metallicInfo.imageView = m_metallicTexture.view;
        metallicInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo heightInfo{};
        heightInfo.sampler = m_heightTexture.sampler;
        heightInfo.imageView = m_heightTexture.view;
        heightInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo shadowInfo{};
        shadowInfo.sampler = m_shadowSampler;
        shadowInfo.imageView = m_shadowArrayView;
        shadowInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        std::array<VkWriteDescriptorSet, 10> writes{};
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

        writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[4].dstSet = descriptorSet;
        writes[4].dstBinding = 4;
        writes[4].descriptorCount = 1;
        writes[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[4].pImageInfo = &normalInfo;

        writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[5].dstSet = descriptorSet;
        writes[5].dstBinding = 5;
        writes[5].descriptorCount = 1;
        writes[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[5].pImageInfo = &aoInfo;

        writes[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[6].dstSet = descriptorSet;
        writes[6].dstBinding = 6;
        writes[6].descriptorCount = 1;
        writes[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[6].pImageInfo = &roughnessInfo;

        writes[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[7].dstSet = descriptorSet;
        writes[7].dstBinding = 7;
        writes[7].descriptorCount = 1;
        writes[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[7].pImageInfo = &metallicInfo;

        writes[8].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[8].dstSet = descriptorSet;
        writes[8].dstBinding = 8;
        writes[8].descriptorCount = 1;
        writes[8].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[8].pImageInfo = &heightInfo;

        writes[9].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[9].dstSet = descriptorSet;
        writes[9].dstBinding = 9;
        writes[9].descriptorCount = 1;
        writes[9].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[9].pImageInfo = &shadowInfo;

        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    };

    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
        writeSet(m_descriptorSets[frame], frame);
}

bool TerrainRenderer::CreateWaterResources(VulkanDevice& device)
{
    for (Buffer& buffer : m_waterUniformBuffers)
    {
        if (!buffer.buffer || !buffer.memory)
        {
            DestroyBuffer(buffer);
            CreateHostVisibleBuffer(device, m_device, sizeof(WaterUniformBlock),
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, nullptr, buffer);
        }
    }

    const bool normals = CreateWaterNormalTextures(device);
    const bool reflectionResources = normals ? CreateOrRecreateWaterReflectionResources(device, true) : false;
    const bool descriptors = reflectionResources ? CreateWaterDescriptors() : false;
    const bool reflectionPipeline = descriptors ? CreateWaterReflectionPipeline(device) : false;
    const bool pipeline = reflectionPipeline ? CreateWaterPipeline(device) : false;
    Tracen("[WATER-OBJ] Water body renderer resources ready");
    return normals && reflectionResources && descriptors && reflectionPipeline && pipeline;
}

bool TerrainRenderer::LoadWaterBodies(VulkanDevice& device, const std::string& mapDirectory)
{
    DestroyWaterBodyResources();

    if (!m_assets)
        return false;

    std::string normalizedRoot = mapDirectory;
    std::replace(normalizedRoot.begin(), normalizedRoot.end(), '\\', '/');
    while (!normalizedRoot.empty() && normalizedRoot.back() == '/')
        normalizedRoot.pop_back();
    const std::string waterPath = normalizedRoot + "/" + client::render::kWaterBodiesFilename;
    const auto bytes = m_assets->ReadAll(waterPath);
    if (!bytes)
    {
        Tracen("[WATER-OBJ] no water body file; map starts without water bodies");
        if (m_waterDescriptorPool)
            CreateWaterDescriptors();
        return true;
    }

    std::vector<WaterBody> bodies;
    std::string error;
    if (!client::render::LoadWaterBodiesBinary(*bytes, bodies, &error))
    {
        Tracenf("[WATER-OBJ] failed to parse %s: %s", waterPath.c_str(), error.c_str());
        if (m_waterDescriptorPool)
            CreateWaterDescriptors();
        return false;
    }

    if (bodies.size() > kMaxWaterBodyDraws)
    {
        Tracenf("[WATER-OBJ] water body count %zu exceeds renderer cap %u", bodies.size(), kMaxWaterBodyDraws);
        bodies.resize(kMaxWaterBodyDraws);
    }

    m_waterBodies.reserve(bodies.size());
    for (WaterBody& body : bodies)
    {
        WaterBodyGpu gpu;
        gpu.body = std::move(body);
        if (!CreateWaterBodyUniformBuffers(device, gpu) || !CreateWaterBodyMesh(device, gpu))
        {
            DestroyWaterBodyResources(gpu);
            Tracen("[WATER-OBJ] skipped invalid water body");
            continue;
        }
        Tracenf("[WATER-OBJ] loaded body id=%u name=%s level=%.2f quads=%u",
            gpu.body.id,
            gpu.body.name.c_str(),
            gpu.body.waterLevelY,
            gpu.indexCount / 6u);
        m_waterBodies.push_back(std::move(gpu));
    }

    if (m_waterDescriptorPool)
        CreateWaterDescriptors();

    Tracenf("[WATER-OBJ] active water bodies: %zu", m_waterBodies.size());
    return true;
}

bool TerrainRenderer::CreateWaterBodyUniformBuffers(VulkanDevice& device, WaterBodyGpu& waterBody)
{
    for (Buffer& buffer : waterBody.uniformBuffers)
    {
        DestroyBuffer(buffer);
        CreateHostVisibleBuffer(device, m_device, sizeof(WaterUniformBlock),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, nullptr, buffer);
        if (!buffer.buffer || !buffer.memory)
            return false;
    }
    return true;
}

bool TerrainRenderer::CreateWaterBodyMesh(VulkanDevice& device, WaterBodyGpu& waterBody)
{
    DestroyBuffer(waterBody.vertexBuffer);
    DestroyBuffer(waterBody.indexBuffer);
    waterBody.indexCount = 0;

    const WaterBody& body = waterBody.body;
    const std::size_t pixelCount = static_cast<std::size_t>(body.maskWidth) * body.maskHeight;
    if (body.maskWidth == 0 || body.maskHeight == 0 || body.shapeMask.size() != pixelCount ||
        body.bboxMax[0] <= body.bboxMin[0] || body.bboxMax[1] <= body.bboxMin[1])
    {
        return false;
    }

    std::vector<WaterVertex> vertices;
    std::vector<uint32_t> indices;
    vertices.reserve(pixelCount * 4u);
    indices.reserve(pixelCount * 6u);

    const float cellX = (body.bboxMax[0] - body.bboxMin[0]) / static_cast<float>(body.maskWidth);
    const float cellZ = (body.bboxMax[1] - body.bboxMin[1]) / static_cast<float>(body.maskHeight);
    const WaterConfig& config = ResolveWaterConfig(body);
    constexpr float kMaxEdgeFadeDistanceMeters = 3.0f;
    std::vector<float> distanceField;
    ComputeWaterBodyDistanceField(body, cellX, cellZ, kMaxEdgeFadeDistanceMeters, distanceField);
    const float edgeFadeDistance = std::clamp(config.edgeFadeDistance, 0.0f, kMaxEdgeFadeDistanceMeters);
    std::uint32_t fadeZoneVertices = 0;
    std::uint32_t fullyWaterVertices = 0;
    auto edgeAlphaAt = [&](float u, float v) {
        if (edgeFadeDistance <= 0.0001f)
        {
            ++fullyWaterVertices;
            return 1.0f;
        }
        const float sampledDistance = BilinearSampleWaterDistance(distanceField, body.maskWidth, body.maskHeight, u, v);
        const float edgeDistance = std::max(0.0f, sampledDistance - std::min(cellX, cellZ) * 0.5f);
        const float alpha = ApplyWaterEdgeCurve(edgeDistance / edgeFadeDistance, config.edgeFadeCurve);
        if (alpha >= 0.999f)
            ++fullyWaterVertices;
        else if (alpha > 0.001f)
            ++fadeZoneVertices;
        return std::clamp(alpha, 0.0f, 1.0f);
    };

    for (std::uint32_t y = 0; y < body.maskHeight; ++y)
    {
        for (std::uint32_t x = 0; x < body.maskWidth; ++x)
        {
            const std::size_t maskIndex = static_cast<std::size_t>(y) * body.maskWidth + x;
            if (body.shapeMask[maskIndex] == 0)
                continue;

            const float x0 = body.bboxMin[0] + static_cast<float>(x) * cellX;
            const float x1 = x0 + cellX;
            const float z0 = body.bboxMin[1] + static_cast<float>(y) * cellZ;
            const float z1 = z0 + cellZ;
            const float u0 = static_cast<float>(x) / static_cast<float>(body.maskWidth);
            const float u1 = static_cast<float>(x + 1u) / static_cast<float>(body.maskWidth);
            const float v0 = static_cast<float>(y) / static_cast<float>(body.maskHeight);
            const float v1 = static_cast<float>(y + 1u) / static_cast<float>(body.maskHeight);
            const uint32_t base = static_cast<uint32_t>(vertices.size());
            vertices.push_back({{x0, body.waterLevelY, z0}, {u0, v0}, edgeAlphaAt(u0, v0)});
            vertices.push_back({{x1, body.waterLevelY, z0}, {u1, v0}, edgeAlphaAt(u1, v0)});
            vertices.push_back({{x1, body.waterLevelY, z1}, {u1, v1}, edgeAlphaAt(u1, v1)});
            vertices.push_back({{x0, body.waterLevelY, z1}, {u0, v1}, edgeAlphaAt(u0, v1)});
            indices.insert(indices.end(), {base, base + 1u, base + 2u, base, base + 2u, base + 3u});
        }
    }

    if (indices.empty())
        return false;

    CreateHostVisibleBuffer(device, m_device, sizeof(WaterVertex) * vertices.size(),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertices.data(), waterBody.vertexBuffer);
    CreateHostVisibleBuffer(device, m_device, sizeof(uint32_t) * indices.size(),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indices.data(), waterBody.indexBuffer);
    waterBody.indexCount = static_cast<uint32_t>(indices.size());
    Tracenf("[WATER-OBJ-6] Vertex alpha computed: body_id=%u fade_zone_vertices=%u fully_water_vertices=%u",
        body.id, fadeZoneVertices, fullyWaterVertices);
    return waterBody.vertexBuffer.buffer && waterBody.indexBuffer.buffer;
}

bool TerrainRenderer::CreateWaterNormalTextures(VulkanDevice& device)
{
    constexpr uint32_t kSize = 128;
    const std::vector<std::uint8_t> small = GenerateWaterNormalPixels(kSize, kSize, 18.0f, 29.0f, 0.020f);
    const std::vector<std::uint8_t> large = GenerateWaterNormalPixels(kSize, kSize, 5.0f, 8.0f, 0.045f);
    return UploadRgbaTexture2D(device, "water_wave_small", kSize, kSize, small,
               VK_SAMPLER_ADDRESS_MODE_REPEAT, m_waterNormalSmall) &&
           UploadRgbaTexture2D(device, "water_wave_large", kSize, kSize, large,
               VK_SAMPLER_ADDRESS_MODE_REPEAT, m_waterNormalLarge);
}

bool TerrainRenderer::CreateWaterDescriptors()
{
    if (!m_waterDescriptorSetLayout)
    {
        VkDescriptorSetLayoutBinding ubo{};
        ubo.binding = 0;
        ubo.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ubo.descriptorCount = 1;
        ubo.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutBinding normalSmall{};
        normalSmall.binding = 1;
        normalSmall.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        normalSmall.descriptorCount = 1;
        normalSmall.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutBinding normalLarge = normalSmall;
        normalLarge.binding = 2;

        VkDescriptorSetLayoutBinding reflection = normalSmall;
        reflection.binding = 3;

        VkDescriptorSetLayoutBinding sceneColor = normalSmall;
        sceneColor.binding = 4;

        VkDescriptorSetLayoutBinding sceneDepth = normalSmall;
        sceneDepth.binding = 5;

        VkDescriptorSetLayoutBinding diffuse = normalSmall;
        diffuse.binding = 6;

        std::array<VkDescriptorSetLayoutBinding, 7> bindings = {ubo, normalSmall, normalLarge, reflection, sceneColor, sceneDepth, diffuse};
        VkDescriptorSetLayoutCreateInfo layout{};
        layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout.bindingCount = static_cast<uint32_t>(bindings.size());
        layout.pBindings = bindings.data();
        VK_CHECK(vkCreateDescriptorSetLayout(m_device, &layout, nullptr, &m_waterDescriptorSetLayout));
    }

    if (m_waterDescriptorPool)
        vkDestroyDescriptorPool(m_device, m_waterDescriptorPool, nullptr);
    m_waterDescriptorPool = VK_NULL_HANDLE;
    m_waterDescriptorSets.fill(VK_NULL_HANDLE);
    for (WaterBodyGpu& waterBody : m_waterBodies)
        waterBody.descriptorSets.fill(VK_NULL_HANDLE);

    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    const uint32_t maxWaterSets = kFramesInFlight * (1u + kMaxWaterBodyDraws);
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = maxWaterSets;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = maxWaterSets * 6u;

    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = maxWaterSets;
    pool.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    pool.pPoolSizes = poolSizes.data();
    VK_CHECK(vkCreateDescriptorPool(m_device, &pool, nullptr, &m_waterDescriptorPool));

    if (!AllocateWaterDescriptorSets(m_waterUniformBuffers, m_waterDescriptorSets))
        return false;
    for (WaterBodyGpu& waterBody : m_waterBodies)
    {
        if (!AllocateWaterDescriptorSets(waterBody.uniformBuffers, waterBody.descriptorSets))
            return false;
    }
    UpdateWaterDescriptors();
    return true;
}

bool TerrainRenderer::AllocateWaterDescriptorSets(const std::array<Buffer, kFramesInFlight>&,
                                                  std::array<VkDescriptorSet, kFramesInFlight>& descriptorSets)
{
    if (!m_waterDescriptorPool || !m_waterDescriptorSetLayout)
        return false;

    std::vector<VkDescriptorSetLayout> layouts(kFramesInFlight, m_waterDescriptorSetLayout);
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = m_waterDescriptorPool;
    alloc.descriptorSetCount = kFramesInFlight;
    alloc.pSetLayouts = layouts.data();
    return vkAllocateDescriptorSets(m_device, &alloc, descriptorSets.data()) == VK_SUCCESS;
}

void TerrainRenderer::WriteWaterDescriptorSets(const std::array<Buffer, kFramesInFlight>& uniformBuffers,
                                               const std::array<VkDescriptorSet, kFramesInFlight>& descriptorSets,
                                               const WaterMaterialTextureSet* materialTextures)
{
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        if (!descriptorSets[frame] || !uniformBuffers[frame].buffer)
            continue;
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = uniformBuffers[frame].buffer;
        bufferInfo.range = sizeof(WaterUniformBlock);

        VkDescriptorImageInfo smallInfo{};
        const Texture* normalA = materialTextures && materialTextures->normalA.view ? &materialTextures->normalA : &m_waterNormalSmall;
        const Texture* normalB = materialTextures && materialTextures->normalB.view ? &materialTextures->normalB : normalA;
        smallInfo.sampler = normalA->sampler;
        smallInfo.imageView = normalA->view;
        smallInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo largeInfo{};
        largeInfo.sampler = normalB->sampler ? normalB->sampler : m_waterNormalLarge.sampler;
        largeInfo.imageView = normalB->view ? normalB->view : m_waterNormalLarge.view;
        largeInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo reflectionInfo{};
        reflectionInfo.sampler = m_waterReflection.sampler;
        reflectionInfo.imageView = m_waterReflection.colorView;
        reflectionInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo sceneColorInfo{};
        sceneColorInfo.sampler = m_waterSceneSampler ? m_waterSceneSampler : m_waterNormalSmall.sampler;
        sceneColorInfo.imageView = m_waterSceneColorView ? m_waterSceneColorView : m_waterNormalSmall.view;
        sceneColorInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo sceneDepthInfo{};
        sceneDepthInfo.sampler = m_waterSceneSampler ? m_waterSceneSampler : m_waterNormalLarge.sampler;
        sceneDepthInfo.imageView = m_waterSceneDepthView ? m_waterSceneDepthView : m_waterNormalLarge.view;
        sceneDepthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo diffuseInfo{};
        const Texture* diffuse = materialTextures && materialTextures->diffuse.view ? &materialTextures->diffuse : &m_waterNormalSmall;
        diffuseInfo.sampler = diffuse->sampler;
        diffuseInfo.imageView = diffuse->view;
        diffuseInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        std::array<VkWriteDescriptorSet, 7> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = descriptorSets[frame];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &bufferInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = descriptorSets[frame];
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = &smallInfo;
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = descriptorSets[frame];
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[2].pImageInfo = &largeInfo;
        writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[3].dstSet = descriptorSets[frame];
        writes[3].dstBinding = 3;
        writes[3].descriptorCount = 1;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[3].pImageInfo = &reflectionInfo;
        writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[4].dstSet = descriptorSets[frame];
        writes[4].dstBinding = 4;
        writes[4].descriptorCount = 1;
        writes[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[4].pImageInfo = &sceneColorInfo;
        writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[5].dstSet = descriptorSets[frame];
        writes[5].dstBinding = 5;
        writes[5].descriptorCount = 1;
        writes[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[5].pImageInfo = &sceneDepthInfo;
        writes[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[6].dstSet = descriptorSets[frame];
        writes[6].dstBinding = 6;
        writes[6].descriptorCount = 1;
        writes[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[6].pImageInfo = &diffuseInfo;
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
}

void TerrainRenderer::UpdateWaterDescriptors()
{
    if (!m_waterDescriptorPool || !m_waterNormalSmall.view || !m_waterNormalLarge.view || !m_waterReflection.colorView)
        return;

    WriteWaterDescriptorSets(m_waterUniformBuffers, m_waterDescriptorSets);
    for (WaterBodyGpu& waterBody : m_waterBodies)
        WriteWaterDescriptorSets(waterBody.uniformBuffers, waterBody.descriptorSets,
            ResolveWaterMaterialTextures(waterBody.body));
}

bool TerrainRenderer::CreateOrRecreateWaterReflectionResources(VulkanDevice& device, bool force)
{
    return CreateOrRecreateWaterReflectionResources(device, force, WaterConfig::ReflectionQuality::Half);
}

bool TerrainRenderer::CreateOrRecreateWaterReflectionResources(VulkanDevice& device,
                                                               bool force,
                                                               WaterConfig::ReflectionQuality quality)
{
    const VkExtent2D swapExtent = device.GetSwapchainExtent();
    if (swapExtent.width == 0 || swapExtent.height == 0)
        return false;

    uint32_t divisor = 2;
    switch (quality)
    {
    case WaterConfig::ReflectionQuality::Quarter: divisor = 4; break;
    case WaterConfig::ReflectionQuality::Half: divisor = 2; break;
    case WaterConfig::ReflectionQuality::Full: divisor = 1; break;
    }

    const uint32_t width = std::max(1u, swapExtent.width / divisor);
    const uint32_t height = std::max(1u, swapExtent.height / divisor);
    const VkFormat colorFormat = device.GetSwapchainFormat();
    const VkFormat depthFormat = device.GetDepthStencilFormat();

    if (!force && m_waterReflection.colorView && m_waterReflection.depthView &&
        m_waterReflection.width == width && m_waterReflection.height == height &&
        m_waterReflection.quality == quality &&
        m_waterReflection.colorFormat == colorFormat && m_waterReflection.depthFormat == depthFormat)
    {
        return true;
    }

    const bool hadResources = m_waterReflection.colorImage != VK_NULL_HANDLE ||
        m_waterReflection.depthImage != VK_NULL_HANDLE ||
        m_waterReflection.framebuffer != VK_NULL_HANDLE;
    if (hadResources)
    {
        device.WaitIdle();
        Tracen("[WATER-2] Re-creating reflection resources");
    }
    DestroyWaterReflectionPipeline();
    DestroyWaterReflectionResources();

    m_waterReflection.width = width;
    m_waterReflection.height = height;
    m_waterReflection.quality = quality;
    m_waterReflection.colorFormat = colorFormat;
    m_waterReflection.depthFormat = depthFormat;
    m_waterReflection.colorLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    CreateDeviceLocalAttachmentImage(device, m_device, width, height, colorFormat,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        m_waterReflection.colorImage, m_waterReflection.colorMemory);
    CreateDeviceLocalAttachmentImage(device, m_device, width, height, depthFormat,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        m_waterReflection.depthImage, m_waterReflection.depthMemory);

    VkImageViewCreateInfo colorView{};
    colorView.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    colorView.image = m_waterReflection.colorImage;
    colorView.viewType = VK_IMAGE_VIEW_TYPE_2D;
    colorView.format = colorFormat;
    colorView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    colorView.subresourceRange.levelCount = 1;
    colorView.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(m_device, &colorView, nullptr, &m_waterReflection.colorView));

    VkImageViewCreateInfo depthView{};
    depthView.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    depthView.image = m_waterReflection.depthImage;
    depthView.viewType = VK_IMAGE_VIEW_TYPE_2D;
    depthView.format = depthFormat;
    depthView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (HasStencilAspect(depthFormat))
        depthView.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
    depthView.subresourceRange.levelCount = 1;
    depthView.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(m_device, &depthView, nullptr, &m_waterReflection.depthView));

    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = colorFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = depthFormat;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    std::array<VkAttachmentDescription, 2> attachments = {colorAttachment, depthAttachment};
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
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo renderPass{};
    renderPass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPass.attachmentCount = static_cast<uint32_t>(attachments.size());
    renderPass.pAttachments = attachments.data();
    renderPass.subpassCount = 1;
    renderPass.pSubpasses = &subpass;
    renderPass.dependencyCount = static_cast<uint32_t>(dependencies.size());
    renderPass.pDependencies = dependencies.data();
    VK_CHECK(vkCreateRenderPass(m_device, &renderPass, nullptr, &m_waterReflection.renderPass));

    std::array<VkImageView, 2> framebufferAttachments = {m_waterReflection.colorView, m_waterReflection.depthView};
    VkFramebufferCreateInfo framebuffer{};
    framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer.renderPass = m_waterReflection.renderPass;
    framebuffer.attachmentCount = static_cast<uint32_t>(framebufferAttachments.size());
    framebuffer.pAttachments = framebufferAttachments.data();
    framebuffer.width = width;
    framebuffer.height = height;
    framebuffer.layers = 1;
    VK_CHECK(vkCreateFramebuffer(m_device, &framebuffer, nullptr, &m_waterReflection.framebuffer));

    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.maxLod = 1.0f;
    VK_CHECK(vkCreateSampler(m_device, &sampler, nullptr, &m_waterReflection.sampler));

    const char* qualityName = "Half";
    if (quality == WaterConfig::ReflectionQuality::Quarter)
        qualityName = "Quarter";
    else if (quality == WaterConfig::ReflectionQuality::Full)
        qualityName = "Full";
    Tracenf("[WATER-2] Reflection resources: %ux%u (quality=%s)", width, height, qualityName);
    return true;
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
    pipeline.renderPass = m_mainRenderPass ? m_mainRenderPass : device.GetRenderPass();
    pipeline.subpass = 0;
    VK_CHECK(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &m_pipeline));

    vkDestroyShaderModule(m_device, ps, nullptr);
    vkDestroyShaderModule(m_device, vs, nullptr);
    return true;
}

bool TerrainRenderer::CreateWaterReflectionPipeline(VulkanDevice& device)
{
    if (!m_assets || !m_waterReflection.renderPass)
        return false;

    DestroyWaterReflectionPipeline();

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
    depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;

    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    VkPushConstantRange pushConstant{};
    pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstant.offset = 0;
    pushConstant.size = sizeof(float) * 4u;

    VkPipelineLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &m_descriptorSetLayout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &pushConstant;
    VK_CHECK(vkCreatePipelineLayout(m_device, &layout, nullptr, &m_waterReflectionPipelineLayout));

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
    pipeline.layout = m_waterReflectionPipelineLayout;
    pipeline.renderPass = m_waterReflection.renderPass;
    pipeline.subpass = 0;
    VK_CHECK(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &m_waterReflectionPipeline));

    vkDestroyShaderModule(m_device, ps, nullptr);
    vkDestroyShaderModule(m_device, vs, nullptr);
    return true;
}

bool TerrainRenderer::CreateWaterPipeline(VulkanDevice& device)
{
    if (!m_assets || !m_waterDescriptorSetLayout)
        return false;

    DestroyWaterPipeline();

    VkShaderModule vs = CreateShaderModule(m_device, *m_assets, "assets/shaders/water_vs.spv");
    VkShaderModule ps = CreateShaderModule(m_device, *m_assets, "assets/shaders/water_ps.spv");

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
    binding.stride = sizeof(WaterVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attributes[3]{};
    attributes[0].location = 0;
    attributes[0].binding = 0;
    attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[0].offset = offsetof(WaterVertex, position);
    attributes[1].location = 1;
    attributes[1].binding = 0;
    attributes[1].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[1].offset = offsetof(WaterVertex, uv);
    attributes[2].location = 2;
    attributes[2].binding = 0;
    attributes[2].format = VK_FORMAT_R32_SFLOAT;
    attributes[2].offset = offsetof(WaterVertex, edgeAlpha);

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
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_FALSE;
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
    layout.pSetLayouts = &m_waterDescriptorSetLayout;
    VK_CHECK(vkCreatePipelineLayout(m_device, &layout, nullptr, &m_waterPipelineLayout));

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
    pipeline.layout = m_waterPipelineLayout;
    pipeline.renderPass = m_mainRenderPass ? m_mainRenderPass : device.GetRenderPass();
    pipeline.subpass = 0;
    VK_CHECK(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &m_waterPipeline));

    vkDestroyShaderModule(m_device, ps, nullptr);
    vkDestroyShaderModule(m_device, vs, nullptr);
    return true;
}

bool TerrainRenderer::CreateShadowPipeline()
{
    if (!m_assets || !m_shadowRenderPass)
        return false;

    VkShaderModule vs = CreateShaderModule(m_device, *m_assets, "assets/shaders/shadow_depth_vs.spv");

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    stage.module = vs;
    stage.pName = "VSMain";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attributes[1]{};
    attributes[0].location = 0;
    attributes[0].binding = 0;
    attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[0].offset = offsetof(Vertex, position);

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
    raster.depthBiasEnable = VK_TRUE;
    raster.depthBiasConstantFactor = 1.25f;
    raster.depthBiasSlopeFactor = 1.75f;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push.offset = 0;
    push.size = sizeof(WorldMat4);

    VkPipelineLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    VK_CHECK(vkCreatePipelineLayout(m_device, &layout, nullptr, &m_shadowPipelineLayout));

    VkGraphicsPipelineCreateInfo pipeline{};
    pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline.stageCount = 1;
    pipeline.pStages = &stage;
    pipeline.pVertexInputState = &vertexInput;
    pipeline.pInputAssemblyState = &assembly;
    pipeline.pViewportState = &viewport;
    pipeline.pRasterizationState = &raster;
    pipeline.pMultisampleState = &multisample;
    pipeline.pDepthStencilState = &depth;
    pipeline.pDynamicState = &dynamic;
    pipeline.layout = m_shadowPipelineLayout;
    pipeline.renderPass = m_shadowRenderPass;
    VK_CHECK(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &m_shadowPipeline));

    vkDestroyShaderModule(m_device, vs, nullptr);
    return true;
}

void TerrainRenderer::DestroyPipeline()
{
    DestroyWaterPipeline();
    DestroyWaterReflectionPipeline();

    if (m_pipeline)
        vkDestroyPipeline(m_device, m_pipeline, nullptr);
    m_pipeline = VK_NULL_HANDLE;

    if (m_pipelineLayout)
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    m_pipelineLayout = VK_NULL_HANDLE;
}

void TerrainRenderer::DestroyWaterPipeline()
{
    if (m_waterPipeline)
        vkDestroyPipeline(m_device, m_waterPipeline, nullptr);
    m_waterPipeline = VK_NULL_HANDLE;
    if (m_waterPipelineLayout)
        vkDestroyPipelineLayout(m_device, m_waterPipelineLayout, nullptr);
    m_waterPipelineLayout = VK_NULL_HANDLE;
}

void TerrainRenderer::DestroyWaterReflectionPipeline()
{
    if (m_waterReflectionPipeline)
        vkDestroyPipeline(m_device, m_waterReflectionPipeline, nullptr);
    m_waterReflectionPipeline = VK_NULL_HANDLE;
    if (m_waterReflectionPipelineLayout)
        vkDestroyPipelineLayout(m_device, m_waterReflectionPipelineLayout, nullptr);
    m_waterReflectionPipelineLayout = VK_NULL_HANDLE;
}

void TerrainRenderer::DestroyWaterReflectionResources()
{
    if (m_waterReflection.sampler)
        vkDestroySampler(m_device, m_waterReflection.sampler, nullptr);
    if (m_waterReflection.framebuffer)
        vkDestroyFramebuffer(m_device, m_waterReflection.framebuffer, nullptr);
    if (m_waterReflection.renderPass)
        vkDestroyRenderPass(m_device, m_waterReflection.renderPass, nullptr);
    if (m_waterReflection.colorView)
        vkDestroyImageView(m_device, m_waterReflection.colorView, nullptr);
    if (m_waterReflection.depthView)
        vkDestroyImageView(m_device, m_waterReflection.depthView, nullptr);
    if (m_waterReflection.colorImage)
        vkDestroyImage(m_device, m_waterReflection.colorImage, nullptr);
    if (m_waterReflection.depthImage)
        vkDestroyImage(m_device, m_waterReflection.depthImage, nullptr);
    if (m_waterReflection.colorMemory)
        vkFreeMemory(m_device, m_waterReflection.colorMemory, nullptr);
    if (m_waterReflection.depthMemory)
        vkFreeMemory(m_device, m_waterReflection.depthMemory, nullptr);
    m_waterReflection = {};
}

void TerrainRenderer::DestroyWaterResources()
{
    DestroyWaterPipeline();
    DestroyWaterReflectionPipeline();
    DestroyWaterReflectionResources();
    if (m_waterDescriptorPool)
        vkDestroyDescriptorPool(m_device, m_waterDescriptorPool, nullptr);
    m_waterDescriptorPool = VK_NULL_HANDLE;
    if (m_waterDescriptorSetLayout)
        vkDestroyDescriptorSetLayout(m_device, m_waterDescriptorSetLayout, nullptr);
    m_waterDescriptorSetLayout = VK_NULL_HANDLE;
    m_waterDescriptorSets.fill(VK_NULL_HANDLE);
    DestroyWaterBodyResources();
    for (Buffer& buffer : m_waterUniformBuffers)
        DestroyBuffer(buffer);
    DestroyTexture(m_waterNormalSmall);
    DestroyTexture(m_waterNormalLarge);
}

void TerrainRenderer::DestroyWaterBodyResources()
{
    for (WaterBodyGpu& waterBody : m_waterBodies)
        DestroyWaterBodyResources(waterBody);
    m_waterBodies.clear();
}

void TerrainRenderer::DestroyWaterBodyResources(WaterBodyGpu& waterBody)
{
    DestroyBuffer(waterBody.vertexBuffer);
    DestroyBuffer(waterBody.indexBuffer);
    for (Buffer& buffer : waterBody.uniformBuffers)
        DestroyBuffer(buffer);
    waterBody.descriptorSets.fill(VK_NULL_HANDLE);
    waterBody.indexCount = 0;
}

void TerrainRenderer::DestroyShadowPipeline()
{
    if (m_shadowPipeline)
        vkDestroyPipeline(m_device, m_shadowPipeline, nullptr);
    m_shadowPipeline = VK_NULL_HANDLE;
    if (m_shadowPipelineLayout)
        vkDestroyPipelineLayout(m_device, m_shadowPipelineLayout, nullptr);
    m_shadowPipelineLayout = VK_NULL_HANDLE;
}

void TerrainRenderer::DestroyShadowResources()
{
    DestroyShadowPipeline();
    for (VkFramebuffer& fb : m_shadowFramebuffers)
    {
        if (fb)
            vkDestroyFramebuffer(m_device, fb, nullptr);
        fb = VK_NULL_HANDLE;
    }
    if (m_shadowRenderPass)
        vkDestroyRenderPass(m_device, m_shadowRenderPass, nullptr);
    m_shadowRenderPass = VK_NULL_HANDLE;
    if (m_shadowSampler)
        vkDestroySampler(m_device, m_shadowSampler, nullptr);
    m_shadowSampler = VK_NULL_HANDLE;
    for (VkImageView& view : m_shadowLayerViews)
    {
        if (view)
            vkDestroyImageView(m_device, view, nullptr);
        view = VK_NULL_HANDLE;
    }
    if (m_shadowArrayView)
        vkDestroyImageView(m_device, m_shadowArrayView, nullptr);
    m_shadowArrayView = VK_NULL_HANDLE;
    if (m_shadowImage)
        vkDestroyImage(m_device, m_shadowImage, nullptr);
    m_shadowImage = VK_NULL_HANDLE;
    if (m_shadowMemory)
        vkFreeMemory(m_device, m_shadowMemory, nullptr);
    m_shadowMemory = VK_NULL_HANDLE;
    m_shadowLayout = VK_IMAGE_LAYOUT_UNDEFINED;
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

void TerrainRenderer::UpdateUniform(uint32_t frameIndex, const WorldCamera& camera, bool reflectionPass)
{
    if (frameIndex >= kFramesInFlight || !m_uniformBuffers[frameIndex].memory)
    {
        Tracen("[TERRAIN] UpdateUniform skipped: uniform buffer is not ready");
        return;
    }

    UniformBlock uniform{};
    uniform.mvp = camera.viewProjection;
    for (uint32_t i = 0; i < m_paletteSlots.size(); ++i)
    {
        uniform.materialTiling[i][0] = std::clamp(m_paletteSlots[i].tilingScaleX, 0.01f, 64.0f);
        uniform.materialTiling[i][1] = std::clamp(m_paletteSlots[i].tilingScaleY, 0.01f, 64.0f);
        uniform.materialTiling[i][2] = m_paletteSlots[i].uvOffset[0];
        uniform.materialTiling[i][3] = m_paletteSlots[i].uvOffset[1];
        uniform.materialTintNormal[i][0] = m_paletteSlots[i].colorTint[0];
        uniform.materialTintNormal[i][1] = m_paletteSlots[i].colorTint[1];
        uniform.materialTintNormal[i][2] = m_paletteSlots[i].colorTint[2];
        uniform.materialTintNormal[i][3] = std::clamp(m_paletteSlots[i].normalStrength, 0.0f, 3.0f);
        uniform.materialPbr[i][0] = std::clamp(m_paletteSlots[i].aoStrength, 0.0f, 1.0f);
        uniform.materialPbr[i][1] = std::clamp(m_paletteSlots[i].roughnessStrength, 0.0f, 2.0f);
        uniform.materialPbr[i][2] = std::clamp(m_paletteSlots[i].metallicStrength, 0.0f, 1.0f);
        uniform.materialPbr[i][3] = m_paletteSlots[i].uvRotationDegrees * 3.1415926535f / 180.0f;
    }
    uniform.terrainMaterialParams[0] = m_sceneTerrain.triplanarEnabled ? 1.0f : 0.0f;
    uniform.terrainMaterialParams[1] = std::clamp(m_sceneTerrain.triplanarSharpness, 1.0f, 16.0f);
    uniform.terrainMaterialParams[2] = std::clamp(m_sceneTerrain.triplanarSlopeThreshold, 0.0f, 1.0f);
    uniform.terrainMaterialParams[3] = std::clamp(m_sceneTerrain.triplanarSlopeTransition, 0.001f, 1.0f);
    uniform.cameraPos[0] = camera.eye.x;
    uniform.cameraPos[1] = camera.eye.y;
    uniform.cameraPos[2] = camera.eye.z;
    uniform.cameraPos[3] = 1.0f;
    const DirectionalLight& directional = m_lightingState.directional;
    const AmbientLight& ambient = m_lightingState.ambient;
    const float azimuthRadians = std::clamp(directional.azimuthDegrees, 0.0f, 360.0f) * 3.1415926535f / 180.0f;
    const float elevationRadians = std::clamp(directional.elevationDegrees, 0.0f, 90.0f) * 3.1415926535f / 180.0f;
    const float cosElevation = std::cos(elevationRadians);
    const float sunEnabled = directional.enabled ? 1.0f : 0.0f;
    const float sunIntensity = std::max(0.0f, directional.intensity) * sunEnabled;
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
    for (uint32_t i = 0; i < kShadowCascadeCount; ++i)
    {
        uniform.cascadeViewProj[i] = m_shadowCascadeViewProj[i];
        uniform.cascadeSplits[i] = m_shadowCascadeSplits[i];
    }
    uniform.shadowParams[0] = (!reflectionPass && m_lightingState.sunShadowsEnabled &&
        m_shadowLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL) ? 1.0f : 0.0f;
    uniform.shadowParams[1] = static_cast<float>(kShadowResolution);
    uniform.shadowParams[2] = 0.0015f;
    uniform.shadowParams[3] = 0.0f;
    if (reflectionPass)
    {
        uniform.numPointLights = 0;
        uniform.numSpotLights = 0;
        uniform.lightPadding[0] = 1.0f;
        uniform.lightPadding[1] = std::isfinite(m_reflectionClipWaterLevelY)
            ? m_reflectionClipWaterLevelY
            : 0.0f;
    }
    else
    {
        FillDynamicLightingUniforms(m_lightingState, uniform);
        uniform.lightPadding[0] = 0.0f;
        uniform.lightPadding[1] = 0.0f;
    }
    uniform.waterGlobalParams[0] = static_cast<float>(m_latestWaterTimeSeconds);
    if (!reflectionPass)
    {
        uint32_t activeCount = 0;
        uint32_t enabledCount = 0;
        for (const WaterBodyGpu& waterBody : m_waterBodies)
        {
            const WaterBody& body = waterBody.body;
            const WaterConfig& water = ResolveWaterConfig(body);
            if (!water.enabled)
                continue;

            ++enabledCount;
            if (activeCount >= kMaxTerrainWaterBodies)
                continue;

            auto& out = uniform.terrainWaterBodies[activeCount++];
            out.bboxMinMax[0] = std::min(body.bboxMin[0], body.bboxMax[0]);
            out.bboxMinMax[1] = std::min(body.bboxMin[1], body.bboxMax[1]);
            out.bboxMinMax[2] = std::max(body.bboxMin[0], body.bboxMax[0]);
            out.bboxMinMax[3] = std::max(body.bboxMin[1], body.bboxMax[1]);
            out.levelModeEnabled[0] = body.waterLevelY;
            out.levelModeEnabled[1] = static_cast<float>(static_cast<int>(water.causticMode));
            out.levelModeEnabled[2] = 1.0f;
            out.levelModeEnabled[3] = water.foamEnabled ? 1.0f : 0.0f;
            out.foamParams[0] = std::clamp(water.foamDistance, 0.02f, 1.5f);
            out.foamParams[1] = std::clamp(water.foamSoftness, 0.001f, 1.0f);
            out.foamParams[2] = std::clamp(water.foamIntensity, 0.0f, 2.0f);
            out.foamParams[3] = std::clamp(water.foamScale, 0.05f, 2.0f);
            out.causticParams[0] = std::clamp(water.causticIntensity, 0.0f, 3.0f);
            out.causticParams[1] = std::clamp(water.causticScale, 0.05f, 2.0f);
            out.causticParams[2] = std::clamp(water.causticSpeed, 0.0f, 2.0f);
            out.causticParams[3] = std::clamp(water.causticMaxDepth, 1.0f, 30.0f);
            out.edgeParams[0] = std::clamp(water.edgeFadeDistance, 0.0f, 3.0f);
            out.edgeParams[1] = static_cast<float>(static_cast<int>(water.edgeFadeCurve));
        }

        uniform.waterGlobalParams[1] = static_cast<float>(activeCount);
        uniform.waterGlobalParams[2] = static_cast<float>(enabledCount > kMaxTerrainWaterBodies ? enabledCount : activeCount);
        static bool loggedWaterBodyLimit = false;
        if (enabledCount > kMaxTerrainWaterBodies && !loggedWaterBodyLimit)
        {
            Tracenf("[TERRAIN-WATER] Active water bodies exceed terrain shader limit (%u>%u), truncating",
                enabledCount, kMaxTerrainWaterBodies);
            loggedWaterBodyLimit = true;
        }
    }
    static bool loggedLighting = false;
    if (!loggedLighting)
    {
        Tracen("[TERRAIN] lighting now reads LightingState");
        loggedLighting = true;
    }
    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(m_device, m_uniformBuffers[frameIndex].memory, 0, sizeof(uniform), 0, &mapped));
    std::memcpy(mapped, &uniform, sizeof(uniform));
    vkUnmapMemory(m_device, m_uniformBuffers[frameIndex].memory);
    if (m_materialParamsDirty)
    {
        Tracen("[TMAT] params buffer updated (live, no reload, no remesh)");
        m_materialParamsDirty = false;
    }
    if (m_triplanarParamsDirty)
    {
        Tracenf("[TRIPLANAR] enabled=%s scope=terrain sharpness=%.2f slopeThreshold=%.3f transition=%.3f",
            m_sceneTerrain.triplanarEnabled ? "yes" : "no",
            std::clamp(m_sceneTerrain.triplanarSharpness, 1.0f, 16.0f),
            std::clamp(m_sceneTerrain.triplanarSlopeThreshold, 0.0f, 1.0f),
            std::clamp(m_sceneTerrain.triplanarSlopeTransition, 0.001f, 1.0f));
        if (m_sceneTerrain.triplanarEnabled)
            Tracen("[TRIPLANAR] sample mode active, layers=8");
        m_triplanarParamsDirty = false;
    }
    if (m_sceneTerrain.triplanarEnabled && !m_triPerfStaticLogged)
    {
        const bool terrainMipsUsable =
            m_baseTexture.mipLevels > 1 &&
            m_normalTexture.mipLevels > 1 &&
            m_aoTexture.mipLevels > 1 &&
            m_roughnessTexture.mipLevels > 1 &&
            m_metallicTexture.mipLevels > 1;
        const double avgActiveLayers = EstimateAverageActiveSplatLayers(m_splatABytes, m_splatBBytes, m_splatWidth, m_splatHeight);
        const double avgAxesPerLayer = EstimateAverageSlopeAxes(m_heightCmGrid,
            m_heightGridWidth,
            m_heightGridHeight,
            m_cellScaleMeters,
            std::clamp(m_sceneTerrain.triplanarSlopeThreshold, 0.0f, 1.0f),
            std::clamp(m_sceneTerrain.triplanarSlopeTransition, 0.001f, 1.0f));
        const double gatedPlanarSamples = 2.0 + avgActiveLayers * 5.0;
        const double gatedTriplanarSamples = 2.0 + avgActiveLayers * 15.0;
        const double selectiveSamples = 2.0 + avgActiveLayers * 5.0 * avgAxesPerLayer;
        Tracenf("[TRI-PERF] avgActiveLayers=%.2f samples/fragment planar=%.1f triplanar=%.1f unconditionalPlanar=42 unconditionalTriplanar=122 layers=8 maps=diff,nor,ao,roughness,metallic axes=1|3 splat=2 shadow_pcf=25-50-independent",
            avgActiveLayers,
            gatedPlanarSamples,
            gatedTriplanarSamples);
        Tracenf("[TRIPLANAR-OPT] mode=selective slopeThreshold=%.3f transition=%.3f avgAxesPerLayer=%.2f samples/fragment=%.1f fps=%.1f",
            std::clamp(m_sceneTerrain.triplanarSlopeThreshold, 0.0f, 1.0f),
            std::clamp(m_sceneTerrain.triplanarSlopeTransition, 0.001f, 1.0f),
            avgAxesPerLayer,
            selectiveSamples,
            m_latestFps);
        Tracenf("[TRI-PERF] triplanar LOD mode=textureGrad-explicit mipUsed=%s forced-0=%s reason=%s",
            terrainMipsUsable ? "yes" : "no",
            terrainMipsUsable ? "no" : "yes",
            terrainMipsUsable ? "terrain-array-textures-have-full-mip-chain-and-branch-safe-gradients" : "one-or-more-terrain-array-textures-have-1-mip");
        Tracen("[TRI-PERF] layer sampling=weight-gated threshold=1/255 derivativeSafe=textureGrad-gradients-before-branch");
        Tracenf("[TRI-PERF] likely bottleneck class=%s",
            terrainMipsUsable ? "active-layer-count-and-remaining-triplanar-sample-count" : "fragment-texture-bandwidth-plus-mip0-cache-pressure");
        m_triPerfStaticLogged = true;
    }
}

TerrainRenderer::WaterUniformBlock TerrainRenderer::BuildWaterUniform(const WorldCamera& camera,
                                                                       double timeSeconds,
                                                                       const WaterConfig& water,
                                                                       float waterLevelY,
                                                                       bool reflectionTarget) const
{
    WaterUniformBlock uniform{};
    uniform.mvp = camera.viewProjection;
    uniform.cameraPos[0] = camera.eye.x;
    uniform.cameraPos[1] = camera.eye.y;
    uniform.cameraPos[2] = camera.eye.z;
    uniform.cameraPos[3] = 1.0f;

    const DirectionalLight& directional = m_lightingState.directional;
    const AmbientLight& ambient = m_lightingState.ambient;
    const float azimuthRadians = std::clamp(directional.azimuthDegrees, 0.0f, 360.0f) * 3.1415926535f / 180.0f;
    const float elevationRadians = std::clamp(directional.elevationDegrees, 0.0f, 90.0f) * 3.1415926535f / 180.0f;
    const float cosElevation = std::cos(elevationRadians);
    const float sunEnabled = directional.enabled ? 1.0f : 0.0f;
    const float sunIntensity = std::max(0.0f, directional.intensity) * sunEnabled;
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

    uniform.baseColor[0] = std::clamp(water.baseColor[0], 0.0f, 1.0f);
    uniform.baseColor[1] = std::clamp(water.baseColor[1], 0.0f, 1.0f);
    uniform.baseColor[2] = std::clamp(water.baseColor[2], 0.0f, 1.0f);
    uniform.baseColor[3] = std::clamp(water.baseColor[3], 0.0f, 1.0f);
    uniform.reflectionColor[0] = std::clamp(water.reflectionColor[0], 0.0f, 2.0f);
    uniform.reflectionColor[1] = std::clamp(water.reflectionColor[1], 0.0f, 2.0f);
    uniform.reflectionColor[2] = std::clamp(water.reflectionColor[2], 0.0f, 2.0f);
    uniform.reflectionColor[3] = 1.0f;
    uniform.waveParams1[0] = std::clamp(water.waveScaleSmall, 0.001f, 0.12f);
    uniform.waveParams1[1] = std::clamp(water.waveScaleLarge, 0.001f, 0.08f);
    uniform.waveParams1[2] = std::clamp(water.waveSpeedSmall, 0.0f, 0.5f);
    uniform.waveParams1[3] = std::clamp(water.waveSpeedLarge, 0.0f, 0.5f);
    uniform.waveParams2[0] = std::clamp(water.normalStrength, 0.0f, 2.0f);
    uniform.waveParams2[1] = std::clamp(water.fresnelPower, 1.0f, 10.0f);
    uniform.waveParams2[2] = std::clamp(water.fresnelMin, 0.0f, 0.5f);
    uniform.waveParams2[3] = 0.0f;
    uniform.levelTimeEnabled[0] = waterLevelY;
    uniform.levelTimeEnabled[1] = static_cast<float>(timeSeconds);
    uniform.levelTimeEnabled[2] = water.enabled ? 1.0f : 0.0f;
    uniform.levelTimeEnabled[3] = 0.0f;
    uniform.reflectionParams[0] = (reflectionTarget && water.reflectionEnabled && m_waterReflection.colorView) ? 1.0f : 0.0f;
    uniform.reflectionParams[1] = std::clamp(water.reflectionDistortionStrength, 0.0f, 0.2f);
    uniform.reflectionParams[2] = m_waterReflection.width > 0 ? static_cast<float>(m_waterReflection.width) : 1.0f;
    uniform.reflectionParams[3] = m_waterReflection.height > 0 ? static_cast<float>(m_waterReflection.height) : 1.0f;
    uniform.refractionParams[0] = (water.refractionEnabled && m_waterSceneColorView && m_waterSceneDepthView) ? 1.0f : 0.0f;
    uniform.refractionParams[1] = std::clamp(water.refractionStrength, 0.0f, 0.1f);
    uniform.refractionParams[2] = std::clamp(water.refractionDepthStrength, 0.0f, 2.0f);
    uniform.refractionParams[3] = m_waterSceneExtent.width > 0 ? static_cast<float>(m_waterSceneExtent.width) : 1.0f;
    uniform.shallowColor[0] = std::clamp(water.shallowColor[0], 0.0f, 2.0f);
    uniform.shallowColor[1] = std::clamp(water.shallowColor[1], 0.0f, 2.0f);
    uniform.shallowColor[2] = std::clamp(water.shallowColor[2], 0.0f, 2.0f);
    uniform.shallowColor[3] = 1.0f;
    uniform.deepColor[0] = std::clamp(water.deepColor[0], 0.0f, 2.0f);
    uniform.deepColor[1] = std::clamp(water.deepColor[1], 0.0f, 2.0f);
    uniform.deepColor[2] = std::clamp(water.deepColor[2], 0.0f, 2.0f);
    uniform.deepColor[3] = 1.0f;
    uniform.depthParams[0] = std::clamp(water.depthColorMin, 0.0f, 50.0f);
    uniform.depthParams[1] = std::max(uniform.depthParams[0] + 0.001f, std::clamp(water.depthColorMax, 0.001f, 50.0f));
    uniform.depthParams[2] = std::clamp(water.depthFadeDistance, 0.001f, 50.0f);
    uniform.depthParams[3] = m_waterSceneExtent.height > 0 ? static_cast<float>(m_waterSceneExtent.height) : 1.0f;
    uniform.foamParams[0] = water.foamEnabled ? 1.0f : 0.0f;
    uniform.foamParams[1] = std::clamp(water.foamScale, 0.05f, 2.0f);
    uniform.foamParams[2] = std::clamp(water.foamScrollSpeed, 0.0f, 0.1f);
    uniform.foamParams[3] = std::clamp(water.foamIntensity, 0.0f, 2.0f);
    uniform.foamDepthParams[0] = std::clamp(water.foamDistance, 0.02f, 1.5f);
    uniform.foamDepthParams[1] = std::clamp(water.foamSoftness, 0.001f, 1.0f);
    uniform.foamDepthParams[2] = std::clamp(water.foamTerrainThickness, 0.0f, 1.0f);
    uniform.foamDepthParams[3] = 0.0f;
    uniform.causticParams[0] = static_cast<float>(static_cast<int>(water.causticMode));
    uniform.causticParams[1] = std::clamp(water.causticIntensity, 0.0f, 3.0f);
    uniform.causticParams[2] = std::clamp(water.causticScale, 0.05f, 2.0f);
    uniform.causticParams[3] = std::clamp(water.causticMaxDepth, 1.0f, 30.0f);
    uniform.cameraNearFar[0] = std::max(camera.nearPlane, 0.0001f);
    uniform.cameraNearFar[1] = std::max(camera.farPlane, uniform.cameraNearFar[0] + 0.001f);
    uniform.cameraNearFar[2] = 0.0f;
    uniform.cameraNearFar[3] = 0.0f;
    uniform.textureParams[0] = 0.0f;
    uniform.textureParams[1] = 0.0f;
    uniform.textureParams[2] = 0.0f;
    uniform.textureParams[3] = 1.0f;
    uniform.textureScroll[0] = 0.03f;
    uniform.textureScroll[1] = 0.014f;
    uniform.textureScroll[2] = -0.015f;
    uniform.textureScroll[3] = 0.02f;
    return uniform;
}

void TerrainRenderer::UploadWaterUniform(Buffer& buffer, const WaterUniformBlock& uniform)
{
    if (!buffer.memory)
        return;
    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(m_device, buffer.memory, 0, sizeof(uniform), 0, &mapped));
    std::memcpy(mapped, &uniform, sizeof(uniform));
    vkUnmapMemory(m_device, buffer.memory);
}

const WaterConfig& TerrainRenderer::ResolveWaterConfig(const WaterBody& body) const
{
    if (!body.materialId.empty())
    {
        auto it = m_waterMaterials.find(body.materialId);
        if (it != m_waterMaterials.end())
            return it->second.config;
    }
    if (!m_waterMaterials.empty())
    {
        auto defaultIt = m_waterMaterials.find("watermat_Default_Water");
        if (defaultIt != m_waterMaterials.end())
            return defaultIt->second.config;
    }
    return body.config;
}

void TerrainRenderer::UpdateWaterBodyUniform(uint32_t frameIndex,
                                             const WorldCamera& camera,
                                             double timeSeconds,
                                             WaterBodyGpu& waterBody,
                                             bool reflectionTarget)
{
    if (frameIndex >= kFramesInFlight)
        return;
    WaterUniformBlock uniform =
        BuildWaterUniform(camera, timeSeconds, ResolveWaterConfig(waterBody.body), waterBody.body.waterLevelY, reflectionTarget);
    const WaterMaterialTextureSet* textures = ResolveWaterMaterialTextures(waterBody.body);
    const WaterMaterialData* material = nullptr;
    if (!waterBody.body.materialId.empty())
    {
        auto it = m_waterMaterials.find(waterBody.body.materialId);
        if (it != m_waterMaterials.end())
            material = &it->second;
    }
    if (textures)
    {
        uniform.textureParams[0] = textures->normalA.view ? 1.0f : 0.0f;
        uniform.textureParams[1] = textures->normalB.view ? 1.0f : 0.0f;
        uniform.textureParams[2] = textures->diffuse.view ? 1.0f : 0.0f;
    }
    if (material)
    {
        uniform.textureParams[3] = std::clamp(material->normalTiling, 0.001f, 100.0f);
        uniform.textureScroll[0] = material->scrollSpeedA[0];
        uniform.textureScroll[1] = material->scrollSpeedA[1];
        uniform.textureScroll[2] = material->scrollSpeedB[0];
        uniform.textureScroll[3] = material->scrollSpeedB[1];
        const bool hasCustomScroll = std::abs(uniform.textureScroll[0]) > 0.00001f ||
            std::abs(uniform.textureScroll[1]) > 0.00001f ||
            std::abs(uniform.textureScroll[2]) > 0.00001f ||
            std::abs(uniform.textureScroll[3]) > 0.00001f;
        if (!hasCustomScroll)
        {
            uniform.textureScroll[0] = 0.03f;
            uniform.textureScroll[1] = 0.014f;
            uniform.textureScroll[2] = -0.015f;
            uniform.textureScroll[3] = 0.02f;
        }
    }
    UploadWaterUniform(waterBody.uniformBuffers[frameIndex], uniform);
}
