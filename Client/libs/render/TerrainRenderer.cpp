#include "TerrainRenderer.h"
#include "SceneClearColor.h"

#include "Debug.h"
#include "IXRHIBinding.h"
#include "IXRHIBuffer.h"
#include "IXRHICommandList.h"
#include "IXRHIDevice.h"
#include "IXRHIPipeline.h"
#include "IXRHIQuery.h"
#include "IXRHIRenderPass.h"
#include "IXRHIRenderTarget.h"
#include "IXRHIShader.h"
#include "IXRHITexture.h"
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
namespace xm = ixtreeme::math;

constexpr uint32_t kMaxWaterBodyDraws = 64;

// m_sceneTerrain keeps the scene's terrain settings; the live grids are m_heightCmGrid & co. A copy of
// the loaded grids kept there is dead weight that every GetTerrainSceneData() call used to copy too.
void ReleaseTerrainGrids(TerrainSceneData& terrain)
{
    std::vector<float>().swap(terrain.heightCmGrid);
    std::vector<std::uint16_t>().swap(terrain.attributes);
    std::vector<std::uint8_t>().swap(terrain.splatABytes);
    std::vector<std::uint8_t>().swap(terrain.splatBBytes);
}

const char* RhiFormatName(ixrhi::IXRHIFormat format)
{
    switch (format)
    {
    case ixrhi::IXRHIFormat::R8Unorm: return "R8_UNORM";
    case ixrhi::IXRHIFormat::R8G8B8A8Unorm: return "R8G8B8A8_UNORM";
    case ixrhi::IXRHIFormat::R8G8B8A8Srgb: return "R8G8B8A8_SRGB";
    case ixrhi::IXRHIFormat::R32Float: return "R32_SFLOAT";
    case ixrhi::IXRHIFormat::D32Float: return "D32_SFLOAT";
    default: return "UNDEFINED";
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
        return 1.0f - xm::Exp(-3.0f * t);
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
    const int searchRadius = std::max(1, static_cast<int>(xm::Ceil(maxDistanceMeters / searchCell)) + 1);
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
            const float distanceMeters = xm::Sqrt(minDistSq);
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
    const std::uint32_t x0 = static_cast<std::uint32_t>(xm::Floor(sx));
    const std::uint32_t y0 = static_cast<std::uint32_t>(xm::Floor(sy));
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
        const WorldVec3 spotDir = WorldForwardFromYawPitch(spot.rotation[1], spot.rotation[0]);
        auto& out = uniform.spotLights[i];
        out.position[0] = spot.position[0];
        out.position[1] = spot.position[1];
        out.position[2] = spot.position[2];
        out.position[3] = std::max(0.1f, spot.radius);
        out.direction[0] = spotDir.x;
        out.direction[1] = spotDir.y;
        out.direction[2] = spotDir.z;
        out.direction[3] = xm::Cos(xm::DegreesToRadians(spot.innerConeDegrees));
        const float intensity = spot.enabled ? std::max(0.0f, spot.intensity) : 0.0f;
        out.color[0] = std::max(0.0f, spot.r) * intensity;
        out.color[1] = std::max(0.0f, spot.g) * intensity;
        out.color[2] = std::max(0.0f, spot.b) * intensity;
        out.color[3] = xm::Cos(xm::DegreesToRadians(spot.outerConeDegrees));
        out.direction[3] = std::max(out.direction[3], out.color[3]);
    }
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
    // Local DDS pixel-format marker (decode-time only; GPU upload always
    // goes through decoded RGBA or the CPU mip-chain builders, so no
    // compressed format ever reaches IXRHI).
    enum class Format
    {
        Unknown,
        R8G8B8A8_SRGB,
        BC1_SRGB,
        BC2_SRGB,
        BC3_SRGB,
    };

    std::string filename;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 0;
    uint32_t blockBytes = 0;
    uint32_t bytesPerPixel = 0;
    bool compressed = false;
    Format format = Format::Unknown;
    std::vector<uint8_t> pixels;
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
            out.format = DdsImage::Format::BC1_SRGB;
            out.blockBytes = 8;
            out.compressed = true;
        }
        else if (header->pixelFormat.fourCC == MakeFourCC('D', 'X', 'T', '3'))
        {
            out.format = DdsImage::Format::BC2_SRGB;
            out.blockBytes = 16;
            out.compressed = true;
        }
        else if (header->pixelFormat.fourCC == MakeFourCC('D', 'X', 'T', '5'))
        {
            out.format = DdsImage::Format::BC3_SRGB;
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
        out.format = DdsImage::Format::R8G8B8A8_SRGB;
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

        // Only the base level is retained: all DDS consumers decode to RGBA
        // (compressed) or take the base level (uncompressed); mip chains for
        // GPU arrays are regenerated on the CPU by the array builders.
        if (mip == 0)
            out.pixels.assign(bytes.begin() + offset, bytes.begin() + offset + mipSize);
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
                if (image.format == DdsImage::Format::BC2_SRGB)
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
    if (dds.format != DdsImage::Format::R8G8B8A8_SRGB || dds.pixels.size() < static_cast<size_t>(dds.width) * dds.height * 4u)
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
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (ext == ".dds")
    {
        DdsImage dds{};
        return LoadDdsImage(assets, path, dds, additionalRoots) && DdsToRgba(dds, out);
    }
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp")
        return LoadStbImage(assets, path, out, additionalRoots);

    DdsImage dds{};
    if (LoadDdsImage(assets, path, dds, additionalRoots))
        return DdsToRgba(dds, out);
    return LoadStbImage(assets, path, out, additionalRoots);
}

// The water reflection's size as a divisor of its view's size.
std::uint32_t WaterReflectionDivisor(WaterConfig::ReflectionQuality quality)
{
    switch (quality)
    {
    case WaterConfig::ReflectionQuality::Quarter: return 4u;
    case WaterConfig::ReflectionQuality::Full: return 1u;
    case WaterConfig::ReflectionQuality::Half: break;
    }
    return 2u;
}

bool TerrainAabbOutsideCameraFrustum(const WorldCamera& camera, WorldVec3 min, WorldVec3 max)
{
    const std::array<WorldVec3, 8> corners = {{
        {min.x, min.y, min.z},
        {max.x, min.y, min.z},
        {min.x, max.y, min.z},
        {max.x, max.y, min.z},
        {min.x, min.y, max.z},
        {max.x, min.y, max.z},
        {min.x, max.y, max.z},
        {max.x, max.y, max.z},
    }};

    bool outsideLeft = true;
    bool outsideRight = true;
    bool outsideBottom = true;
    bool outsideTop = true;
    bool outsideNear = true;
    bool outsideFar = true;
    const float* m = camera.viewProjection.m;
    for (const WorldVec3& p : corners)
    {
        const float clipX = p.x * m[0] + p.y * m[4] + p.z * m[8] + m[12];
        const float clipY = p.x * m[1] + p.y * m[5] + p.z * m[9] + m[13];
        const float clipZ = p.x * m[2] + p.y * m[6] + p.z * m[10] + m[14];
        const float clipW = p.x * m[3] + p.y * m[7] + p.z * m[11] + m[15];
        outsideLeft = outsideLeft && (clipX < -clipW);
        outsideRight = outsideRight && (clipX > clipW);
        outsideBottom = outsideBottom && (clipY < -clipW);
        outsideTop = outsideTop && (clipY > clipW);
        outsideNear = outsideNear && (clipZ < 0.0f);
        outsideFar = outsideFar && (clipZ > clipW);
    }
    return outsideLeft || outsideRight || outsideBottom || outsideTop || outsideNear || outsideFar;
}

// A shadow cascade's light projection is orthographic along the sun: a caster outside the cascade's
// side planes cannot shadow anything inside it. Near/far are left to the rasterizer, as before.
bool TerrainAabbOutsideCascadeFootprint(const WorldMat4& viewProjection, WorldVec3 min, WorldVec3 max)
{
    bool outsideLeft = true;
    bool outsideRight = true;
    bool outsideBottom = true;
    bool outsideTop = true;
    const float* m = viewProjection.m;
    for (int corner = 0; corner < 8; ++corner)
    {
        const float x = (corner & 1) ? max.x : min.x;
        const float y = (corner & 2) ? max.y : min.y;
        const float z = (corner & 4) ? max.z : min.z;
        const float clipX = x * m[0] + y * m[4] + z * m[8] + m[12];
        const float clipY = x * m[1] + y * m[5] + z * m[9] + m[13];
        const float clipW = x * m[3] + y * m[7] + z * m[11] + m[15];
        outsideLeft = outsideLeft && (clipX < -clipW);
        outsideRight = outsideRight && (clipX > clipW);
        outsideBottom = outsideBottom && (clipY < -clipW);
        outsideTop = outsideTop && (clipY > clipW);
    }
    return outsideLeft || outsideRight || outsideBottom || outsideTop;
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

// Three single-channel layers (ambient occlusion, roughness, metallic) as one RGBA image (A = 255):
// the terrain shader reads all three with one fetch per layer and projection.
std::vector<uint8_t> PackOcclusionRoughnessMetallic(const std::vector<uint8_t>& ao,
                                                    const std::vector<uint8_t>& roughness,
                                                    const std::vector<uint8_t>& metallic)
{
    std::vector<uint8_t> out(ao.size() * 4u, 255);
    for (size_t i = 0; i < ao.size(); ++i)
    {
        out[i * 4u + 0] = ao[i];
        out[i * 4u + 1] = i < roughness.size() ? roughness[i] : 128;
        out[i * 4u + 2] = i < metallic.size() ? metallic[i] : 0;
    }
    return out;
}

// A tileable ripple normal map, Y-up encoded (g = up, as Water.hlsl reads it). It is the slope field
// of a height made of many waves with integer wave vectors (so the tile wraps seamlessly), random
// directions and phases, and an amplitude falling with frequency. Two crossing sine waves (the old
// generator) have two dominant directions: tiled across the water they drew a regular grid.
std::vector<std::uint8_t> GenerateWaterNormalPixels(uint32_t width,
                                                    uint32_t height,
                                                    std::uint32_t seed,
                                                    float minCyclesPerTile,
                                                    float maxCyclesPerTile,
                                                    float rmsSlope)
{
    struct Wave
    {
        float kx = 0.0f;
        float ky = 0.0f;
        float amplitude = 0.0f;
        float phase = 0.0f;
    };
    constexpr float twoPi = xm::Pi * 2.0f;
    std::uint32_t state = seed * 747796405u + 2891336453u;
    const auto random01 = [&state]() {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>((state >> 8u) & 0xFFFFFFu) / 16777215.0f;
    };
    std::vector<Wave> waves;
    for (int attempt = 0; attempt < 512 && waves.size() < 56u; ++attempt)
    {
        const float angle = random01() * twoPi;
        // Log-uniform over the band: as many short as long ripples per octave.
        const float cycles = minCyclesPerTile * std::pow(maxCyclesPerTile / minCyclesPerTile, random01());
        const float kx = std::round(std::cos(angle) * cycles);
        const float ky = std::round(std::sin(angle) * cycles);
        if (kx == 0.0f && ky == 0.0f)
            continue;
        const float k = std::sqrt(kx * kx + ky * ky);
        waves.push_back({kx, ky, 1.0f / std::pow(k, 1.6f), random01() * twoPi});
    }

    const size_t pixelCount = static_cast<size_t>(width) * height;
    std::vector<float> slopeX(pixelCount, 0.0f);
    std::vector<float> slopeZ(pixelCount, 0.0f);
    double sumSquares = 0.0;
    for (uint32_t y = 0; y < height; ++y)
    {
        for (uint32_t x = 0; x < width; ++x)
        {
            const float u = static_cast<float>(x) / static_cast<float>(width);
            const float v = static_cast<float>(y) / static_cast<float>(height);
            float dx = 0.0f;
            float dz = 0.0f;
            for (const Wave& wave : waves)
            {
                const float c = std::cos(twoPi * (wave.kx * u + wave.ky * v) + wave.phase) * wave.amplitude * twoPi;
                dx += c * wave.kx;
                dz += c * wave.ky;
            }
            const size_t index = static_cast<size_t>(y) * width + x;
            slopeX[index] = dx;
            slopeZ[index] = dz;
            sumSquares += static_cast<double>(dx) * dx + static_cast<double>(dz) * dz;
        }
    }
    const float rms = static_cast<float>(std::sqrt(sumSquares / std::max<double>(1.0, 2.0 * pixelCount)));
    const float scale = rms > 0.0f ? rmsSlope / rms : 0.0f;

    std::vector<std::uint8_t> pixels(pixelCount * 4u, 255);
    for (size_t index = 0; index < pixelCount; ++index)
    {
        const WorldVec3 n = xm::Normalize(WorldVec3{-slopeX[index] * scale, 1.0f, -slopeZ[index] * scale});
        pixels[index * 4u + 0] = static_cast<std::uint8_t>(std::clamp(n.x * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
        pixels[index * 4u + 1] = static_cast<std::uint8_t>(std::clamp(n.y * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
        pixels[index * 4u + 2] = static_cast<std::uint8_t>(std::clamp(n.z * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
    return pixels;
}

// Standard tangent-space normal maps keep "up" in blue; the water shader reads green as up (like the
// generated ripple maps). Swap them so authored water normal maps tilt the surface as intended.
void ConvertTangentNormalMapToYUp(std::vector<std::uint8_t>& pixels)
{
    for (size_t offset = 0; offset + 3u < pixels.size(); offset += 4u)
        std::swap(pixels[offset + 1], pixels[offset + 2]);
}

// Loads SPIR-V words via the asset reader (aborts like the old shader loader
// when the asset is missing or misaligned).
std::vector<std::uint32_t> ReadSpirv(client::asset::IAssetReader& assets, const std::string& path)
{
    auto bytes = assets.ReadAll(path);
    if (!bytes || bytes->empty() || bytes->size() % sizeof(std::uint32_t) != 0)
    {
        Tracenf("[TERRAIN] failed to read shader: %s", path.c_str());
        std::abort();
    }
    const auto* words = reinterpret_cast<const std::uint32_t*>(bytes->data());
    return std::vector<std::uint32_t>(words, words + bytes->size() / sizeof(std::uint32_t));
}

std::shared_ptr<ixrhi::IXRHIShader> LoadShader(ixrhi::IXRHIDevice& rhi,
                                               client::asset::IAssetReader& assets,
                                               const std::string& path,
                                               ixrhi::IXRHIShaderStage stage,
                                               const char* entry)
{
    ixrhi::IXRHIShaderDesc desc;
    desc.stage = stage;
    desc.entryPoint = entry;
    desc.spirv = ReadSpirv(assets, path);
    desc.debugName = path;
    return rhi.CreateShader(desc);
}

std::shared_ptr<ixrhi::IXRHIBuffer> CreateRhiBuffer(ixrhi::IXRHIDevice& rhi,
                                                    std::uint64_t sizeBytes,
                                                    ixrhi::IXRHIBufferUsage usage,
                                                    const void* initialData,
                                                    const char* debugName)
{
    ixrhi::IXRHIBufferDesc desc;
    desc.sizeBytes = sizeBytes;
    desc.usage = usage;
    desc.cpuAccess = ixrhi::IXRHICpuAccess::Write;
    desc.debugName = debugName ? debugName : "";
    const std::size_t bytes = static_cast<std::size_t>(sizeBytes);
    return rhi.CreateBuffer(desc, initialData, initialData != nullptr ? bytes : 0);
}

// Device-local, for data the CPU never touches after creation. A host-visible buffer lives in system
// memory: the GPU then reads it over the bus in every pass that draws it (the terrain index buffer is
// drawn by the main pass and each shadow cascade).
std::shared_ptr<ixrhi::IXRHIBuffer> CreateStaticRhiBuffer(ixrhi::IXRHIDevice& rhi,
                                                          std::uint64_t sizeBytes,
                                                          ixrhi::IXRHIBufferUsage usage,
                                                          const void* initialData,
                                                          const char* debugName)
{
    ixrhi::IXRHIBufferDesc desc;
    desc.sizeBytes = sizeBytes;
    desc.usage = usage;
    desc.cpuAccess = ixrhi::IXRHICpuAccess::None;
    desc.debugName = debugName ? debugName : "";
    const std::size_t bytes = static_cast<std::size_t>(sizeBytes);
    return rhi.CreateBuffer(desc, initialData, initialData != nullptr ? bytes : 0);
}

std::shared_ptr<ixrhi::IXRHISampler> CreateRhiSampler(ixrhi::IXRHIDevice& rhi,
                                                      ixrhi::IXRHISamplerAddress addressMode,
                                                      bool trilinear,
                                                      float maxLod,
                                                      float mipLodBias,
                                                      const char* debugName)
{
    const ixrhi::IXRHICapabilities& caps = rhi.GetCapabilities();
    ixrhi::IXRHISamplerDesc desc;
    desc.minFilter = ixrhi::IXRHISamplerFilter::Linear;
    desc.magFilter = ixrhi::IXRHISamplerFilter::Linear;
    desc.mipmapFilter = trilinear ? ixrhi::IXRHISamplerFilter::Linear : ixrhi::IXRHISamplerFilter::Nearest;
    desc.addressU = addressMode;
    desc.addressV = addressMode;
    desc.addressW = addressMode;
    // Anisotropy only for repeat (tiling) samplers on capable hardware —
    // parity with the pre-migration sampler setup, via capabilities instead
    // of native device limits.
    if (addressMode == ixrhi::IXRHISamplerAddress::Repeat && caps.supportsAnisotropy)
        desc.maxAnisotropy = caps.maxAnisotropy;
    desc.maxLod = maxLod;
    desc.mipLodBias = mipLodBias;
    desc.debugName = debugName ? debugName : "";
    return rhi.CreateSampler(desc);
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
    return value <= 0.04045f ? value / 12.92f : xm::Pow((value + 0.055f) / 1.055f, 2.4f);
}

float LinearToSrgb(float value)
{
    value = std::clamp(value, 0.0f, 1.0f);
    return value <= 0.0031308f ? value * 12.92f : 1.055f * xm::Pow(value, 1.0f / 2.4f) - 0.055f;
}

uint8_t QuantizeByte(float value)
{
    return static_cast<uint8_t>(std::clamp(std::lround(value * 255.0f), 0l, 255l));
}

struct ArrayMipUpload
{
    // Tight layer-major/mip-minor packing consumed directly by
    // IXRHITexture creation (same layout the backend re-derives).
    std::vector<uint8_t> pixels;
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

    const size_t baseLayerBytes = static_cast<size_t>(width) * height * 4u;
    for (uint32_t layer = 0; layer < layers; ++layer)
    {
        std::vector<uint8_t> current(baseLayerBytes);
        std::memcpy(current.data(), basePixels.data() + static_cast<size_t>(layer) * baseLayerBytes, baseLayerBytes);
        uint32_t mipWidth = width;
        uint32_t mipHeight = height;

        for (uint32_t mip = 0; mip < upload.mipLevels; ++mip)
        {
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
                        const float invLen = len2 > 0.000001f ? 1.0f / xm::Sqrt(len2) : 1.0f;
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

    const size_t baseLayerBytes = static_cast<size_t>(width) * height;
    for (uint32_t layer = 0; layer < layers; ++layer)
    {
        std::vector<uint8_t> current(baseLayerBytes);
        std::memcpy(current.data(), basePixels.data() + static_cast<size_t>(layer) * baseLayerBytes, baseLayerBytes);
        uint32_t mipWidth = width;
        uint32_t mipHeight = height;

        for (uint32_t mip = 0; mip < upload.mipLevels; ++mip)
        {
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

uint32_t EstimateActiveSplatLayerSpan(const std::vector<uint8_t>& splatA,
                                      const std::vector<uint8_t>& splatB,
                                      uint32_t width,
                                      uint32_t height)
{
    const size_t texelCount = static_cast<size_t>(width) * height;
    if (texelCount == 0 ||
        splatA.size() < texelCount * 4u ||
        splatB.size() < texelCount * 4u)
    {
        return 1u;
    }

    uint32_t highestActiveLayer = 0;
    for (size_t texel = 0; texel < texelCount; ++texel)
    {
        const size_t byte = texel * 4u;
        for (uint32_t i = 0; i < 4; ++i)
        {
            if (splatA[byte + i] > 0)
                highestActiveLayer = std::max(highestActiveLayer, i);
            if (splatB[byte + i] > 0)
                highestActiveLayer = std::max(highestActiveLayer, 4u + i);
        }
    }

    return std::clamp(highestActiveLayer + 1u, 1u, 8u);
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
            const float normalY = 1.0f / xm::Sqrt(1.0f + dhdx * dhdx + dhdz * dhdz);
            const float slope = 1.0f - std::clamp(normalY, 0.0f, 1.0f);
            const float blend = Smoothstep(slopeThreshold, slopeThreshold + slopeTransition, slope);
            axesTotal += 1.0 + 2.0 * static_cast<double>(blend);
            ++samples;
        }
    }

    return samples == 0 ? 1.0 : axesTotal / static_cast<double>(samples);
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

WorldVec3 TransformWorldPointNoPerspective(const WorldMat4& m, WorldVec3 p)
{
    return {
        p.x * m.m[0] + p.y * m.m[4] + p.z * m.m[8] + m.m[12],
        p.x * m.m[1] + p.y * m.m[5] + p.z * m.m[9] + m.m[13],
        p.x * m.m[2] + p.y * m.m[6] + p.z * m.m[10] + m.m[14]};
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

float SampleTerrainCollisionHeightCm(const std::vector<float>& grid,
                                    std::uint32_t width, std::uint32_t height,
                                    float localXcm, float localYcm, float cellScaleCm)
{
    if (width < 2 || height < 2 || grid.size() < static_cast<std::size_t>(width) * height ||
        !std::isfinite(localXcm) || !std::isfinite(localYcm) ||
        !std::isfinite(cellScaleCm) || cellScaleCm <= 0)
        return 0.0f;
    const float maxX = static_cast<float>(width - 1) * cellScaleCm;
    const float maxY = static_cast<float>(height - 1) * cellScaleCm;
    if (!std::isfinite(maxX) || !std::isfinite(maxY))
        return 0.0f;
    const float gx = std::clamp(localXcm, 0.0f, maxX) / cellScaleCm;
    const float gy = std::clamp(localYcm, 0.0f, maxY) / cellScaleCm;
    const std::uint32_t x0 = std::min(static_cast<std::uint32_t>(gx), width - 2);
    const std::uint32_t y0 = std::min(static_cast<std::uint32_t>(gy), height - 2);
    const float tx = std::clamp(gx - static_cast<float>(x0), 0.0f, 1.0f);
    const float ty = std::clamp(gy - static_cast<float>(y0), 0.0f, 1.0f);
    const auto offset = static_cast<std::size_t>(y0) * width + x0;
    const float h00 = grid[offset];
    const float h10 = grid[offset + 1];
    const float h01 = grid[offset + width];
    const float h11 = grid[offset + width + 1];
    // Match PhysicsWorld::CreateTerrainCollider: source diagonal v10-v01.
    if (tx + ty <= 1.0f)
        return h00 + (h10 - h00) * tx + (h01 - h00) * ty;
    return h11 + (h01 - h11) * (1.0f - tx) + (h10 - h11) * (1.0f - ty);
}

bool TerrainRenderer::Create(ixrhi::IXRHIDevice& rhi, client::asset::IAssetReader& assets)
{
    Destroy();
    m_rhi = &rhi;
    m_assets = &assets;
    LoadEditorConfig();

    const bool texture = CreateFallbackTexture(rhi);
    const bool mask = texture ? CreateFallbackSplatTextures(rhi) : false;
    const bool buffers = mask ? CreateBuffers(rhi) : false;
    const bool shadows = buffers ? CreateShadowResources(rhi) : false;
    const bool descriptors = shadows ? CreateBindGroup(rhi) : false;
    const bool pipeline = descriptors ? CreatePipeline(rhi) : false;
    const bool water = pipeline ? CreateWaterResources(rhi) : false;
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

bool TerrainRenderer::LoadMap(ixrhi::IXRHIDevice& rhi, const std::string& mapDirectory, int32_t serverX, int32_t serverY)
{
    if (!m_rhi)
        return false;

    rhi.WaitIdle();
    m_vertexBuffer.reset();
    m_vertexEditBuffer.reset();
    m_indexBuffer.reset();
    m_debugVertexBuffer.reset();
    m_debugIndexBuffer.reset();
    m_logicVertexBuffer.reset();
    m_logicIndexBuffer.reset();
    m_selectedWaterBodyVertexBuffer.reset();
    m_selectedWaterBodyIndexBuffer.reset();
    DestroyTerrainLayers();
    m_indexCount = 0;
    m_debugIndexCount = 0;
    m_spawnDebugIndexOffset = 0;
    m_spawnDebugIndexCount = 0;
    m_logicDebugIndexOffset = 0;
    m_logicDebugIndexCount = 0;
    m_terrainChunks.clear();
    m_visibleTerrainChunksScratch.clear();
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

    if (!CreateMapBuffers(rhi, mapDirectory, serverX, serverY))
    {
        Tracen("[TERRAIN-MAP] LoadMap failed; restoring flat fallback terrain");
        m_mapLoaded = false;
        m_heightCmGrid.clear();
        m_attributes.clear();
        m_splatABytes.clear();
        m_splatBBytes.clear();
        m_activeSplatLayerSpan = -1;
        m_splatWidth = 0;
        m_splatHeight = 0;
        m_chunkSplatWidth = 0;
        m_chunkSplatHeight = 0;
        m_terrainChunks.clear();
        m_visibleTerrainChunksScratch.clear();
        m_undoStack.clear();
        const bool flat = CreateFlatBuffers(rhi);
        LoadWaterBodies(rhi, mapDirectory);
        CreateBindGroup(rhi);
        return flat;
    }

    LoadWaterBodies(rhi, mapDirectory);
    CreateBindGroup(rhi);
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

bool TerrainRenderer::CreateFlatTerrain(ixrhi::IXRHIDevice& rhi, const TerrainSceneData& terrain)
{
    if (!m_rhi)
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
    const std::size_t expectedHeightCount =
        (static_cast<std::size_t>(next.cellsX) + 1) * (static_cast<std::size_t>(next.cellsZ) + 1);
    if (!next.heightCmGrid.empty() && next.heightCmGrid.size() != expectedHeightCount)
    {
        TraceError("[TERRAIN-CREATE] invalid height count: expected=%zu actual=%zu",
            expectedHeightCount, next.heightCmGrid.size());
        return false;
    }
    if (!std::all_of(next.heightCmGrid.begin(), next.heightCmGrid.end(),
        [](float value) { return std::isfinite(value); }))
    {
        TraceError("[TERRAIN-CREATE] non-finite terrain height");
        return false;
    }
    const std::size_t expectedAttributeCount = static_cast<std::size_t>(next.cellsX) * next.cellsZ;
    if (!next.attributes.empty() && next.attributes.size() != expectedAttributeCount)
    {
        TraceError("[TERRAIN-CREATE] invalid attribute count: expected=%zu actual=%zu",
            expectedAttributeCount, next.attributes.size());
        return false;
    }

    rhi.WaitIdle();
    m_vertexBuffer.reset();
    m_vertexEditBuffer.reset();
    m_indexBuffer.reset();
    m_debugVertexBuffer.reset();
    m_debugIndexBuffer.reset();
    m_logicVertexBuffer.reset();
    m_logicIndexBuffer.reset();
    m_selectedWaterBodyVertexBuffer.reset();
    m_selectedWaterBodyIndexBuffer.reset();
    DestroyTerrainLayers();
    DestroyWaterBodyResources();
    m_splatA = {};
    m_splatB = {};
    m_indexCount = 0;
    m_debugIndexCount = 0;
    m_selectedWaterBodyIndexCount = 0;
    m_selectedWaterBodyId = 0;
    m_terrainChunks.clear();
    m_visibleTerrainChunksScratch.clear();
    m_waterBodies.clear();
    m_heightCmGrid.clear();
    m_attributes.clear();
    m_splatABytes.clear();
    m_splatBBytes.clear();
    m_activeSplatLayerSpan = -1;
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
    if (next.heightCmGrid.size() == expectedHeightCount)
        m_heightCmGrid = next.heightCmGrid;
    else
        m_heightCmGrid.assign(expectedHeightCount, 0.0f);
    m_attributes = next.attributes;
    if (m_attributes.empty())
        m_attributes.assign(expectedAttributeCount, 0);
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
    m_activeSplatLayerSpan = -1;
    m_splatUndoRecorded.assign(m_splatABytes.size() + m_splatBBytes.size(), 0);
    const uint32_t chunksX = (m_mapSizeX + m_chunkSizeCells - 1u) / m_chunkSizeCells;
    const uint32_t chunksY = (m_mapSizeY + m_chunkSizeCells - 1u) / m_chunkSizeCells;
    m_dirtyChunkTexels.assign(static_cast<size_t>(chunksX) * chunksY, 0);

    if (!CreateFlatBuffers(rhi))
        return false;
    if (!CreateSceneSplatTextures(rhi))
        return false;
    CreateBindGroup(rhi);

    m_mapLoaded = true;
    m_sceneTerrainActive = true;
    m_sceneTerrain = next;
    ReleaseTerrainGrids(m_sceneTerrain);
    m_triplanarParamsDirty = true;
    m_terrainShaderOptimDiagLogged = false;
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

void TerrainRenderer::ClearTerrain()
{
    if (!m_rhi)
        return;
    m_rhi->WaitIdle();
    m_vertexBuffer.reset();
    m_vertexEditBuffer.reset();
    m_indexBuffer.reset();
    m_debugVertexBuffer.reset();
    m_debugIndexBuffer.reset();
    m_logicVertexBuffer.reset();
    m_logicIndexBuffer.reset();
    m_selectedWaterBodyVertexBuffer.reset();
    m_selectedWaterBodyIndexBuffer.reset();
    DestroyTerrainLayers();
    DestroyWaterBodyResources();
    m_splatA = {};
    m_splatB = {};
    m_waterBodies.clear();
    m_heightCmGrid.clear();
    m_attributes.clear();
    m_splatABytes.clear();
    m_splatBBytes.clear();
    m_activeSplatLayerSpan = -1;
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
    m_terrainShaderOptimDiagLogged = false;
    m_sceneTerrain = {};
    if (m_rhi)
        CreateBindGroup(*m_rhi);
    Tracenf("[TEDIT-DIAG] active terrain cleared id=%p activeTerrain=NULL", static_cast<void*>(this));
}

TerrainSceneData TerrainRenderer::GetTerrainSceneInfo() const
{
    TerrainSceneData data = m_sceneTerrain;  // holds no grids (ReleaseTerrainGrids)
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
    }
    return data;
}

TerrainSceneData TerrainRenderer::GetTerrainSceneData() const
{
    TerrainSceneData data;
    GetTerrainSceneData(data);
    return data;
}

void TerrainRenderer::GetTerrainSceneData(TerrainSceneData& out) const
{
    // Keep out's grid buffers: assigning into them reuses their capacity, so a caller refreshing the
    // same snapshot every frame copies megabytes instead of allocating them.
    std::vector<float> heights = std::move(out.heightCmGrid);
    std::vector<std::uint16_t> attributes = std::move(out.attributes);
    std::vector<std::uint8_t> splatA = std::move(out.splatABytes);
    std::vector<std::uint8_t> splatB = std::move(out.splatBBytes);
    out = GetTerrainSceneInfo();
    if (!out.exists)
        return;
    heights.assign(m_heightCmGrid.begin(), m_heightCmGrid.end());
    attributes.assign(m_attributes.begin(), m_attributes.end());
    splatA.assign(m_splatABytes.begin(), m_splatABytes.end());
    splatB.assign(m_splatBBytes.begin(), m_splatBBytes.end());
    out.heightCmGrid = std::move(heights);
    out.attributes = std::move(attributes);
    out.splatABytes = std::move(splatA);
    out.splatBBytes = std::move(splatB);
}

void TerrainRenderer::SetTerrainSceneData(const TerrainSceneData& terrain)
{
    if (!m_sceneTerrainActive)
        return;
    m_sceneTerrain = terrain;
    ReleaseTerrainGrids(m_sceneTerrain);
    m_sceneTerrain.exists = true;
    m_sceneTerrain.triplanarSharpness = std::clamp(m_sceneTerrain.triplanarSharpness, 1.0f, 16.0f);
    m_sceneTerrain.triplanarSlopeThreshold = std::clamp(m_sceneTerrain.triplanarSlopeThreshold, 0.0f, 1.0f);
    m_sceneTerrain.triplanarSlopeTransition = std::clamp(m_sceneTerrain.triplanarSlopeTransition, 0.001f, 1.0f);
    m_triplanarParamsDirty = true;
}

bool TerrainRenderer::RecreatePipeline(ixrhi::IXRHIDevice& rhi)
{
    if (!m_rhi)
        return true;

    DestroyPipeline();
    DestroyWaterReflectionPipeline();
    // Deferred-true while the effective pass is torn down (parity with the
    // pre-migration null-pass early-out); real failures propagate as false.
    const ixrhi::IXRHIRenderPass* effectivePass = m_targetPass ? m_targetPass : rhi.GetMainPass();
    if (effectivePass == nullptr)
        return true;

    const bool terrainPipeline = CreatePipeline(rhi);
    const bool reflectionResources = terrainPipeline ? CreateOrRecreateWaterReflectionResources(rhi, true) : false;
    const bool reflectionPipeline = reflectionResources ? CreateWaterReflectionPipeline(rhi) : false;
    if (reflectionPipeline)
        UpdateWaterBindGroup();
    const bool waterPipeline = reflectionPipeline ? CreateWaterPipeline(rhi) : false;
    return terrainPipeline && reflectionResources && reflectionPipeline && waterPipeline;
}

void TerrainRenderer::SetTargetPass(const ixrhi::IXRHIRenderPass* pass)
{
    m_targetPass = pass;
}

void TerrainRenderer::SetWaterRefractionInputs(std::shared_ptr<ixrhi::IXRHITexture> colorSnapshot,
                                               std::shared_ptr<ixrhi::IXRHITexture> depthSnapshot,
                                               std::shared_ptr<ixrhi::IXRHISampler> sampler,
                                               std::uint32_t width,
                                               std::uint32_t height)
{
    if (m_waterSceneColor == colorSnapshot && m_waterSceneDepth == depthSnapshot &&
        m_waterSceneSampler == sampler && m_waterSceneWidth == width &&
        m_waterSceneHeight == height)
    {
        return;
    }

    m_waterSceneColor = std::move(colorSnapshot);
    m_waterSceneDepth = std::move(depthSnapshot);
    m_waterSceneSampler = std::move(sampler);
    m_waterSceneWidth = width;
    m_waterSceneHeight = height;
    UpdateWaterBindGroup();
}

void TerrainRenderer::UpdateShadowCascades(const WorldCamera& camera)
{
    const WorldVec3 forward = WorldCameraForward(camera);
    WorldVec3 right = WorldCameraRight(camera);
    if (xm::Dot(right, right) <= 0.0001f)
        right = {1.0f, 0.0f, 0.0f};
    const WorldVec3 up = WorldCameraUp(camera);
    const float aspect = 16.0f / 9.0f;
    constexpr float tanHalfFov = 0.41421356237f;
    const float nearPlane = 0.1f;
    const float farPlane = 200.0f;
    constexpr float lambda = 0.7f;

    float splitPlanes[kShadowCascadeCount + 1]{};
    splitPlanes[0] = nearPlane;
    for (uint32_t i = 1; i < kShadowCascadeCount; ++i)
    {
        const float p = static_cast<float>(i) / static_cast<float>(kShadowCascadeCount);
        const float logSplit = nearPlane * xm::Pow(farPlane / nearPlane, p);
        const float uniformSplit = nearPlane + (farPlane - nearPlane) * p;
        splitPlanes[i] = uniformSplit * (1.0f - lambda) + logSplit * lambda;
    }
    splitPlanes[kShadowCascadeCount] = farPlane;

    const DirectionalLight& sun = m_lightingState.directional;
    const float azimuthRadians = xm::DegreesToRadians(std::clamp(sun.azimuthDegrees, 0.0f, 360.0f));
    const float elevationRadians = xm::DegreesToRadians(std::clamp(sun.elevationDegrees, 0.0f, 90.0f));
    WorldVec3 sunDir = WorldDirectionFromAzimuthElevation(azimuthRadians, elevationRadians);
    if (xm::Dot(sunDir, sunDir) <= 0.0001f)
        sunDir = {0.0f, 1.0f, 0.0f};

    for (uint32_t cascade = 0; cascade < kShadowCascadeCount; ++cascade)
    {
        const float zn = splitPlanes[cascade];
        const float zf = splitPlanes[cascade + 1];
        const float nearH = 2.0f * tanHalfFov * zn;
        const float nearW = nearH * aspect;
        const float farH = 2.0f * tanHalfFov * zf;
        const float farW = farH * aspect;
        const WorldVec3 nearCenter = camera.eye + forward * zn;
        const WorldVec3 farCenter = camera.eye + forward * zf;
        std::array<WorldVec3, 8> corners = {
            nearCenter + up * (nearH * 0.5f) + right * (-nearW * 0.5f),
            nearCenter + up * (nearH * 0.5f) + right * (nearW * 0.5f),
            nearCenter + up * (-nearH * 0.5f) + right * (-nearW * 0.5f),
            nearCenter + up * (-nearH * 0.5f) + right * (nearW * 0.5f),
            farCenter + up * (farH * 0.5f) + right * (-farW * 0.5f),
            farCenter + up * (farH * 0.5f) + right * (farW * 0.5f),
            farCenter + up * (-farH * 0.5f) + right * (-farW * 0.5f),
            farCenter + up * (-farH * 0.5f) + right * (farW * 0.5f),
        };

        WorldVec3 center{};
        for (WorldVec3 corner : corners)
            center = center + corner;
        center = center * (1.0f / static_cast<float>(corners.size()));

        // The light camera sits on the sun's side and looks away from it, so depth grows with the
        // distance from the sun and the shadow map keeps the surface the sun reaches first.
        const WorldVec3 lightEye = center + sunDir * 120.0f;
        WorldMat4 lightView = WorldLookAt(lightEye, center, {0.0f, 1.0f, 0.0f});

        WorldVec3 minBound{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
        WorldVec3 maxBound{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
        for (WorldVec3 corner : corners)
        {
            const WorldVec3 p = TransformWorldPointNoPerspective(lightView, corner);
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

void TerrainRenderer::UploadEditedTerrain(ixrhi::IXRHICommandList& cmd, const ixrhi::IXRHIFrameInfo& frame)
{
    if (m_vertexBufferUploadPending && m_vertexBuffer && m_vertexEditBuffer)
    {
        m_vertexBufferUploadPending = false;
        ++m_terrainGeometryRevision;  // the sun shadow map must be drawn again
        // Whole-buffer copy (the backend copies from offset 0): only on frames with a sculpt edit. The
        // barriers order it after earlier frames' vertex reads and before this frame's draws.
        cmd.TransitionBuffer(*m_vertexBuffer, ixrhi::IXRHIBufferState::VertexRead, ixrhi::IXRHIBufferState::TransferDst);
        cmd.CopyBuffer(*m_vertexEditBuffer, *m_vertexBuffer, std::min(m_vertexEditBuffer->SizeBytes(), m_vertexBuffer->SizeBytes()));
        cmd.TransitionBuffer(*m_vertexBuffer, ixrhi::IXRHIBufferState::TransferDst, ixrhi::IXRHIBufferState::VertexRead);
    }
    UploadEditedSplat(cmd, frame.frameIndex % kFramesInFlight);
}

void TerrainRenderer::UploadEditedSplat(ixrhi::IXRHICommandList& cmd, uint32_t frameIndex)
{
    if (!m_editorSplatGpuDirty || !m_splatDirtyRect.Valid() || !m_rhi || !m_splatA.image || !m_splatB.image)
        return;
    const SplatDirtyRect rect = m_splatDirtyRect;
    m_splatDirtyRect = {};
    m_editorSplatGpuDirty = false;

    const std::size_t layerBytes = static_cast<std::size_t>(m_splatWidth) * m_splatHeight * 4u;
    if (m_splatA.width != m_splatWidth || m_splatA.height != m_splatHeight ||
        m_splatB.width != m_splatWidth || m_splatB.height != m_splatHeight ||
        m_splatABytes.size() != layerBytes || m_splatBBytes.size() != layerBytes ||
        rect.maxX >= m_splatWidth || rect.maxY >= m_splatHeight)
    {
        RefreshSplatTextures(*m_rhi);  // unexpected layout: the whole-texture path
        return;
    }

    // This frame slot's staging buffer: the GPU copy that last read it (kFramesInFlight frames ago)
    // is finished, since the frame began after waiting on that slot's fence.
    std::shared_ptr<ixrhi::IXRHIBuffer>& staging = m_splatStaging[frameIndex];
    if (!staging || staging->SizeBytes() < 2u * layerBytes)
    {
        staging = CreateRhiBuffer(*m_rhi,
            2u * layerBytes,
            ixrhi::IXRHIBufferUsage::TransferSrc,
            nullptr,
            "Terrain:SplatStaging");
        if (!staging)
        {
            RefreshSplatTextures(*m_rhi);
            return;
        }
    }

    // Only the painted rectangle: its rows go to the same place in the staging buffer as in the CPU
    // copy (A at 0, B after it), and the copies read them with the full row pitch.
    const uint32_t width = rect.maxX - rect.minX + 1u;
    const uint32_t height = rect.maxY - rect.minY + 1u;
    const std::size_t rowBytes = static_cast<std::size_t>(width) * 4u;
    for (uint32_t y = rect.minY; y <= rect.maxY; ++y)
    {
        const std::size_t offset = (static_cast<std::size_t>(y) * m_splatWidth + rect.minX) * 4u;
        staging->Write(offset, m_splatABytes.data() + offset, rowBytes);
        staging->Write(layerBytes + offset, m_splatBBytes.data() + offset, rowBytes);
    }
    const std::uint64_t firstTexel = (static_cast<std::uint64_t>(rect.minY) * m_splatWidth + rect.minX) * 4u;
    using L = ixrhi::IXRHIImageLayout;
    cmd.TransitionTexture(*m_splatA.image, L::ShaderReadOnly, L::TransferDst);
    cmd.TransitionTexture(*m_splatB.image, L::ShaderReadOnly, L::TransferDst);
    cmd.CopyBufferToTexture(*staging, firstTexel, m_splatWidth, *m_splatA.image, 0, 0,
        rect.minX, rect.minY, width, height);
    cmd.CopyBufferToTexture(*staging, layerBytes + firstTexel, m_splatWidth, *m_splatB.image, 0, 0,
        rect.minX, rect.minY, width, height);
    cmd.TransitionTexture(*m_splatA.image, L::TransferDst, L::ShaderReadOnly);
    cmd.TransitionTexture(*m_splatB.image, L::TransferDst, L::ShaderReadOnly);
}

void TerrainRenderer::RenderSunShadowMap(ixrhi::IXRHICommandList& cmd,
                                          const ixrhi::IXRHIFrameInfo& frame,
                                          const WorldCamera& camera)
{
    if (!m_sceneTerrainActive || m_sceneTerrain.editorHidden || !m_lightingState.sunShadowsEnabled || !m_shadowPipeline ||
        !m_shadowTexture || !m_vertexBuffer || !m_indexBuffer || m_indexCount == 0 || !frame.frameActive)
    {
        for (PassDrawStats& cascadeStats : m_frameDrawStats.shadowCascades)
            cascadeStats.skipped = true;
        m_shadowMapInputs.reset();
        return;
    }

    const DirectionalLight& sun = m_lightingState.directional;
    const ShadowMapInputs inputs{
        {camera.eye.x, camera.eye.y, camera.eye.z},
        {camera.target.x, camera.target.y, camera.target.z},
        sun.azimuthDegrees,
        sun.elevationDegrees,
        m_vertexBuffer.get(),
        m_indexBuffer.get(),
        m_shadowTexture.get(),
        m_indexCount,
        m_terrainGeometryRevision};
    if (m_shadowMapInputs && *m_shadowMapInputs == inputs)
    {
        // Same camera, sun and terrain as the map already holds (and still shader-readable): reuse it.
        for (PassDrawStats& cascadeStats : m_frameDrawStats.shadowCascades)
            cascadeStats.skipped = true;
        m_shadowDrawnFrame = frame.frameNumber;
        return;
    }
    m_shadowMapInputs = inputs;

    UpdateShadowCascades(camera);

    if (m_sceneTerrain.triplanarEnabled && !m_triPerfShadowPassLogged)
    {
        TraceDiagf("[TRI-PERF] terrain pipeline bound in pass=shadow-cascade0..%u triplanar=no shader=depth-only extent=%ux%u",
            kShadowCascadeCount - 1u,
            kShadowResolution,
            kShadowResolution);
        m_triPerfShadowPassLogged = true;
    }

    for (uint32_t cascade = 0; cascade < kShadowCascadeCount; ++cascade)
    {
        // Cascade timestamp pairs are sequential in IXRHITimestampPoint
        // (CascadeNBegin + 1 == CascadeNEnd); routed through the backend so
        // the legacy timestamp API could be deleted (Phase 3C).
        const auto cascadeBegin = static_cast<ixrhi::IXRHITimestampPoint>(
            static_cast<std::uint32_t>(ixrhi::IXRHITimestampPoint::ShadowCascade0Begin) +
            cascade * 2u);
        const auto cascadeEnd = static_cast<ixrhi::IXRHITimestampPoint>(
            static_cast<std::uint32_t>(cascadeBegin) + 1u);
        if (m_rhi)
            m_rhi->WriteTimestamp(cascadeBegin);
        if (m_shadowTargets[cascade])
            m_shadowTargets[cascade]->Begin(cmd);
        cmd.SetViewport(0.0f, 0.0f, static_cast<float>(kShadowResolution), static_cast<float>(kShadowResolution));
        cmd.SetScissor(0, 0, kShadowResolution, kShadowResolution);
        cmd.SetGraphicsPipeline(*m_shadowPipeline);
        cmd.PushConstants(&m_shadowCascadeViewProj[cascade], sizeof(WorldMat4));
        cmd.SetVertexBuffer(0, *m_vertexBuffer, 0);
        cmd.SetIndexBuffer(*m_indexBuffer, 0, /*thirtyTwoBit=*/true);
        PassDrawStats& cascadeStats = m_frameDrawStats.shadowCascades[cascade];
        cascadeStats.executed = true;
        cascadeStats.drawCalls = 0;
        cascadeStats.chunksDrawn = 0;
        cascadeStats.chunksCulled = 0;
        if (m_terrainChunks.empty())
        {
            cmd.DrawIndexed(m_indexCount, 1, 0, 0, 0);
            cascadeStats.drawCalls = 1;
            cascadeStats.chunksDrawn = 1;
        }
        else
        {
            // Only the chunks under the cascade's footprint: the near cascades cover a small part of
            // the terrain, and every drawn chunk is vertex/index traffic. Chunks adjacent in the index
            // buffer are merged into one draw.
            uint32_t runOffset = 0;
            uint32_t runCount = 0;
            auto flushRun = [&]() {
                if (runCount == 0)
                    return;
                cmd.DrawIndexed(runCount, 1, runOffset, 0, 0);
                ++cascadeStats.drawCalls;
                runCount = 0;
            };
            for (const TerrainChunkDraw& chunk : m_terrainChunks)
            {
                if (TerrainAabbOutsideCascadeFootprint(m_shadowCascadeViewProj[cascade], chunk.worldMin, chunk.worldMax))
                {
                    ++cascadeStats.chunksCulled;
                    continue;
                }
                ++cascadeStats.chunksDrawn;
                if (runCount != 0 && runOffset + runCount == chunk.indexOffset)
                {
                    runCount += chunk.indexCount;
                    continue;
                }
                flushRun();
                runOffset = chunk.indexOffset;
                runCount = chunk.indexCount;
            }
            flushRun();
        }
        if (m_shadowTargets[cascade])
            m_shadowTargets[cascade]->End(cmd);
        if (m_rhi)
            m_rhi->WriteTimestamp(cascadeEnd);
    }

    // Depth-attachment -> sampled transition for the terrain main pass
    // (binding 9). The next shadow render re-opens the targets with
    // Clear (UNDEFINED initial), so no transition back is needed.
    cmd.TransitionTexture(*m_shadowTexture,
        ixrhi::IXRHIImageLayout::DepthStencilAttachment,
        ixrhi::IXRHIImageLayout::ShaderReadOnly);
    m_shadowDrawnFrame = frame.frameNumber;
}

WorldCamera TerrainRenderer::ComputeMirrorCamera(const WorldCamera& camera,
                                                  std::uint32_t targetWidth,
                                                  std::uint32_t targetHeight,
                                                  float waterLevelY) const
{
    // A true planar mirror: the scene reflected across the water plane, seen through the camera's
    // OWN view-projection. A point on the water then lands on the same screen position in both
    // images, so the water samples the reflection at its own screen coordinate. (Before, a separate
    // 45-degree look-up camera from below the water produced a mismatched, vertically flipped image.)
    // The geometry is mirrored, so its winding flips: the reflection pipelines draw both faces.
    (void)targetWidth;
    (void)targetHeight;
    const float waterY = waterLevelY;
    const WorldMat4 reflectAcrossWater = WorldMultiply(
        WorldMultiply(WorldTranslation(0.0f, -waterY, 0.0f), xm::Scale({1.0f, -1.0f, 1.0f})),
        WorldTranslation(0.0f, waterY, 0.0f));
    WorldCamera mirror = camera;
    mirror.eye = {camera.eye.x, 2.0f * waterY - camera.eye.y, camera.eye.z};  // lighting: the mirrored viewer
    mirror.target = {camera.target.x, 2.0f * waterY - camera.target.y, camera.target.z};
    mirror.viewProjection = WorldMultiply(reflectAcrossWater, camera.viewProjection);
    return mirror;
}

const TerrainRenderer::WaterBodyGpu* TerrainRenderer::FindClosestWaterBody(const WorldCamera& camera,
                                                                           float* outDistanceMeters) const
{
    const WaterBodyGpu* closest = nullptr;
    float closestDistanceSq = std::numeric_limits<float>::max();
    for (const WaterBodyGpu& waterBody : m_waterBodies)
    {
        if (!ResolveWaterConfig(waterBody.body).enabled || waterBody.indexCount == 0 ||
            !WaterBodyInView(waterBody, camera))
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
        *outDistanceMeters = closest ? xm::Sqrt(closestDistanceSq) : 0.0f;
    return closest;
}

bool TerrainRenderer::WaterBodyInView(const WaterBodyGpu& waterBody, const WorldCamera& camera) const
{
    // A little height around the flat surface for the wave normals' look; the mesh itself is flat.
    const WorldVec3 margin{0.5f, 0.5f, 0.5f};
    return !TerrainAabbOutsideCameraFrustum(camera, waterBody.boundsMin - margin, waterBody.boundsMax + margin);
}

bool TerrainRenderer::AnyWaterBodyInView(const WorldCamera& camera) const
{
    if (!m_sceneTerrainActive || m_sceneTerrain.editorHidden)
        return false;
    for (const WaterBodyGpu& waterBody : m_waterBodies)
    {
        if (ResolveWaterConfig(waterBody.body).enabled && waterBody.indexCount != 0 &&
            WaterBodyInView(waterBody, camera))
            return true;
    }
    return false;
}

void TerrainRenderer::RenderWaterReflection(ixrhi::IXRHICommandList& cmd,
                                              const ixrhi::IXRHIFrameInfo& frame,
                                              const WorldCamera& camera,
                                              std::uint32_t viewWidth,
                                              std::uint32_t viewHeight,
                                              double timeSeconds,
                                              const std::function<void(const WorldCamera&,
                                                                       std::uint32_t,
                                                                       std::uint32_t,
                                                                       const ixrhi::IXRHIRenderPass*,
                                                                       float)>& renderEntities)
{
    PassDrawStats& reflectionStats = m_frameDrawStats.waterReflection;
    auto skipReflection = [&]() {
        reflectionStats.skipped = true;
    };
    const WaterBodyGpu* reflectionBody = nullptr;
    float reflectionDistanceMeters = 0.0f;
    if (!m_sceneTerrainActive || m_sceneTerrain.editorHidden || m_waterBodies.empty())
    {
        skipReflection();
        return;
    }

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
        skipReflection();
        return;
    }

    const WaterConfig& reflectionConfig = ResolveWaterConfig(reflectionBody->body);
    const float reflectionWaterLevelY = reflectionBody->body.waterLevelY;
    if (!m_rhi || !reflectionConfig.enabled || !reflectionConfig.reflectionEnabled || !m_reflectionPipeline ||
        !m_bindGroup || !m_waterReflection.target || !m_indexBuffer || !m_vertexBuffer || m_indexCount == 0 ||
        !frame.frameActive)
    {
        skipReflection();
        return;
    }

    const std::uint32_t baseWidth = viewWidth > 0 ? viewWidth : m_rhi->GetMainSwapchain().Width();
    const std::uint32_t baseHeight = viewHeight > 0 ? viewHeight : m_rhi->GetMainSwapchain().Height();
    if (baseWidth == 0 || baseHeight == 0)
    {
        skipReflection();
        return;
    }

    // The reflection is a fraction of the view it is seen in (the water samples it at its own
    // screen position), so it follows that view's size.
    const std::uint32_t reflectionDivisor = WaterReflectionDivisor(reflectionConfig.reflectionQuality);
    const std::uint32_t wantedWidth = std::max(1u, baseWidth / reflectionDivisor);
    const std::uint32_t wantedHeight = std::max(1u, baseHeight / reflectionDivisor);
    if (m_waterReflection.quality != reflectionConfig.reflectionQuality ||
        m_waterReflection.width != wantedWidth || m_waterReflection.height != wantedHeight)
    {
        CreateOrRecreateWaterReflectionResources(*m_rhi, true, reflectionConfig.reflectionQuality, baseWidth, baseHeight);
        CreateWaterReflectionPipeline(*m_rhi);
        UpdateWaterBindGroup();
    }

    if (!m_waterReflection.target || !m_reflectionPipeline)
    {
        skipReflection();
        return;
    }

    const uint32_t frameIndex = frame.frameIndex % kFramesInFlight;
    const WorldCamera mirror = ComputeMirrorCamera(camera, m_waterReflection.width, m_waterReflection.height, reflectionWaterLevelY);
    m_reflectionClipWaterLevelY = reflectionWaterLevelY;
    UpdateUniform(frameIndex, mirror, true, kReflectionUniformView);

    m_waterReflection.target->Begin(cmd);
    cmd.SetViewport(0.0f, 0.0f, static_cast<float>(m_waterReflection.width), static_cast<float>(m_waterReflection.height));
    cmd.SetScissor(0, 0, m_waterReflection.width, m_waterReflection.height);
    cmd.SetGraphicsPipeline(*m_reflectionPipeline);
    if (m_sceneTerrain.triplanarEnabled && !m_triPerfReflectionPassLogged)
    {
        TraceDiagf("[TRI-PERF] terrain pipeline bound in pass=water-reflection triplanar=yes extent=%ux%u shader=terrain_ps",
            m_waterReflection.width,
            m_waterReflection.height);
        m_triPerfReflectionPassLogged = true;
    }

    cmd.SetVertexBuffer(0, *m_vertexBuffer, 0);
    cmd.SetIndexBuffer(*m_indexBuffer, 0, /*thirtyTwoBit=*/true);

    struct TerrainPushConstants
    {
        float layerParams[4];
    };

    TerrainPushConstants push{{1.0f, 1.0f, 0.0f, 0.0f}};
    cmd.PushConstants(&push, sizeof(push));
    cmd.BindGroup(0, *m_bindGroup, kReflectionUniformView * kFramesInFlight + frameIndex);
    if (m_terrainChunks.empty())
    {
        cmd.DrawIndexed(m_indexCount, 1, 0, 0, 0);
        ++reflectionStats.drawCalls;
        reflectionStats.chunksDrawn = 1;
    }
    else
    {
        // Only the chunks the mirror camera sees that reach above the water (the reflection shader
        // clips everything under it). Chunks adjacent in the index buffer share one draw.
        uint32_t runOffset = 0;
        uint32_t runCount = 0;
        auto flushRun = [&]() {
            if (runCount == 0)
                return;
            cmd.DrawIndexed(runCount, 1, runOffset, 0, 0);
            ++reflectionStats.drawCalls;
            runCount = 0;
        };
        for (const TerrainChunkDraw& chunk : m_terrainChunks)
        {
            if (chunk.worldMax.y < reflectionWaterLevelY ||
                TerrainAabbOutsideCameraFrustum(mirror, chunk.worldMin, chunk.worldMax))
            {
                ++reflectionStats.chunksCulled;
                continue;
            }
            ++reflectionStats.chunksDrawn;
            if (runCount != 0 && runOffset + runCount == chunk.indexOffset)
            {
                runCount += chunk.indexCount;
                continue;
            }
            flushRun();
            runOffset = chunk.indexOffset;
            runCount = chunk.indexCount;
        }
        flushRun();
    }
    reflectionStats.executed = true;

    if (renderEntities)
        renderEntities(mirror,
            m_waterReflection.width,
            m_waterReflection.height,
            m_waterReflection.target->GetPass(),
            reflectionWaterLevelY);

    m_waterReflection.target->End(cmd);

    m_reflectionClipWaterLevelY = std::numeric_limits<float>::quiet_NaN();
}

void TerrainRenderer::Render(ixrhi::IXRHICommandList& cmd,
                             const ixrhi::IXRHIFrameInfo& frame,
                             const WorldCamera& camera,
                             std::uint32_t targetWidth,
                             std::uint32_t targetHeight,
                             uint32_t viewIndex,
                             bool clearDepth)
{
    static bool loggedDraw = false;
    static bool loggedSkip = false;
    PassDrawStats& terrainStats = m_frameDrawStats.terrainMain;

    if (!m_sceneTerrainActive || m_sceneTerrain.editorHidden || !m_pipeline || !m_bindGroup || !m_vertexBuffer ||
        !m_indexBuffer || m_indexCount == 0 || !frame.frameActive)
    {
        terrainStats.skipped = true;
        if (!loggedSkip)
        {
            Tracen("[TERRAIN] Render skip: inactive pipeline/frame");
            loggedSkip = true;
        }
        return;
    }

    const std::uint32_t extentWidth = targetWidth > 0 ? targetWidth : frame.targetWidth;
    const std::uint32_t extentHeight = targetHeight > 0 ? targetHeight : frame.targetHeight;
    if (extentWidth == 0 || extentHeight == 0)
    {
        terrainStats.skipped = true;
        return;
    }

    const uint32_t frameIndex = frame.frameIndex % kFramesInFlight;
    UpdateUniform(frameIndex, camera, false, viewIndex);

    // Depth-only clear over the draw area (parity with the pre-migration
    // native depth clear inside the outer pass).
    if (clearDepth)
        cmd.ClearDepth(1.0f, 0, 0, extentWidth, extentHeight);

    cmd.SetViewport(0.0f, 0.0f, static_cast<float>(extentWidth), static_cast<float>(extentHeight));
    cmd.SetScissor(0, 0, extentWidth, extentHeight);
    cmd.SetGraphicsPipeline(*m_pipeline);
    if (m_sceneTerrain.triplanarEnabled && !m_triPerfMainPassLogged)
    {
        TraceDiagf("[TRI-PERF] terrain pipeline bound in pass=main triplanar=yes extent=%ux%u shader=terrain_ps",
            extentWidth,
            extentHeight);
        TraceDiagf("[TRI-PERF] offscreen target extent=%ux%u fragment-bound-cost-scales-with-pixels",
            extentWidth,
            extentHeight);
        m_triPerfMainPassLogged = true;
    }

    cmd.SetVertexBuffer(0, *m_vertexBuffer, 0);
    cmd.SetIndexBuffer(*m_indexBuffer, 0, /*thirtyTwoBit=*/true);
    struct TerrainPushConstants
    {
        float layerParams[4];
    };

    m_visibleTerrainChunksScratch.clear();
    uint32_t culledChunks = 0;
    if (!m_terrainChunks.empty())
    {
        m_visibleTerrainChunksScratch.reserve(m_terrainChunks.size());
        for (uint32_t chunkIndex = 0; chunkIndex < static_cast<uint32_t>(m_terrainChunks.size()); ++chunkIndex)
        {
            const TerrainChunkDraw& chunk = m_terrainChunks[chunkIndex];
            if (TerrainAabbOutsideCameraFrustum(camera, chunk.worldMin, chunk.worldMax))
            {
                ++culledChunks;
            }
            else
            {
                m_visibleTerrainChunksScratch.push_back(chunkIndex);
            }
        }
    }
    auto drawVisibleTerrainChunks = [&]() {
        if (m_terrainChunks.empty())
        {
            cmd.DrawIndexed(m_indexCount, 1, 0, 0, 0);
            ++terrainStats.drawCalls;
            return;
        }
        for (uint32_t chunkIndex : m_visibleTerrainChunksScratch)
        {
            const TerrainChunkDraw& chunk = m_terrainChunks[chunkIndex];
            cmd.DrawIndexed(chunk.indexCount, 1, chunk.indexOffset, 0, 0);
            ++terrainStats.drawCalls;
        }
    };
    terrainStats.executed = true;
    terrainStats.chunksDrawn = m_terrainChunks.empty()
        ? (m_indexCount > 0 ? 1u : 0u)
        : static_cast<uint32_t>(m_visibleTerrainChunksScratch.size());
    terrainStats.chunksCulled = m_terrainChunks.empty() ? 0u : culledChunks;

    TerrainPushConstants push{{1.0f, 1.0f, 0.0f, 0.0f}};
    cmd.PushConstants(&push, sizeof(push));
    // Slot = viewIndex * kFramesInFlight + frameIndex (primary + secondary
    // camera-uniform paths).
    const std::uint32_t terrainSlot = viewIndex * kFramesInFlight + frameIndex;
    cmd.BindGroup(0, *m_bindGroup, terrainSlot);
    const uint32_t baseDrawCallsBefore = terrainStats.drawCalls;
    drawVisibleTerrainChunks();
    const uint32_t baseDrawCalls = terrainStats.drawCalls - baseDrawCallsBefore;

    if (!m_terrainShaderOptimDiagLogged)
    {
        const uint32_t activeLayerCount =
            ActiveSplatLayerSpan();
        const uint32_t samplesPerLayer = m_sceneTerrain.triplanarEnabled ? 16u : 5u;
        Tracenf("[TERRAIN-SHADER-DIAG] active_layer_count=%u total_layer_count=8 triplanar_enabled=%s samples_per_pixel_estimate=%u render_pass_count=1 draw_calls_per_frame=%u",
            activeLayerCount,
            m_sceneTerrain.triplanarEnabled ? "yes" : "no",
            activeLayerCount * samplesPerLayer,
            baseDrawCalls);
        m_terrainShaderOptimDiagLogged = true;
    }

    if (m_mapLoaded && m_mapEditorOpen && m_editorBrushVisible)
    {
        TerrainPushConstants brushPush{{m_editorBrushLocalX,
                                        m_editorBrushLocalZ,
                                        6.0f,
                                        m_editorBrushRadiusMeters}};
        cmd.PushConstants(&brushPush, sizeof(brushPush));
        cmd.SetVertexBuffer(0, *m_vertexBuffer, 0);
        cmd.SetIndexBuffer(*m_indexBuffer, 0, /*thirtyTwoBit=*/true);
        cmd.BindGroup(0, *m_bindGroup, frameIndex);
        drawVisibleTerrainChunks();
    }

    if (m_mapLoaded && m_mapEditorOpen && m_waterSculptBrushVisible)
    {
        TerrainPushConstants brushPush{{m_waterSculptBrushWorldX,
                                        m_waterSculptBrushWorldZ,
                                        m_waterSculptBrushAddMode ? 8.0f : 9.0f,
                                        m_waterSculptBrushRadiusMeters}};
        cmd.PushConstants(&brushPush, sizeof(brushPush));
        cmd.SetVertexBuffer(0, *m_vertexBuffer, 0);
        cmd.SetIndexBuffer(*m_indexBuffer, 0, /*thirtyTwoBit=*/true);
        cmd.BindGroup(0, *m_bindGroup, frameIndex);
        drawVisibleTerrainChunks();
    }

#if defined(IXTREEME_DEBUG_LOGS)
    {
        static uint64_t terrainChunkLogFrame = 0;
        ++terrainChunkLogFrame;
        if (terrainChunkLogFrame <= 3 || (terrainChunkLogFrame % 60u) == 0u)
        {
            const uint32_t drawnChunks = m_terrainChunks.empty()
                ? (m_indexCount > 0 ? 1u : 0u)
                : static_cast<uint32_t>(m_visibleTerrainChunksScratch.size());
            const uint32_t gridX = m_chunkSizeCells > 0 && m_mapSizeX > 0
                ? (m_mapSizeX + m_chunkSizeCells - 1u) / m_chunkSizeCells
                : 0u;
            const uint32_t gridY = m_chunkSizeCells > 0 && m_mapSizeY > 0
                ? (m_mapSizeY + m_chunkSizeCells - 1u) / m_chunkSizeCells
                : 0u;
            TraceDiagf("[TCHUNK] render chunksDrawn=%u culled=%u chunkGrid=%ux%u policy=frustum-culled",
                drawnChunks,
                m_terrainChunks.empty() ? 0u : culledChunks,
                gridX,
                gridY);
        }
    }
#endif

    if (!loggedDraw)
    {
        const size_t paletteLayers = m_baseTexture.image && m_normalTexture.image &&
            m_aoTexture.image && m_roughnessTexture.image && m_metallicTexture.image && m_heightTexture.image
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
        loggedDraw = true;
    }
}

void TerrainRenderer::RenderSelectedWaterBodyHighlight(ixrhi::IXRHICommandList& cmd,
                                                        const ixrhi::IXRHIFrameInfo& frame,
                                                        const WorldCamera& camera,
                                                        std::uint32_t targetWidth,
                                                        std::uint32_t targetHeight)
{
    if (!m_sceneTerrainActive || m_sceneTerrain.editorHidden || !m_mapEditorOpen || !m_pipeline || !m_bindGroup ||
        !m_selectedWaterBodyIndexCount || !m_selectedWaterBodyVertexBuffer || !m_selectedWaterBodyIndexBuffer ||
        !frame.frameActive)
    {
        return;
    }

    const std::uint32_t extentWidth = targetWidth > 0 ? targetWidth : frame.targetWidth;
    const std::uint32_t extentHeight = targetHeight > 0 ? targetHeight : frame.targetHeight;
    if (extentWidth == 0 || extentHeight == 0)
        return;

    const uint32_t frameIndex = frame.frameIndex % kFramesInFlight;
    UpdateUniform(frameIndex, camera);

    cmd.SetViewport(0.0f, 0.0f, static_cast<float>(extentWidth), static_cast<float>(extentHeight));
    cmd.SetScissor(0, 0, extentWidth, extentHeight);
    cmd.SetGraphicsPipeline(*m_pipeline);

    struct TerrainPushConstants
    {
        float layerParams[4];
    };
    TerrainPushConstants push{{1.0f, 1.0f, 7.0f, 0.0f}};
    cmd.PushConstants(&push, sizeof(push));

    cmd.SetVertexBuffer(0, *m_selectedWaterBodyVertexBuffer, 0);
    cmd.SetIndexBuffer(*m_selectedWaterBodyIndexBuffer, 0, /*thirtyTwoBit=*/true);
    cmd.BindGroup(0, *m_bindGroup, frameIndex);
    cmd.DrawIndexed(m_selectedWaterBodyIndexCount, 1, 0, 0, 0);
}

void TerrainRenderer::RenderWater(ixrhi::IXRHICommandList& cmd,
                                   const ixrhi::IXRHIFrameInfo& frame,
                                   const WorldCamera& camera,
                                   double timeSeconds,
                                   std::uint32_t targetWidth,
                                   std::uint32_t targetHeight,
                                   uint32_t viewIndex)
{
    m_latestWaterTimeSeconds = timeSeconds;
    if (!m_sceneTerrainActive || m_sceneTerrain.editorHidden || m_waterBodies.empty() || !m_waterPipeline ||
        !m_waterBindGroup || !frame.frameActive)
        return;

    const std::uint32_t extentWidth = targetWidth > 0 ? targetWidth : frame.targetWidth;
    const std::uint32_t extentHeight = targetHeight > 0 ? targetHeight : frame.targetHeight;
    if (extentWidth == 0 || extentHeight == 0)
        return;

    const uint32_t frameIndex = frame.frameIndex % kFramesInFlight;
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

    cmd.SetViewport(0.0f, 0.0f, static_cast<float>(extentWidth), static_cast<float>(extentHeight));
    cmd.SetScissor(0, 0, extentWidth, extentHeight);
    cmd.SetGraphicsPipeline(*m_waterPipeline);

    for (std::uint32_t bodyIndex = 0; bodyIndex < static_cast<std::uint32_t>(m_waterBodies.size()); ++bodyIndex)
    {
        WaterBodyGpu& waterBody = m_waterBodies[bodyIndex];
        if (!ResolveWaterConfig(waterBody.body).enabled || !waterBody.indexCount ||
            !waterBody.vertexBuffer || !waterBody.indexBuffer || !WaterBodyInView(waterBody, camera))
        {
            continue;
        }
        const bool isReflectionTarget = (&waterBody == reflectionTarget);
        UpdateWaterBodyUniform(frameIndex, camera, timeSeconds, waterBody, isReflectionTarget, viewIndex);
        cmd.SetVertexBuffer(0, *waterBody.vertexBuffer, 0);
        cmd.SetIndexBuffer(*waterBody.indexBuffer, 0, /*thirtyTwoBit=*/true);
        // Slot = (bodyIndex * 2 + viewIndex) * kFramesInFlight + frameIndex.
        cmd.BindGroup(0,
            *m_waterBindGroup,
            (bodyIndex * 2u + (viewIndex == 0 ? 0u : 1u)) * kFramesInFlight + frameIndex);
        cmd.DrawIndexed(waterBody.indexCount, 1, 0, 0, 0);
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

bool TerrainRenderer::SetWaterBodies(ixrhi::IXRHIDevice& rhi, const std::vector<WaterBody>& bodies)
{
    m_rhi = &rhi;
    rhi.WaitIdle();
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
        if (!CreateWaterBodyUniformBuffers(rhi, gpu) || !CreateWaterBodyMesh(rhi, gpu))
        {
            DestroyWaterBodyResources(gpu);
            Tracen("[WATER-OBJ] skipped invalid editor water body");
            continue;
        }
        m_waterBodies.push_back(std::move(gpu));
    }

    if (m_waterBindGroup)
        CreateWaterBindGroup(rhi);
    SetSelectedWaterBodyHighlight(rhi, m_selectedWaterBodyId);
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
    if (textureSignature != m_waterMaterialTextureSignature && m_rhi && m_assets)
    {
        m_rhi->WaitIdle();
        DestroyWaterMaterialTextureCache();
        m_waterMaterialTextureSignature = textureSignature;
        for (const auto& [id, material] : m_waterMaterials)
        {
            WaterMaterialTextureSet textureSet{};
            if (LoadWaterMaterialTextureSet(*m_rhi, id, material, textureSet))
                m_waterMaterialTextures[id] = std::move(textureSet);
        }
        UpdateWaterBindGroup();
    }
    if (edgeSignature != m_waterMaterialEdgeSignature && m_rhi)
    {
        m_rhi->WaitIdle();
        m_waterMaterialEdgeSignature = edgeSignature;
        for (WaterBodyGpu& waterBody : m_waterBodies)
            CreateWaterBodyMesh(*m_rhi, waterBody);
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
    m_waterMaterialTextures.clear();
    m_waterMaterialTextureSignature.clear();
}

bool TerrainRenderer::LoadWaterMaterialTextureSet(ixrhi::IXRHIDevice& rhi,
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

    auto loadTexture = [&](const std::string& path, const std::string& label, Texture& texture, ixrhi::IXRHIFormat format) -> bool {
        if (path.empty())
            return false;
        RgbaImage image{};
        if (!LoadAnyTerrainImage(*m_assets, path, image, &m_additionalAssetRoots))
        {
            Tracenf("[WATER-MAT] failed to load %s texture for %s: %s",
                label.c_str(),
                id.c_str(),
                path.c_str());
            return false;
        }
        if (label.rfind("normal", 0) == 0)
            ConvertTangentNormalMapToYUp(image.pixels);
        return UploadRgbaTexture2D(rhi, "watermat_" + id + "_" + label, image.width, image.height, image.pixels,
            ixrhi::IXRHISamplerAddress::Repeat, texture, format, /*generateMips=*/true);
    };

    const bool loadedA = loadTexture(normalAPath, "normal_a", out.normalA, ixrhi::IXRHIFormat::R8G8B8A8Unorm);
    const bool loadedB = loadTexture(normalBPath, "normal_b", out.normalB, ixrhi::IXRHIFormat::R8G8B8A8Unorm);
    const bool loadedDiffuse = loadTexture(diffusePath, "diffuse", out.diffuse, ixrhi::IXRHIFormat::R8G8B8A8Srgb);
    if (!loadedA && !loadedB && !loadedDiffuse)
        return false;
    if (!loadedA)
        out.normalA = {};
    if (!loadedB)
        out.normalB = {};
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

bool TerrainRenderer::SetSelectedWaterBodyHighlight(ixrhi::IXRHIDevice& rhi, std::uint32_t selectedWaterBodyId)
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
        return RebuildSelectedWaterBodyHighlight(rhi, nullptr);
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
            [](float a, float b) { return xm::Abs(a - b) < 0.001f; }))
    {
        return true;
    }

    m_selectedWaterBodyId = selectedBody->id;
    std::copy(std::begin(signature), std::end(signature), std::begin(m_selectedWaterBodySignature));
    return RebuildSelectedWaterBodyHighlight(rhi, selectedBody);
}

bool TerrainRenderer::RebuildSelectedWaterBodyHighlight(ixrhi::IXRHIDevice& rhi, const WaterBody* body)
{
    rhi.WaitIdle();
    m_selectedWaterBodyVertexBuffer.reset();
    m_selectedWaterBodyIndexBuffer.reset();
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

    m_selectedWaterBodyVertexBuffer = CreateRhiBuffer(rhi,
        sizeof(Vertex) * vertices.size(),
        ixrhi::IXRHIBufferUsage::Vertex,
        vertices.data(),
        "Terrain:SelectedWaterVB");
    m_selectedWaterBodyIndexBuffer = CreateRhiBuffer(rhi,
        sizeof(uint32_t) * indices.size(),
        ixrhi::IXRHIBufferUsage::Index,
        indices.data(),
        "Terrain:SelectedWaterIB");
    m_selectedWaterBodyIndexCount = static_cast<uint32_t>(indices.size());
    return m_selectedWaterBodyVertexBuffer != nullptr && m_selectedWaterBodyIndexBuffer != nullptr;
}

void TerrainRenderer::SetPaletteSlots(const std::array<MapEditorPaletteSlot, 8>& slots)
{
    m_paletteSlots = slots;
    m_materialParamsDirty = true;
}

bool TerrainRenderer::ApplyPaletteSlotChange(ixrhi::IXRHIDevice& rhi, const MapEditorPaletteSlot& slot)
{
    if (slot.slot >= m_paletteSlots.size() || slot.texturePath.empty())
        return false;

    auto next = m_paletteSlots;
    next[slot.slot] = slot;
    if (!LoadTerrainPaletteFromPaths(rhi, next))
        return false;
    m_paletteSlots = next;
    return true;
}

bool TerrainRenderer::ApplyPaletteSlots(ixrhi::IXRHIDevice& rhi, const std::array<MapEditorPaletteSlot, 8>& slots)
{
    return LoadTerrainPaletteFromPaths(rhi, slots);
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
        xm::Abs(m_sceneTerrain.triplanarSharpness - clampedSharpness) > 0.0001f ||
        xm::Abs(m_sceneTerrain.triplanarSlopeThreshold - clampedSlopeThreshold) > 0.0001f ||
        xm::Abs(m_sceneTerrain.triplanarSlopeTransition - clampedSlopeTransition) > 0.0001f;
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
        if (m_mapEditorOpen && m_editorTerrainToolActive && m_editorLmbHeld && BrushDiagLogs())
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
                m_brushDiagApplications = 0;
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

void TerrainRenderer::UpdateEditor(ixrhi::IXRHIDevice& rhi,
                                   double deltaSeconds,
                                   const WorldCamera& camera,
                                   uint32_t viewportWidth,
                                   uint32_t viewportHeight)
{
    m_rhi = &rhi;
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
        ReloadCurrentMap(rhi);
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
        UndoLastEditorStroke(rhi);
    }

    if (!editorBrushActive || !m_mapLoaded)
        return;

    if (m_mapEditorOpen)
    {
        if (m_editorLmbHeld && m_editorBrushVisible)
            ApplyEditorBrush(rhi, deltaSeconds);
        // Painted texels upload in the frame (UploadEditedTerrain); this synchronous whole-texture path
        // is only for a change without a dirty rectangle.
        if (m_editorSplatGpuDirty && !m_splatDirtyRect.Valid())
            RefreshSplatTextures(rhi);
        return;
    }

    if (m_editorRaiseHeld)
        ApplyLegacyHeightBrush(1.0f, deltaSeconds);
    if (m_editorLowerHeld)
        ApplyLegacyHeightBrush(-1.0f, deltaSeconds);
}

float TerrainRenderer::SampleHeightAt(float localX, float localZ) const
{
    if (!m_mapLoaded || m_heightCmGrid.empty())
        return 0.0f;

    const float localXcm = m_spawnLocalXcm + localX * 100.0f;
    const float localYcm = m_spawnLocalYcm - localZ * 100.0f;
    const float heightCm = SampleTerrainCollisionHeightCm(m_heightCmGrid, m_heightGridWidth, m_heightGridHeight,
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
    if (!m_rhi)
        return;

    // Teardown drain: in-flight frames may still reference terrain buffers
    // and textures being released here (parity with the old device wait).
    m_rhi->WaitIdle();

    DestroyPipeline();
    DestroyShadowResources();
    DestroyWaterResources();

    m_bindGroup.reset();
    m_bindLayout.reset();
    m_waterBindGroup.reset();
    m_waterBindLayout.reset();

    // Refraction inputs share ownership with OffscreenSceneRenderer. Release
    // our copies during explicit teardown while the device is still alive;
    // leaving them for member destruction would outlive device shutdown.
    m_waterSceneColor.reset();
    m_waterSceneDepth.reset();
    m_waterSceneSampler.reset();
    m_waterSceneWidth = 0;
    m_waterSceneHeight = 0;

    m_vertexBuffer.reset();
    m_vertexEditBuffer.reset();
    for (std::shared_ptr<ixrhi::IXRHIBuffer>& staging : m_splatStaging)
        staging.reset();
    m_indexBuffer.reset();
    m_debugVertexBuffer.reset();
    m_debugIndexBuffer.reset();
    m_logicVertexBuffer.reset();
    m_logicIndexBuffer.reset();
    m_selectedWaterBodyVertexBuffer.reset();
    m_selectedWaterBodyIndexBuffer.reset();
    for (auto& buffer : m_uniformBuffers)
        buffer.reset();
    for (auto& buffer : m_uniformBuffersSecondary)
        buffer.reset();
    for (auto& buffer : m_uniformBuffersReflection)
        buffer.reset();
    DestroyTerrainLayers();
    m_baseTexture = {};
    m_normalTexture = {};
    m_aoTexture = {};
    m_roughnessTexture = {};
    m_metallicTexture = {};
    m_heightTexture = {};
    m_fallbackMask = {};
    m_splatA = {};
    m_splatB = {};
    DestroyWaterMaterialTextureCache();
    m_waterNormalSmall = {};
    m_waterNormalLarge = {};

    m_indexCount = 0;
    m_debugIndexCount = 0;
    m_spawnDebugIndexOffset = 0;
    m_spawnDebugIndexCount = 0;
    m_logicDebugIndexOffset = 0;
    m_logicDebugIndexCount = 0;
    m_terrainChunks.clear();
    m_visibleTerrainChunksScratch.clear();
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
    m_splatDirtyRect = {};
    m_loadedMapDirectory.clear();
    m_loadedServerX = 0;
    m_loadedServerY = 0;
    m_heightCmGrid.clear();
    m_attributes.clear();
    m_splatABytes.clear();
    m_splatBBytes.clear();
    m_activeSplatLayerSpan = -1;
    m_dirtyChunkTexels.clear();
    m_terrainChunks.clear();
    m_visibleTerrainChunksScratch.clear();
    m_heightUndoRecorded.clear();
    m_splatUndoRecorded.clear();
    m_currentUndo = {};
    m_undoStack.clear();
    m_targetPass = nullptr;
    m_rhi = nullptr;
    m_assets = nullptr;
}

bool TerrainRenderer::CreateBuffers(ixrhi::IXRHIDevice& rhi)
{
    m_mapSizeX = 100;
    m_mapSizeY = 100;
    m_cellScaleMeters = 1.0f;
    m_heightCmGrid.assign(static_cast<size_t>(m_mapSizeX + 1u) * (m_mapSizeY + 1u), 0.0f);
    return CreateFlatBuffers(rhi);
}

bool TerrainRenderer::EnsureUniformBuffers(ixrhi::IXRHIDevice& rhi)
{
    auto ensure = [&](std::array<std::shared_ptr<ixrhi::IXRHIBuffer>, kFramesInFlight>& buffers,
                      const char* debugName) {
        for (auto& buffer : buffers)
        {
            if (buffer)
                continue;

            buffer = CreateRhiBuffer(rhi, sizeof(UniformBlock), ixrhi::IXRHIBufferUsage::Uniform, nullptr, debugName);
        }
    };
    ensure(m_uniformBuffers, "Terrain:UBO");
    ensure(m_uniformBuffersSecondary, "Terrain:UBO secondary");
    ensure(m_uniformBuffersReflection, "Terrain:UBO reflection");

    for (const auto& buffer : m_uniformBuffers)
    {
        if (!buffer)
        {
            Tracen("[TERRAIN] uniform buffer creation failed");
            return false;
        }
    }
    for (const auto& buffer : m_uniformBuffersSecondary)
    {
        if (!buffer)
        {
            Tracen("[TERRAIN] secondary uniform buffer creation failed");
            return false;
        }
    }
    for (const auto& buffer : m_uniformBuffersReflection)
    {
        if (!buffer)
        {
            Tracen("[TERRAIN] reflection uniform buffer creation failed");
            return false;
        }
    }
    return true;
}

void TerrainRenderer::ExpandTerrainChunkBounds(const Vertex* vertices, std::size_t count)
{
    if (m_terrainChunks.empty() || vertices == nullptr || count == 0)
        return;
    WorldVec3 lo{vertices[0].position[0], vertices[0].position[1], vertices[0].position[2]};
    WorldVec3 hi = lo;
    for (std::size_t i = 1; i < count; ++i)
    {
        const float* p = vertices[i].position;
        lo = {std::min(lo.x, p[0]), std::min(lo.y, p[1]), std::min(lo.z, p[2])};
        hi = {std::max(hi.x, p[0]), std::max(hi.y, p[1]), std::max(hi.z, p[2])};
    }
    // Grow only (never shrink): conservative for the main-pass and shadow-cascade culling.
    for (TerrainChunkDraw& chunk : m_terrainChunks)
    {
        if (chunk.worldMax.x < lo.x || chunk.worldMin.x > hi.x || chunk.worldMax.z < lo.z || chunk.worldMin.z > hi.z)
            continue;
        chunk.worldMin.y = std::min(chunk.worldMin.y, lo.y);
        chunk.worldMax.y = std::max(chunk.worldMax.y, hi.y);
    }
}

void TerrainRenderer::BuildTerrainChunkDraws(const std::vector<Vertex>& vertices, std::vector<uint32_t>& indices)
{
    m_terrainChunks.clear();
    indices.clear();
    if (m_mapSizeX == 0 || m_mapSizeY == 0 || m_heightGridWidth < 2 || m_heightGridHeight < 2 ||
        m_chunkSizeCells == 0 || vertices.empty())
    {
        return;
    }

    const uint32_t chunksX = (m_mapSizeX + m_chunkSizeCells - 1u) / m_chunkSizeCells;
    const uint32_t chunksY = (m_mapSizeY + m_chunkSizeCells - 1u) / m_chunkSizeCells;
    indices.reserve(static_cast<size_t>(m_mapSizeX) * m_mapSizeY * 6u);
    m_terrainChunks.reserve(static_cast<size_t>(chunksX) * chunksY);

    const float horizontalPad = std::max(0.05f, static_cast<float>(m_chunkSizeCells) * m_cellScaleMeters * 0.05f);
    constexpr float kVerticalPad = 2.0f;

    for (uint32_t chunkY = 0; chunkY < chunksY; ++chunkY)
    {
        const uint32_t cellMinY = chunkY * m_chunkSizeCells;
        const uint32_t cellMaxY = std::min(cellMinY + m_chunkSizeCells, m_mapSizeY);
        for (uint32_t chunkX = 0; chunkX < chunksX; ++chunkX)
        {
            const uint32_t cellMinX = chunkX * m_chunkSizeCells;
            const uint32_t cellMaxX = std::min(cellMinX + m_chunkSizeCells, m_mapSizeX);
            TerrainChunkDraw chunk{};
            chunk.indexOffset = static_cast<uint32_t>(indices.size());
            chunk.worldMin = {
                std::numeric_limits<float>::max(),
                std::numeric_limits<float>::max(),
                std::numeric_limits<float>::max()};
            chunk.worldMax = {
                -std::numeric_limits<float>::max(),
                -std::numeric_limits<float>::max(),
                -std::numeric_limits<float>::max()};

            for (uint32_t z = cellMinY; z <= cellMaxY; ++z)
            {
                for (uint32_t x = cellMinX; x <= cellMaxX; ++x)
                {
                    const size_t vertexIndex = static_cast<size_t>(z) * m_heightGridWidth + x;
                    if (vertexIndex >= vertices.size())
                        continue;
                    const Vertex& vertex = vertices[vertexIndex];
                    chunk.worldMin.x = std::min(chunk.worldMin.x, vertex.position[0]);
                    chunk.worldMin.y = std::min(chunk.worldMin.y, vertex.position[1]);
                    chunk.worldMin.z = std::min(chunk.worldMin.z, vertex.position[2]);
                    chunk.worldMax.x = std::max(chunk.worldMax.x, vertex.position[0]);
                    chunk.worldMax.y = std::max(chunk.worldMax.y, vertex.position[1]);
                    chunk.worldMax.z = std::max(chunk.worldMax.z, vertex.position[2]);
                }
            }

            for (uint32_t z = cellMinY; z < cellMaxY; ++z)
            {
                for (uint32_t x = cellMinX; x < cellMaxX; ++x)
                {
                    const uint32_t i0 = z * m_heightGridWidth + x;
                    const uint32_t i1 = i0 + 1u;
                    const uint32_t i2 = i0 + m_heightGridWidth + 1u;
                    const uint32_t i3 = i0 + m_heightGridWidth;
                    indices.push_back(i0);
                    indices.push_back(i1);
                    indices.push_back(i3);
                    indices.push_back(i1);
                    indices.push_back(i2);
                    indices.push_back(i3);
                }
            }

            chunk.indexCount = static_cast<uint32_t>(indices.size()) - chunk.indexOffset;
            if (chunk.indexCount == 0)
                continue;
            chunk.worldMin.x -= horizontalPad;
            chunk.worldMin.y -= kVerticalPad;
            chunk.worldMin.z -= horizontalPad;
            chunk.worldMax.x += horizontalPad;
            chunk.worldMax.y += kVerticalPad;
            chunk.worldMax.z += horizontalPad;
            m_terrainChunks.push_back(chunk);
        }
    }
}

bool TerrainRenderer::CreateFlatBuffers(ixrhi::IXRHIDevice& rhi)
{
    const uint32_t cellsX = std::max(1u, m_mapSizeX == 0 ? 100u : m_mapSizeX);
    const uint32_t cellsZ = std::max(1u, m_mapSizeY == 0 ? 100u : m_mapSizeY);
    const float cellSize = std::max(0.01f, m_cellScaleMeters);
    if (m_chunkSizeCells == 0)
        m_chunkSizeCells = 64;
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
    BuildTerrainChunkDraws(vertices, indices);
    m_indexCount = static_cast<uint32_t>(indices.size());

    // Drawn from video memory (main pass + every shadow cascade); edited (sculpt) through a
    // host-visible copy that UploadEditedTerrain() copies over.
    m_vertexBuffer = CreateStaticRhiBuffer(rhi,
        sizeof(Vertex) * vertices.size(),
        ixrhi::IXRHIBufferUsage::Vertex | ixrhi::IXRHIBufferUsage::TransferDst,
        vertices.data(),
        "Terrain:VB");
    m_vertexEditBuffer = CreateRhiBuffer(rhi,
        sizeof(Vertex) * vertices.size(),
        ixrhi::IXRHIBufferUsage::Vertex | ixrhi::IXRHIBufferUsage::TransferSrc,
        vertices.data(),
        "Terrain:VB(edit)");
    m_vertexBufferUploadPending = false;
    m_indexBuffer = CreateStaticRhiBuffer(rhi,
        sizeof(uint32_t) * indices.size(),
        ixrhi::IXRHIBufferUsage::Index,
        indices.data(),
        "Terrain:IB");
    if (!m_vertexBuffer || !m_vertexEditBuffer || !m_indexBuffer)
        return false;

    return EnsureUniformBuffers(rhi);
}

bool TerrainRenderer::UploadRgbaTexture2D(ixrhi::IXRHIDevice& rhi,
    const std::string& name,
    uint32_t width,
    uint32_t height,
    const std::vector<std::uint8_t>& pixels,
    ixrhi::IXRHISamplerAddress addressMode,
    Texture& out,
    ixrhi::IXRHIFormat format,
    bool generateMips)
{
    if (width == 0 || height == 0 || pixels.size() != static_cast<size_t>(width) * height * 4u)
        return false;

    out = {};
    // A texture tiled across a surface needs its mip chain (and trilinear filtering): without it
    // the distant, grazing-angle part of the surface aliases into shimmering stripes.
    ArrayMipUpload mipUpload{};
    if (generateMips)
    {
        mipUpload = BuildRgbaArrayMipUpload(width, height, 1, pixels,
            format == ixrhi::IXRHIFormat::R8G8B8A8Srgb, name.find("normal") != std::string::npos ||
                name.find("wave") != std::string::npos);
    }
    const std::uint32_t mipLevels = generateMips ? mipUpload.mipLevels : 1u;
    ixrhi::IXRHITextureDesc imageDesc;
    imageDesc.width = width;
    imageDesc.height = height;
    imageDesc.mipLevels = mipLevels;
    imageDesc.format = format;
    imageDesc.usage = ixrhi::IXRHITextureUsage::Sampled | ixrhi::IXRHITextureUsage::TransferDst;
    imageDesc.debugName = "Terrain:" + name;
    out.image = generateMips
        ? rhi.CreateTexture(imageDesc, mipUpload.pixels.data(), mipUpload.pixels.size())
        : rhi.CreateTexture(imageDesc, pixels.data(), pixels.size());
    if (!out.image)
        return false;

    out.sampler = CreateRhiSampler(rhi, addressMode, /*trilinear=*/generateMips,
        /*maxLod=*/generateMips ? static_cast<float>(mipLevels - 1u) : 1.0f, /*mipLodBias=*/0.0f,
        ("Terrain:" + name + ":Sampler").c_str());
    if (!out.sampler)
    {
        out = {};
        return false;
    }

    out.format = format;
    out.width = width;
    out.height = height;
    out.mipLevels = mipLevels;
    out.arrayLayers = 1;
    out.name = name;
    return true;
}

bool TerrainRenderer::UpdateRgbaTexture2D(ixrhi::IXRHIDevice& rhi,
                                           Texture& texture,
                                           const std::vector<std::uint8_t>& pixels)
{
    if (!texture.image || texture.width == 0 || texture.height == 0 ||
        pixels.size() != static_cast<size_t>(texture.width) * texture.height * 4u)
        return false;

    // Full base-level in-place rewrite (splat paint parity); the image,
    // sampler and bind-group entries stay valid, so no descriptor update.
    return rhi.UpdateTexture(*texture.image, pixels.data(), pixels.size());
}

bool TerrainRenderer::UploadRgbaTextureArray(ixrhi::IXRHIDevice& rhi,
    const std::string& name,
    uint32_t width,
    uint32_t height,
    uint32_t layers,
    const std::vector<std::uint8_t>& pixels,
    ixrhi::IXRHIFormat format,
    Texture& out)
{
    if (width == 0 || height == 0 || layers == 0 ||
        pixels.size() != static_cast<size_t>(width) * height * layers * 4u)
        return false;

    out = {};
    const bool srgbColor = format == ixrhi::IXRHIFormat::R8G8B8A8Srgb;
    const bool normalMap = name.find("normal") != std::string::npos;
    const ArrayMipUpload mipUpload = BuildRgbaArrayMipUpload(width, height, layers, pixels, srgbColor, normalMap);

    ixrhi::IXRHITextureDesc imageDesc;
    imageDesc.width = width;
    imageDesc.height = height;
    imageDesc.mipLevels = mipUpload.mipLevels;
    imageDesc.arrayLayers = layers;
    imageDesc.format = format;
    imageDesc.usage = ixrhi::IXRHITextureUsage::Sampled | ixrhi::IXRHITextureUsage::TransferDst;
    imageDesc.debugName = "Terrain:" + name;
    out.image = rhi.CreateTexture(imageDesc, mipUpload.pixels.data(), mipUpload.pixels.size());
    if (!out.image)
        return false;

    const bool trilinear = mipUpload.mipLevels > 1;
    out.sampler = CreateRhiSampler(rhi,
        ixrhi::IXRHISamplerAddress::Repeat,
        trilinear,
        static_cast<float>(mipUpload.mipLevels > 0 ? mipUpload.mipLevels - 1u : 0u),
        /*mipLodBias=*/0.0f,
        ("Terrain:" + name + ":Sampler").c_str());
    if (!out.sampler)
    {
        out = {};
        return false;
    }

    out.format = format;
    out.width = width;
    out.height = height;
    out.mipLevels = mipUpload.mipLevels;
    out.arrayLayers = layers;
    out.name = name;
    const bool aniso = rhi.GetCapabilities().supportsAnisotropy;
    Tracenf("[TERRAIN-MIPS] generated array=%s size=%ux%u layers=%u mips=%u format=%s normalRenorm=%s sampler=trilinear aniso=%s",
        name.c_str(),
        width,
        height,
        layers,
        out.mipLevels,
        RhiFormatName(format),
        normalMap ? "yes" : "no",
        aniso ? "yes" : "no");
    return true;
}

bool TerrainRenderer::UploadR8TextureArray(ixrhi::IXRHIDevice& rhi,
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

    out = {};
    const ArrayMipUpload mipUpload = BuildR8ArrayMipUpload(width, height, layers, pixels);

    ixrhi::IXRHITextureDesc imageDesc;
    imageDesc.width = width;
    imageDesc.height = height;
    imageDesc.mipLevels = mipUpload.mipLevels;
    imageDesc.arrayLayers = layers;
    imageDesc.format = ixrhi::IXRHIFormat::R8Unorm;
    imageDesc.usage = ixrhi::IXRHITextureUsage::Sampled | ixrhi::IXRHITextureUsage::TransferDst;
    imageDesc.debugName = "Terrain:" + name;
    out.image = rhi.CreateTexture(imageDesc, mipUpload.pixels.data(), mipUpload.pixels.size());
    if (!out.image)
        return false;

    const bool trilinear = mipUpload.mipLevels > 1;
    out.sampler = CreateRhiSampler(rhi,
        ixrhi::IXRHISamplerAddress::Repeat,
        trilinear,
        static_cast<float>(mipUpload.mipLevels > 0 ? mipUpload.mipLevels - 1u : 0u),
        /*mipLodBias=*/0.0f,
        ("Terrain:" + name + ":Sampler").c_str());
    if (!out.sampler)
    {
        out = {};
        return false;
    }

    out.format = ixrhi::IXRHIFormat::R8Unorm;
    out.width = width;
    out.height = height;
    out.mipLevels = mipUpload.mipLevels;
    out.arrayLayers = layers;
    out.name = name;
    const bool aniso = rhi.GetCapabilities().supportsAnisotropy;
    Tracenf("[TERRAIN-MIPS] generated array=%s size=%ux%u layers=%u mips=%u format=%s data=linear sampler=trilinear aniso=%s",
        name.c_str(),
        width,
        height,
        layers,
        out.mipLevels,
        RhiFormatName(out.format),
        aniso ? "yes" : "no");
    return true;
}

bool TerrainRenderer::CreateMapBuffers(ixrhi::IXRHIDevice& rhi, const std::string& mapDirectory, int32_t serverX, int32_t serverY)
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
    m_activeSplatLayerSpan = -1;
    m_heightUndoRecorded.assign(m_heightCmGrid.size(), 0);
    m_splatUndoRecorded.assign(static_cast<size_t>(m_splatWidth) * m_splatHeight, 0);
    m_currentUndo = {};
    m_undoStack.clear();
    m_editorSplatGpuDirty = false;
    m_splatDirtyRect = {};
    if (!field->splat_a_rgba8.empty() && !field->splat_b_rgba8.empty())
    {
        UploadRgbaTexture2D(rhi, "splat_a", field->splat_width, field->splat_height,
            field->splat_a_rgba8, ixrhi::IXRHISamplerAddress::ClampToEdge, m_splatA);
        UploadRgbaTexture2D(rhi, "splat_b", field->splat_width, field->splat_height,
            field->splat_b_rgba8, ixrhi::IXRHISamplerAddress::ClampToEdge, m_splatB);
        if (!LoadTerrainPalette(rhi, field->manifest, mapDirectory))
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

    m_spawnHeightCm = SampleTerrainCollisionHeightCm(m_heightCmGrid, m_heightGridWidth, m_heightGridHeight,
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
    BuildTerrainChunkDraws(vertices, indices);

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
                debugIndices.push_back(base + 3);
                debugIndices.push_back(base + 1);
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
            const float heightCm = SampleTerrainCollisionHeightCm(m_heightCmGrid,
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
    // Drawn from video memory (main pass + every shadow cascade); edited (sculpt) through a
    // host-visible copy that UploadEditedTerrain() copies over.
    m_vertexBuffer = CreateStaticRhiBuffer(rhi,
        sizeof(Vertex) * vertices.size(),
        ixrhi::IXRHIBufferUsage::Vertex | ixrhi::IXRHIBufferUsage::TransferDst,
        vertices.data(),
        "Terrain:VB");
    m_vertexEditBuffer = CreateRhiBuffer(rhi,
        sizeof(Vertex) * vertices.size(),
        ixrhi::IXRHIBufferUsage::Vertex | ixrhi::IXRHIBufferUsage::TransferSrc,
        vertices.data(),
        "Terrain:VB(edit)");
    m_vertexBufferUploadPending = false;
    m_indexBuffer = CreateStaticRhiBuffer(rhi,
        sizeof(uint32_t) * indices.size(),
        ixrhi::IXRHIBufferUsage::Index,
        indices.data(),
        "Terrain:IB");
    if (!m_vertexBuffer || !m_vertexEditBuffer || !m_indexBuffer)
        return false;
    m_debugIndexCount = static_cast<uint32_t>(debugIndices.size());
    if (!debugVertices.empty() && !debugIndices.empty())
    {
        m_debugVertexBuffer = CreateRhiBuffer(rhi,
            sizeof(Vertex) * debugVertices.size(),
            ixrhi::IXRHIBufferUsage::Vertex,
            debugVertices.data(),
            "Terrain:DebugVB");
        m_debugIndexBuffer = CreateRhiBuffer(rhi,
            sizeof(uint32_t) * debugIndices.size(),
            ixrhi::IXRHIBufferUsage::Index,
            debugIndices.data(),
            "Terrain:DebugIB");
        if (!m_debugVertexBuffer || !m_debugIndexBuffer)
            return false;
    }
    if (!logicVertices.empty() && !logicIndices.empty())
    {
        m_logicVertexBuffer = CreateRhiBuffer(rhi,
            sizeof(Vertex) * logicVertices.size(),
            ixrhi::IXRHIBufferUsage::Vertex,
            logicVertices.data(),
            "Terrain:LogicVB");
        m_logicIndexBuffer = CreateRhiBuffer(rhi,
            sizeof(uint32_t) * logicIndices.size(),
            ixrhi::IXRHIBufferUsage::Index,
            logicIndices.data(),
            "Terrain:LogicIB");
        if (!m_logicVertexBuffer || !m_logicIndexBuffer)
            return false;
    }
    if (!EnsureUniformBuffers(rhi)) {
        return false;
    }

    m_mapLoaded = true;
    m_terrainShaderOptimDiagLogged = false;
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

void TerrainRenderer::ApplyLegacyHeightBrush(float sign, double deltaSeconds)
{
    if (!m_mapLoaded || m_heightCmGrid.empty() || !m_vertexBuffer ||
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
    const uint32_t minX = std::max(chunkMinX, static_cast<uint32_t>(std::max(0.0f, xm::Floor(centerGridX - radiusCells))));
    const uint32_t minY = std::max(chunkMinY, static_cast<uint32_t>(std::max(0.0f, xm::Floor(centerGridY - radiusCells))));
    const uint32_t maxX = std::min(chunkMaxX, static_cast<uint32_t>(xm::Ceil(centerGridX + radiusCells)));
    const uint32_t maxY = std::min(chunkMaxY, static_cast<uint32_t>(xm::Ceil(centerGridY + radiusCells)));

    const float deltaCenterCm =
        sign * m_editorBrushStrength * static_cast<float>(deltaSeconds) * 100.0f;
    if (xm::Abs(deltaCenterCm) < 0.0001f)
        return;

    // Mapped-write parity via Read-modify-Write: the touched index span is
    // read back, patched on the CPU and written once (same bytes the old
    // in-place mapped writes produced, same in-flight hazard profile).
    const std::uint64_t spanFirst = static_cast<std::uint64_t>(minY) * m_heightGridWidth + minX;
    const std::uint64_t spanLast = static_cast<std::uint64_t>(maxY) * m_heightGridWidth + maxX;
    const std::uint64_t spanCount = spanLast - spanFirst + 1u;
    std::vector<Vertex> span(static_cast<std::size_t>(spanCount));
    m_vertexEditBuffer->Read(spanFirst * sizeof(Vertex), span.data(), spanCount * sizeof(Vertex));

    uint32_t changed = 0;
    for (uint32_t gy = minY; gy <= maxY; ++gy)
    {
        for (uint32_t gx = minX; gx <= maxX; ++gx)
        {
            const float dx = (static_cast<float>(gx) - centerGridX) * m_cellScaleMeters;
            const float dy = (static_cast<float>(gy) - centerGridY) * m_cellScaleMeters;
            const float dist = xm::Sqrt(dx * dx + dy * dy);
            if (dist > m_editorBrushRadiusMeters)
                continue;

            const float t = 1.0f - dist / std::max(m_editorBrushRadiusMeters, 0.001f);
            const float falloff = t * t;
            const size_t index = static_cast<size_t>(gy) * m_heightGridWidth + gx;
            const float newHeight = std::clamp(m_heightCmGrid[index] + deltaCenterCm * falloff,
                                               -32768.0f,
                                               32767.0f);
            m_heightCmGrid[index] = newHeight;
            span[static_cast<std::size_t>(index) - static_cast<std::size_t>(spanFirst)].position[1] =
                newHeight * 0.01f;
            ++changed;
        }
    }
    if (changed > 0)
    {
        ExpandTerrainChunkBounds(span.data(), span.size());
        m_vertexEditBuffer->Write(spanFirst * sizeof(Vertex), span.data(), spanCount * sizeof(Vertex));
        m_vertexBufferUploadPending = true;
    }

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

    WorldVec3 rayDir = WorldScreenRayDirection(
        camera,
        viewportWidth,
        viewportHeight,
        m_editorCursorX,
        m_editorCursorY);

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
        const WorldVec3 p = camera.eye + rayDir * t;
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
            const WorldVec3 hit = camera.eye + rayDir * hitT;
            m_editorBrushLocalX = std::clamp(hit.x, bounds.minX, bounds.maxX);
            m_editorBrushLocalZ = std::clamp(hit.z, bounds.minZ, bounds.maxZ);
            m_editorBrushVisible = true;
            if (m_editorLmbHeld && m_editorTerrainToolActive && BrushDiagLogs())
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

uint32_t TerrainRenderer::ActiveSplatLayerSpan()
{
    if (m_activeSplatLayerSpan < 0)
    {
        m_activeSplatLayerSpan = static_cast<std::int32_t>(
            EstimateActiveSplatLayerSpan(m_splatABytes, m_splatBBytes, m_splatWidth, m_splatHeight));
    }
    return static_cast<uint32_t>(m_activeSplatLayerSpan);
}

void TerrainRenderer::MarkSplatDirty(size_t splatIndex)
{
    // Every per-texel splat write (paint, undo) comes through here, after the write. Grow the cached
    // active layer span by this texel's layers instead of dropping it: a span that only grows stays
    // correct (the shader skips zero weights), and a full rescan per painted frame cost ~1 ms.
    const size_t splatByte = splatIndex * 4u;
    if (m_activeSplatLayerSpan >= 0 && splatByte + 3u < m_splatABytes.size() && splatByte + 3u < m_splatBBytes.size())
    {
        for (int layer = 0; layer < 4; ++layer)
        {
            if (m_splatABytes[splatByte + static_cast<size_t>(layer)] > 0)
                m_activeSplatLayerSpan = std::max(m_activeSplatLayerSpan, layer + 1);
            if (m_splatBBytes[splatByte + static_cast<size_t>(layer)] > 0)
                m_activeSplatLayerSpan = std::max(m_activeSplatLayerSpan, layer + 5);
        }
    }
    else
    {
        m_activeSplatLayerSpan = -1;
    }
    if (m_splatWidth > 0)
    {
        const uint32_t texelX = static_cast<uint32_t>(splatIndex % m_splatWidth);
        const uint32_t texelY = static_cast<uint32_t>(splatIndex / m_splatWidth);
        m_splatDirtyRect.minX = std::min(m_splatDirtyRect.minX, texelX);
        m_splatDirtyRect.minY = std::min(m_splatDirtyRect.minY, texelY);
        m_splatDirtyRect.maxX = std::max(m_splatDirtyRect.maxX, texelX);
        m_splatDirtyRect.maxY = std::max(m_splatDirtyRect.maxY, texelY);
    }
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

void TerrainRenderer::ApplyEditorBrush(ixrhi::IXRHIDevice& rhi, double deltaSeconds)
{
    m_rhi = &rhi;
    if (!m_editorBrushVisible || m_heightCmGrid.empty() || !m_vertexBuffer ||
        m_heightGridWidth < 2 || m_heightGridHeight < 2 || m_chunkSizeCells == 0)
    {
        if (m_editorLmbHeld && m_editorTerrainToolActive)
        {
            Tracenf("[TEDIT-DIAG] %s apply skipped terrainId=%p brushVisible=%d vertexBuffer=%p heightGrid=%ux%u chunkCells=%u",
                m_editorTool == MapEditorTool::Paint ? "splat" : "sculpt",
                m_sceneTerrainActive ? static_cast<void*>(this) : nullptr,
                m_editorBrushVisible ? 1 : 0,
                m_vertexBuffer ? static_cast<const void*>(m_vertexBuffer.get()) : nullptr,
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
        const uint32_t minX = static_cast<uint32_t>(std::max(0.0f, xm::Floor(centerSplatX - radiusSplatX)));
        const uint32_t minY = static_cast<uint32_t>(std::max(0.0f, xm::Floor(centerSplatY - radiusSplatY)));
        const uint32_t maxX = std::min(m_splatWidth - 1u, static_cast<uint32_t>(xm::Ceil(centerSplatX + radiusSplatX)));
        const uint32_t maxY = std::min(m_splatHeight - 1u, static_cast<uint32_t>(xm::Ceil(centerSplatY + radiusSplatY)));
        bool changed = false;
        std::uint32_t changedCells = 0;
        std::unordered_set<std::uint32_t> touchedChunks;
        for (uint32_t sy = minY; sy <= maxY; ++sy)
        {
            for (uint32_t sx = minX; sx <= maxX; ++sx)
            {
                const float dx = (static_cast<float>(sx) - centerSplatX) / std::max(splatScaleX, 0.001f) * m_cellScaleMeters;
                const float dy = (static_cast<float>(sy) - centerSplatY) / std::max(splatScaleY, 0.001f) * m_cellScaleMeters;
                const float dist = xm::Sqrt(dx * dx + dy * dy);
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
        if (BrushDiagLogs())
        {
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
        }
        ++m_brushDiagApplications;
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

    const uint32_t minX = static_cast<uint32_t>(std::max(0.0f, xm::Floor(centerGridX - radiusCells)));
    const uint32_t minY = static_cast<uint32_t>(std::max(0.0f, xm::Floor(centerGridY - radiusCells)));
    const uint32_t maxX = std::min(m_heightGridWidth - 1u, static_cast<uint32_t>(xm::Ceil(centerGridX + radiusCells)));
    const uint32_t maxY = std::min(m_heightGridHeight - 1u, static_cast<uint32_t>(xm::Ceil(centerGridY + radiusCells)));
    std::uint32_t changedHeights = 0;
    float maxHeightDeltaCm = 0.0f;
    std::unordered_set<std::uint32_t> touchedChunks;

    // Mapped-write parity via Read-modify-Write over the touched index span
    // (see ApplyLegacyHeightBrush): same GPU bytes, one Write per stroke.
    const std::uint64_t spanFirst = static_cast<std::uint64_t>(minY) * m_heightGridWidth + minX;
    const std::uint64_t spanLast = static_cast<std::uint64_t>(maxY) * m_heightGridWidth + maxX;
    const std::uint64_t spanCount = spanLast - spanFirst + 1u;
    std::vector<Vertex> span(static_cast<std::size_t>(spanCount));
    m_vertexEditBuffer->Read(spanFirst * sizeof(Vertex), span.data(), spanCount * sizeof(Vertex));

    for (uint32_t gy = minY; gy <= maxY; ++gy)
    {
        for (uint32_t gx = minX; gx <= maxX; ++gx)
        {
            const float dx = (static_cast<float>(gx) - centerGridX) * m_cellScaleMeters;
            const float dy = (static_cast<float>(gy) - centerGridY) * m_cellScaleMeters;
            const float dist = xm::Sqrt(dx * dx + dy * dy);
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
            if (xm::Abs(heightDeltaCm) <= 0.001f)
                continue;
            if (xm::Abs(heightDeltaCm) > xm::Abs(maxHeightDeltaCm))
                maxHeightDeltaCm = heightDeltaCm;
            RecordHeightUndo(index);
            m_heightCmGrid[index] = newHeight;
            span[static_cast<std::size_t>(index) - static_cast<std::size_t>(spanFirst)].position[1] =
                newHeight * 0.01f;
            MarkHeightDirty(index);
            const std::uint32_t cx = m_chunkSizeCells > 0 ? std::min(gx / m_chunkSizeCells, chunksX - 1u) : 0;
            const std::uint32_t cy = m_chunkSizeCells > 0 ? std::min(gy / m_chunkSizeCells, chunksY - 1u) : 0;
            touchedChunks.insert((cy << 16u) | (cx & 0xffffu));
            ++changedHeights;
        }
    }
    if (changedHeights > 0)
    {
        ExpandTerrainChunkBounds(span.data(), span.size());
        m_vertexEditBuffer->Write(spanFirst * sizeof(Vertex), span.data(), spanCount * sizeof(Vertex));
        m_vertexBufferUploadPending = true;
    }
    if (BrushDiagLogs())
    {
        Tracenf("[TEDIT-DIAG] sculpt apply terrainId=%p tool=%s cellRange=(%u,%u)-(%u,%u) changedCells=%u gpuUpload=%s remesh=no",
            m_sceneTerrainActive ? static_cast<void*>(this) : nullptr,
            TeditToolName(m_editorTool),
            minX,
            minY,
            maxX,
            maxY,
            changedHeights,
            changedHeights > 0 ? "read-modify-write" : "no");
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
    ++m_brushDiagApplications;
}

bool TerrainRenderer::RefreshSplatTextures(ixrhi::IXRHIDevice& rhi)
{
    if (m_splatABytes.empty() || m_splatBBytes.empty())
        return false;
    const bool okA = UpdateRgbaTexture2D(rhi, m_splatA, m_splatABytes);
    const bool okB = UpdateRgbaTexture2D(rhi, m_splatB, m_splatBBytes);
    m_editorSplatGpuDirty = !(okA && okB);
    if (okA && okB)
        m_splatDirtyRect = {};
    if (BrushDiagLogs() || !(okA && okB))  // every frame while painting: only a stroke's first few
        Tracenf("[TEDIT-DIAG] splat gpu upload terrainId=%p targets=%p/%p textureSize=%ux%u cpuSize=%ux%u okA=%d okB=%d gpuUpload=%s",
            m_sceneTerrainActive ? static_cast<void*>(this) : nullptr,
            m_splatA.image ? static_cast<const void*>(m_splatA.image.get()) : nullptr,
            m_splatB.image ? static_cast<const void*>(m_splatB.image.get()) : nullptr,
            m_splatA.width,
            m_splatA.height,
            m_splatWidth,
            m_splatHeight,
            okA ? 1 : 0,
            okB ? 1 : 0,
            (okA && okB) ? "yes" : "no");
    return okA && okB;
}

void TerrainRenderer::UndoLastEditorStroke(ixrhi::IXRHIDevice& rhi)
{
    m_rhi = &rhi;
    EndEditorStroke();
    if (m_undoStack.empty())
    {
        Tracen("[TERRAIN-EDITOR] undo requested but stack is empty");
        return;
    }

    EditorUndoEntry entry = std::move(m_undoStack.back());
    m_undoStack.pop_back();

    if (!entry.heights.empty() && m_vertexBuffer)
    {
        // Read-modify-Write over the undo span (same bytes the old mapped
        // writes produced).
        std::uint64_t spanFirst = std::numeric_limits<std::uint64_t>::max();
        std::uint64_t spanLast = 0;
        for (const HeightUndo& undo : entry.heights)
        {
            if (undo.index >= m_heightCmGrid.size())
                continue;
            spanFirst = std::min(spanFirst, static_cast<std::uint64_t>(undo.index));
            spanLast = std::max(spanLast, static_cast<std::uint64_t>(undo.index));
        }
        if (spanLast >= spanFirst)
        {
            const std::uint64_t spanCount = spanLast - spanFirst + 1u;
            std::vector<Vertex> span(static_cast<std::size_t>(spanCount));
            m_vertexEditBuffer->Read(spanFirst * sizeof(Vertex), span.data(), spanCount * sizeof(Vertex));
            for (const HeightUndo& undo : entry.heights)
            {
                if (undo.index >= m_heightCmGrid.size())
                    continue;
                m_heightCmGrid[undo.index] = undo.oldCm;
                span[static_cast<std::size_t>(undo.index) - static_cast<std::size_t>(spanFirst)].position[1] =
                    undo.oldCm * 0.01f;
                MarkHeightDirty(undo.index);
            }
            ExpandTerrainChunkBounds(span.data(), span.size());
            m_vertexEditBuffer->Write(spanFirst * sizeof(Vertex), span.data(), spanCount * sizeof(Vertex));
            m_vertexBufferUploadPending = true;
        }
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
        m_editorSplatGpuDirty = true;  // uploaded in the frame (MarkSplatDirty grew the rectangle)
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

bool TerrainRenderer::ReloadCurrentMap(ixrhi::IXRHIDevice& rhi)
{
    if (m_loadedMapDirectory.empty())
        return false;
    Tracen("[TERRAIN-EDITOR] reload heightmap from disk");
    return LoadMap(rhi, m_loadedMapDirectory, m_loadedServerX, m_loadedServerY);
}

bool TerrainRenderer::CreateFallbackTexture(ixrhi::IXRHIDevice& rhi)
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
    if (!UploadRgbaTextureArray(rhi, "terrain_palette_fallback", kSize, kSize, kLayers, pixels,
            ixrhi::IXRHIFormat::R8G8B8A8Srgb, m_baseTexture) ||
        !UploadRgbaTextureArray(rhi, "terrain_normal_fallback", kSize, kSize, kLayers, normals,
            ixrhi::IXRHIFormat::R8G8B8A8Unorm, m_normalTexture) ||
        !UploadRgbaTextureArray(rhi, "terrain_orm_fallback", kSize, kSize, kLayers,
            PackOcclusionRoughnessMetallic(ao, roughness, metallic), ixrhi::IXRHIFormat::R8G8B8A8Unorm, m_aoTexture) ||
        !UploadR8TextureArray(rhi, "terrain_height_fallback", kSize, kSize, kLayers, height, m_heightTexture))
    {
        return false;
    }
    m_roughnessTexture = m_aoTexture;  // the packed array serves all three slots
    m_metallicTexture = m_aoTexture;
    return true;
}

bool TerrainRenderer::CreateFallbackMask(ixrhi::IXRHIDevice& rhi)
{
    // 1x1 white R8 mask (created but currently unbound — parity with the
    // pre-migration resource set; trivially cheap).
    m_fallbackMask = {};
    const std::uint8_t pixel = 255;
    ixrhi::IXRHITextureDesc imageDesc;
    imageDesc.width = 1;
    imageDesc.height = 1;
    imageDesc.format = ixrhi::IXRHIFormat::R8Unorm;
    imageDesc.usage = ixrhi::IXRHITextureUsage::Sampled | ixrhi::IXRHITextureUsage::TransferDst;
    imageDesc.debugName = "Terrain:terrain_full_mask";
    m_fallbackMask.image = rhi.CreateTexture(imageDesc, &pixel, sizeof(pixel));
    if (!m_fallbackMask.image)
        return false;
    m_fallbackMask.sampler = CreateRhiSampler(rhi,
        ixrhi::IXRHISamplerAddress::ClampToEdge,
        /*trilinear=*/false,
        /*maxLod=*/1.0f,
        /*mipLodBias=*/0.0f,
        "Terrain:terrain_full_mask:Sampler");
    if (!m_fallbackMask.sampler)
    {
        m_fallbackMask = {};
        return false;
    }

    m_fallbackMask.format = ixrhi::IXRHIFormat::R8Unorm;
    m_fallbackMask.width = 1;
    m_fallbackMask.height = 1;
    m_fallbackMask.mipLevels = 1;
    m_fallbackMask.arrayLayers = 1;
    m_fallbackMask.name = "terrain_full_mask";
    return true;
}

bool TerrainRenderer::CreateFallbackSplatTextures(ixrhi::IXRHIDevice& rhi)
{
    std::vector<uint8_t> splatA(4, 0);
    std::vector<uint8_t> splatB(4, 0);
    splatA[2] = 255; // fallback grass.
    return UploadRgbaTexture2D(rhi, "splat_a_fallback", 1, 1, splatA,
               ixrhi::IXRHISamplerAddress::ClampToEdge, m_splatA) &&
            UploadRgbaTexture2D(rhi, "splat_b_fallback", 1, 1, splatB,
                ixrhi::IXRHISamplerAddress::ClampToEdge, m_splatB);
}

bool TerrainRenderer::CreateSceneSplatTextures(ixrhi::IXRHIDevice& rhi)
{
    if (m_splatWidth == 0 || m_splatHeight == 0 ||
        m_splatABytes.size() != static_cast<size_t>(m_splatWidth) * m_splatHeight * 4u ||
        m_splatBBytes.size() != static_cast<size_t>(m_splatWidth) * m_splatHeight * 4u)
    {
        return false;
    }

    const bool okA = UploadRgbaTexture2D(rhi, "splat_a_scene", m_splatWidth, m_splatHeight, m_splatABytes,
        ixrhi::IXRHISamplerAddress::ClampToEdge, m_splatA);
    const bool okB = UploadRgbaTexture2D(rhi, "splat_b_scene", m_splatWidth, m_splatHeight, m_splatBBytes,
        ixrhi::IXRHISamplerAddress::ClampToEdge, m_splatB);
    Tracenf("[TERRAIN-SPLAT] scene splat textures created size=%ux%u okA=%d okB=%d",
        m_splatWidth,
        m_splatHeight,
        okA ? 1 : 0,
        okB ? 1 : 0);
    return okA && okB;
}

bool TerrainRenderer::LoadTerrainPalette(ixrhi::IXRHIDevice& rhi,
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

    return LoadTerrainPaletteFromPaths(rhi, slots);
}

bool TerrainRenderer::LoadTerrainPaletteFromPaths(ixrhi::IXRHIDevice& rhi, const std::array<MapEditorPaletteSlot, 8>& slots)
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
    Texture orm{};
    Texture heightTex{};
    if (!UploadRgbaTextureArray(rhi, "terrain_palette", width, height, static_cast<uint32_t>(images.size()), pixels,
            ixrhi::IXRHIFormat::R8G8B8A8Srgb, palette) ||
        !UploadRgbaTextureArray(rhi, "terrain_normals", width, height, static_cast<uint32_t>(images.size()), normalPixels,
            ixrhi::IXRHIFormat::R8G8B8A8Unorm, normals) ||
        !UploadRgbaTextureArray(rhi, "terrain_orm", width, height, static_cast<uint32_t>(images.size()),
            PackOcclusionRoughnessMetallic(aoPixels, roughnessPixels, metallicPixels),
            ixrhi::IXRHIFormat::R8G8B8A8Unorm, orm) ||
        !UploadR8TextureArray(rhi, "terrain_height", width, height, static_cast<uint32_t>(images.size()), heightPixels, heightTex))
    {
        return false;
    }

    m_baseTexture = std::move(palette);
    m_normalTexture = std::move(normals);
    // One array for the three (the shader takes them in one fetch); it stays bound to all three slots.
    m_aoTexture = orm;
    m_roughnessTexture = orm;
    m_metallicTexture = std::move(orm);
    m_heightTexture = std::move(heightTex);
    m_paletteSlots = slots;
    m_materialParamsDirty = true;
    UpdateBindGroup();
    Tracenf("[TERRAIN-PALETTE] loaded 8-layer PBR palette size=%ux%u diffuse=%s normal=%s orm_height=R8",
        m_baseTexture.width,
        m_baseTexture.height,
        RhiFormatName(m_baseTexture.format),
        RhiFormatName(m_normalTexture.format));
    if (!m_triPerfPaletteLogged)
    {
        TraceDiagf("[TRI-PERF] palette size=%ux%u layers=%zu format(diffuse=%s normal=%s ao=%s roughness=%s metallic=%s height=%s) mips(diffuse=%u normal=%u ao=%u roughness=%u metallic=%u height=%u)",
            m_baseTexture.width,
            m_baseTexture.height,
            images.size(),
            RhiFormatName(m_baseTexture.format),
            RhiFormatName(m_normalTexture.format),
            RhiFormatName(m_aoTexture.format),
            RhiFormatName(m_roughnessTexture.format),
            RhiFormatName(m_metallicTexture.format),
            RhiFormatName(m_heightTexture.format),
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


bool TerrainRenderer::CreateBindGroup(ixrhi::IXRHIDevice& rhi)
{
    for (const auto& buffer : m_uniformBuffers)
    {
        if (!buffer)
        {
            Tracen("[TERRAIN] CreateBindGroup skipped: uniform buffer is not ready");
            return false;
        }
    }
    for (const auto& buffer : m_uniformBuffersSecondary)
    {
        if (!buffer)
        {
            Tracen("[TERRAIN] CreateBindGroup skipped: secondary uniform buffer is not ready");
            return false;
        }
    }
    for (const auto& buffer : m_uniformBuffersReflection)
    {
        if (!buffer)
        {
            Tracen("[TERRAIN] CreateBindGroup skipped: reflection uniform buffer is not ready");
            return false;
        }
    }

    if (!m_bindLayout)
    {
        // Binding 0 = per-view UBO (vertex+fragment); bindings 1..9 =
        // combined image samplers (fragment): palette array, splat A/B,
        // normal/ao/roughness/metallic/height arrays, shadow depth array.
        const ixrhi::IXRHIShaderStage allStages =
            ixrhi::IXRHIShaderStage::Vertex | ixrhi::IXRHIShaderStage::Fragment;
        const std::vector<ixrhi::IXRHIBinding> bindings = {
            {0, ixrhi::IXRHIBindingType::UniformBuffer, allStages},
            {1, ixrhi::IXRHIBindingType::SampledTexture, ixrhi::IXRHIShaderStage::Fragment},
            {2, ixrhi::IXRHIBindingType::SampledTexture, ixrhi::IXRHIShaderStage::Fragment},
            {3, ixrhi::IXRHIBindingType::SampledTexture, ixrhi::IXRHIShaderStage::Fragment},
            {4, ixrhi::IXRHIBindingType::SampledTexture, ixrhi::IXRHIShaderStage::Fragment},
            {5, ixrhi::IXRHIBindingType::SampledTexture, ixrhi::IXRHIShaderStage::Fragment},
            {6, ixrhi::IXRHIBindingType::SampledTexture, ixrhi::IXRHIShaderStage::Fragment},
            {7, ixrhi::IXRHIBindingType::SampledTexture, ixrhi::IXRHIShaderStage::Fragment},
            {8, ixrhi::IXRHIBindingType::SampledTexture, ixrhi::IXRHIShaderStage::Fragment},
            {9, ixrhi::IXRHIBindingType::SampledTexture, ixrhi::IXRHIShaderStage::Fragment},
        };
        m_bindLayout = rhi.CreateBindGroupLayout(bindings);
        if (!m_bindLayout)
            return false;
    }

    // Two camera views (primary = Scene View / free-fly, secondary = Game view
    // / Main Camera), plus the water reflection's mirrored camera, each with one slot per
    // frame-in-flight. Recreating the group retires the old sets (RAII); in-flight frames keep
    // theirs alive through the group's shared keeps.
    m_bindGroup = rhi.CreateBindGroup(*m_bindLayout, kFramesInFlight * (kReflectionUniformView + 1u));
    if (!m_bindGroup)
        return false;

    UpdateBindGroup();
    return true;
}

bool TerrainRenderer::CreateShadowResources(ixrhi::IXRHIDevice& rhi)
{
    if (m_shadowTexture)
        return true;

    // 4-cascade depth array sampled by the main pass (binding 9) with a
    // comparison sampler; one depth-only target per cascade slice.
    ixrhi::IXRHITextureDesc shadowDesc;
    shadowDesc.width = kShadowResolution;
    shadowDesc.height = kShadowResolution;
    shadowDesc.mipLevels = 1;
    shadowDesc.arrayLayers = kShadowCascadeCount;
    shadowDesc.format = ixrhi::IXRHIFormat::D32Float;
    shadowDesc.usage = ixrhi::IXRHITextureUsage::DepthStencilAttachment | ixrhi::IXRHITextureUsage::Sampled;
    shadowDesc.debugName = "Terrain:ShadowCascades";
    m_shadowTexture = rhi.CreateTexture(shadowDesc, nullptr, 0);
    if (!m_shadowTexture)
        return false;

    ixrhi::IXRHISamplerDesc samplerDesc;
    samplerDesc.minFilter = ixrhi::IXRHISamplerFilter::Linear;
    samplerDesc.magFilter = ixrhi::IXRHISamplerFilter::Linear;
    samplerDesc.mipmapFilter = ixrhi::IXRHISamplerFilter::Nearest;
    samplerDesc.addressU = ixrhi::IXRHISamplerAddress::ClampToBorder;
    samplerDesc.addressV = ixrhi::IXRHISamplerAddress::ClampToBorder;
    samplerDesc.addressW = ixrhi::IXRHISamplerAddress::ClampToBorder;
    samplerDesc.compareEnable = true;
    samplerDesc.compareOp = ixrhi::IXRHICompareOp::LessOrEqual;
    samplerDesc.maxLod = 0.0f;
    samplerDesc.debugName = "Terrain:ShadowSampler";
    m_shadowSampler = rhi.CreateSampler(samplerDesc);
    if (!m_shadowSampler)
    {
        m_shadowTexture.reset();
        return false;
    }

    for (uint32_t i = 0; i < kShadowCascadeCount; ++i)
    {
        ixrhi::IXRHIRenderTargetDesc targetDesc;
        targetDesc.color = nullptr;
        targetDesc.depth = m_shadowTexture;
        targetDesc.depthLayer = i;
        targetDesc.depthLoad = ixrhi::IXRHILoadOp::Clear;
        targetDesc.depthStore = ixrhi::IXRHIStoreOp::Store;
        targetDesc.clearDepth = 1.0f;
        targetDesc.debugName = "Terrain:ShadowCascade" + std::to_string(i);
        m_shadowTargets[i] = rhi.CreateRenderTarget(targetDesc);
        if (!m_shadowTargets[i])
        {
            DestroyShadowResources();
            return false;
        }
    }

    if (!CreateShadowPipeline(rhi))
    {
        DestroyShadowResources();
        return false;
    }

    Tracen("[SHADOW] Cascade Shadow Maps: 4 cascades x 2048x2048 D32_SFLOAT, PCF 5x5");
    return true;
}

void TerrainRenderer::UpdateBindGroup()
{
    if (!m_bindGroup)
        return;

    auto writeSlot = [this](std::uint32_t slot, const std::shared_ptr<ixrhi::IXRHIBuffer>& uniformBuffer) {
        if (!uniformBuffer || !m_baseTexture.image || !m_baseTexture.sampler ||
            !m_normalTexture.image || !m_normalTexture.sampler ||
            !m_aoTexture.image || !m_aoTexture.sampler ||
            !m_roughnessTexture.image || !m_roughnessTexture.sampler ||
            !m_metallicTexture.image || !m_metallicTexture.sampler ||
            !m_heightTexture.image || !m_heightTexture.sampler ||
            !m_shadowTexture || !m_shadowSampler ||
            !m_splatA.image || !m_splatA.sampler || !m_splatB.image || !m_splatB.sampler)
            return;

        m_bindGroup->UpdateBuffer(slot, 0, uniformBuffer, 0, sizeof(UniformBlock));
        m_bindGroup->UpdateTexture(slot, 1, m_baseTexture.image, m_baseTexture.sampler);
        m_bindGroup->UpdateTexture(slot, 2, m_splatA.image, m_splatA.sampler);
        m_bindGroup->UpdateTexture(slot, 3, m_splatB.image, m_splatB.sampler);
        m_bindGroup->UpdateTexture(slot, 4, m_normalTexture.image, m_normalTexture.sampler);
        m_bindGroup->UpdateTexture(slot, 5, m_aoTexture.image, m_aoTexture.sampler);
        m_bindGroup->UpdateTexture(slot, 6, m_roughnessTexture.image, m_roughnessTexture.sampler);
        m_bindGroup->UpdateTexture(slot, 7, m_metallicTexture.image, m_metallicTexture.sampler);
        m_bindGroup->UpdateTexture(slot, 8, m_heightTexture.image, m_heightTexture.sampler);
        m_bindGroup->UpdateTexture(slot, 9, m_shadowTexture, m_shadowSampler);
    };

    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        writeSlot(frame, m_uniformBuffers[frame]);
        writeSlot(kFramesInFlight + frame, m_uniformBuffersSecondary[frame]);
        writeSlot(kReflectionUniformView * kFramesInFlight + frame, m_uniformBuffersReflection[frame]);
    }
}

bool TerrainRenderer::CreateWaterResources(ixrhi::IXRHIDevice& rhi)
{
    const bool normals = CreateWaterNormalTextures(rhi);
    const bool reflectionResources = normals ? CreateOrRecreateWaterReflectionResources(rhi, true) : false;
    const bool descriptors = reflectionResources ? CreateWaterBindGroup(rhi) : false;
    const bool reflectionPipeline = descriptors ? CreateWaterReflectionPipeline(rhi) : false;
    const bool pipeline = reflectionPipeline ? CreateWaterPipeline(rhi) : false;
    Tracen("[WATER-OBJ] Water body renderer resources ready");
    return normals && reflectionResources && descriptors && reflectionPipeline && pipeline;
}

bool TerrainRenderer::LoadWaterBodies(ixrhi::IXRHIDevice& rhi, const std::string& mapDirectory)
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
        if (m_waterBindGroup)
            CreateWaterBindGroup(rhi);
        return true;
    }

    std::vector<WaterBody> bodies;
    std::string error;
    if (!client::render::LoadWaterBodiesBinary(*bytes, bodies, &error))
    {
        Tracenf("[WATER-OBJ] failed to parse %s: %s", waterPath.c_str(), error.c_str());
        if (m_waterBindGroup)
            CreateWaterBindGroup(rhi);
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
        if (!CreateWaterBodyUniformBuffers(rhi, gpu) || !CreateWaterBodyMesh(rhi, gpu))
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

    if (m_waterBindGroup)
        CreateWaterBindGroup(rhi);

    Tracenf("[WATER-OBJ] active water bodies: %zu", m_waterBodies.size());
    return true;
}

bool TerrainRenderer::CreateWaterBodyUniformBuffers(ixrhi::IXRHIDevice& rhi, WaterBodyGpu& waterBody)
{
    auto createSet = [&](std::array<std::shared_ptr<ixrhi::IXRHIBuffer>, kFramesInFlight>& buffers,
                         const char* debugName) -> bool {
        for (auto& buffer : buffers)
        {
            buffer = CreateRhiBuffer(rhi, sizeof(WaterUniformBlock), ixrhi::IXRHIBufferUsage::Uniform, nullptr, debugName);
            if (!buffer)
                return false;
        }
        return true;
    };
    return createSet(waterBody.uniformBuffers, "Terrain:WaterUBO") &&
        createSet(waterBody.uniformBuffersSecondary, "Terrain:WaterUBO secondary");
}

bool TerrainRenderer::CreateWaterBodyMesh(ixrhi::IXRHIDevice& rhi, WaterBodyGpu& waterBody)
{
    waterBody.vertexBuffer.reset();
    waterBody.indexBuffer.reset();
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

    waterBody.boundsMin = {vertices[0].position[0], vertices[0].position[1], vertices[0].position[2]};
    waterBody.boundsMax = waterBody.boundsMin;
    for (const WaterVertex& vertex : vertices)
    {
        waterBody.boundsMin.x = std::min(waterBody.boundsMin.x, vertex.position[0]);
        waterBody.boundsMin.y = std::min(waterBody.boundsMin.y, vertex.position[1]);
        waterBody.boundsMin.z = std::min(waterBody.boundsMin.z, vertex.position[2]);
        waterBody.boundsMax.x = std::max(waterBody.boundsMax.x, vertex.position[0]);
        waterBody.boundsMax.y = std::max(waterBody.boundsMax.y, vertex.position[1]);
        waterBody.boundsMax.z = std::max(waterBody.boundsMax.z, vertex.position[2]);
    }

    // Device-local: the GPU reads the (often large) water mesh in every pass that draws it.
    waterBody.vertexBuffer = CreateStaticRhiBuffer(rhi,
        sizeof(WaterVertex) * vertices.size(),
        ixrhi::IXRHIBufferUsage::Vertex,
        vertices.data(),
        "Terrain:WaterVB");
    waterBody.indexBuffer = CreateStaticRhiBuffer(rhi,
        sizeof(uint32_t) * indices.size(),
        ixrhi::IXRHIBufferUsage::Index,
        indices.data(),
        "Terrain:WaterIB");
    waterBody.indexCount = static_cast<uint32_t>(indices.size());
    Tracenf("[WATER-OBJ-6] Vertex alpha computed: body_id=%u fade_zone_vertices=%u fully_water_vertices=%u",
        body.id, fadeZoneVertices, fullyWaterVertices);
    return waterBody.vertexBuffer != nullptr && waterBody.indexBuffer != nullptr;
}

bool TerrainRenderer::CreateWaterNormalTextures(ixrhi::IXRHIDevice& rhi)
{
    // Fine ripples and broad swells, each a different random wave set, with full mip chains.
    constexpr uint32_t kSize = 256;
    const std::vector<std::uint8_t> small = GenerateWaterNormalPixels(kSize, kSize, 0x5157u, 5.0f, 28.0f, 0.30f);
    const std::vector<std::uint8_t> large = GenerateWaterNormalPixels(kSize, kSize, 0xB16Bu, 2.0f, 10.0f, 0.24f);
    return UploadRgbaTexture2D(rhi, "water_wave_small", kSize, kSize, small,
               ixrhi::IXRHISamplerAddress::Repeat, m_waterNormalSmall, ixrhi::IXRHIFormat::R8G8B8A8Unorm, true) &&
            UploadRgbaTexture2D(rhi, "water_wave_large", kSize, kSize, large,
                ixrhi::IXRHISamplerAddress::Repeat, m_waterNormalLarge, ixrhi::IXRHIFormat::R8G8B8A8Unorm, true);
}

bool TerrainRenderer::CreateWaterBindGroup(ixrhi::IXRHIDevice& rhi)
{
    if (!m_waterBindLayout)
    {
        // Binding 0 = per-body UBO (vertex+fragment); bindings 1..6 =
        // combined image samplers (fragment): small/large wave normals,
        // reflection color, scene color/depth refraction inputs, material
        // diffuse.
        const ixrhi::IXRHIShaderStage allStages =
            ixrhi::IXRHIShaderStage::Vertex | ixrhi::IXRHIShaderStage::Fragment;
        const std::vector<ixrhi::IXRHIBinding> bindings = {
            {0, ixrhi::IXRHIBindingType::UniformBuffer, allStages},
            {1, ixrhi::IXRHIBindingType::SampledTexture, ixrhi::IXRHIShaderStage::Fragment},
            {2, ixrhi::IXRHIBindingType::SampledTexture, ixrhi::IXRHIShaderStage::Fragment},
            {3, ixrhi::IXRHIBindingType::SampledTexture, ixrhi::IXRHIShaderStage::Fragment},
            {4, ixrhi::IXRHIBindingType::SampledTexture, ixrhi::IXRHIShaderStage::Fragment},
            {5, ixrhi::IXRHIBindingType::SampledTexture, ixrhi::IXRHIShaderStage::Fragment},
            {6, ixrhi::IXRHIBindingType::SampledTexture, ixrhi::IXRHIShaderStage::Fragment},
        };
        m_waterBindLayout = rhi.CreateBindGroupLayout(bindings);
        if (!m_waterBindLayout)
            return false;
    }

    // One slot per (body, view, frame); bodies are capped at
    // kMaxWaterBodyDraws. Recreating the group retires the old sets (RAII).
    m_waterBindGroup = rhi.CreateBindGroup(*m_waterBindLayout,
        kMaxWaterBodyDraws * 2u * kFramesInFlight);
    if (!m_waterBindGroup)
        return false;

    UpdateWaterBindGroup();
    return true;
}

void TerrainRenderer::WriteWaterBindGroupSets(std::uint32_t bodyIndex,
                                                uint32_t viewIndex,
                                                uint32_t frameIndex,
                                                const WaterMaterialTextureSet* materialTextures)
{
    if (!m_waterBindGroup || bodyIndex >= m_waterBodies.size() || frameIndex >= kFramesInFlight)
        return;
    const WaterBodyGpu& waterBody = m_waterBodies[bodyIndex];
    const std::shared_ptr<ixrhi::IXRHIBuffer>& uniformBuffer =
        viewIndex == 0 ? waterBody.uniformBuffers[frameIndex] : waterBody.uniformBuffersSecondary[frameIndex];
    if (!uniformBuffer)
        return;

    // Fallback chain preserved exactly: material textures, else wave normals,
    // else refraction snapshots fall back to the wave normals as well.
    const Texture* normalA = materialTextures && materialTextures->normalA.image ? &materialTextures->normalA : &m_waterNormalSmall;
    const Texture* normalB = materialTextures && materialTextures->normalB.image ? &materialTextures->normalB : normalA;
    const Texture* diffuse = materialTextures && materialTextures->diffuse.image ? &materialTextures->diffuse : &m_waterNormalSmall;
    const Texture* normalBSafe = (normalB->image && normalB->sampler) ? normalB : &m_waterNormalLarge;
    if (!normalA->image || !normalA->sampler || !normalBSafe->image || !normalBSafe->sampler ||
        !m_waterReflection.color || !m_waterReflection.sampler || !diffuse->image || !diffuse->sampler)
        return;

    const std::uint32_t slot = (bodyIndex * 2u + (viewIndex == 0 ? 0u : 1u)) * kFramesInFlight + frameIndex;
    m_waterBindGroup->UpdateBuffer(slot, 0, uniformBuffer, 0, sizeof(WaterUniformBlock));
    m_waterBindGroup->UpdateTexture(slot, 1, normalA->image, normalA->sampler);
    m_waterBindGroup->UpdateTexture(slot, 2, normalBSafe->image, normalBSafe->sampler);
    m_waterBindGroup->UpdateTexture(slot, 3, m_waterReflection.color, m_waterReflection.sampler);
    // Refraction snapshots are IXRHI-owned (shared lifetime); sampler and
    // view resolve independently with wave-normal fallbacks, exactly like
    // the old null-view path.
    m_waterBindGroup->UpdateTexture(slot,
        4,
        m_waterSceneColor ? m_waterSceneColor : m_waterNormalSmall.image,
        m_waterSceneSampler ? m_waterSceneSampler : m_waterNormalSmall.sampler);
    m_waterBindGroup->UpdateTexture(slot,
        5,
        m_waterSceneDepth ? m_waterSceneDepth : m_waterNormalLarge.image,
        m_waterSceneSampler ? m_waterSceneSampler : m_waterNormalLarge.sampler);
    m_waterBindGroup->UpdateTexture(slot, 6, diffuse->image, diffuse->sampler);
}

void TerrainRenderer::UpdateWaterBindGroup()
{
    if (!m_waterBindGroup || !m_waterNormalSmall.image || !m_waterNormalLarge.image || !m_waterReflection.color)
        return;

    for (std::uint32_t bodyIndex = 0; bodyIndex < static_cast<std::uint32_t>(m_waterBodies.size()); ++bodyIndex)
    {
        const WaterMaterialTextureSet* textures = ResolveWaterMaterialTextures(m_waterBodies[bodyIndex].body);
        for (uint32_t viewIndex = 0; viewIndex < 2u; ++viewIndex)
        {
            for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
                WriteWaterBindGroupSets(bodyIndex, viewIndex, frame, textures);
        }
    }
}

bool TerrainRenderer::CreateOrRecreateWaterReflectionResources(ixrhi::IXRHIDevice& rhi, bool force)
{
    return CreateOrRecreateWaterReflectionResources(rhi, force, WaterConfig::ReflectionQuality::Half);
}

bool TerrainRenderer::CreateOrRecreateWaterReflectionResources(ixrhi::IXRHIDevice& rhi,
                                                               bool force,
                                                               WaterConfig::ReflectionQuality quality,
                                                               std::uint32_t viewWidth,
                                                               std::uint32_t viewHeight)
{
    m_rhi = &rhi;
    const std::uint32_t baseWidth = viewWidth > 0 ? viewWidth : rhi.GetMainSwapchain().Width();
    const std::uint32_t baseHeight = viewHeight > 0 ? viewHeight : rhi.GetMainSwapchain().Height();
    if (baseWidth == 0 || baseHeight == 0)
        return false;

    const uint32_t divisor = WaterReflectionDivisor(quality);
    const uint32_t width = std::max(1u, baseWidth / divisor);
    const uint32_t height = std::max(1u, baseHeight / divisor);
    const ixrhi::IXRHIFormat colorFormat = rhi.GetMainSwapchain().ColorFormat();
    const ixrhi::IXRHIFormat depthFormat = rhi.GetMainSwapchain().DepthFormat();

    if (!force && m_waterReflection.color && m_waterReflection.depth &&
        m_waterReflection.width == width && m_waterReflection.height == height &&
        m_waterReflection.quality == quality &&
        m_waterReflection.colorFormat == colorFormat && m_waterReflection.depthFormat == depthFormat)
    {
        return true;
    }

    const bool hadResources = m_waterReflection.color != nullptr || m_waterReflection.depth != nullptr ||
        m_waterReflection.target != nullptr;
    if (hadResources)
    {
        rhi.WaitIdle();
        Tracen("[WATER-2] Re-creating reflection resources");
    }
    DestroyWaterReflectionPipeline();
    DestroyWaterReflectionResources();

    m_waterReflection.width = width;
    m_waterReflection.height = height;
    m_waterReflection.quality = quality;
    m_waterReflection.colorFormat = colorFormat;
    m_waterReflection.depthFormat = depthFormat;

    ixrhi::IXRHITextureDesc colorDesc;
    colorDesc.width = width;
    colorDesc.height = height;
    colorDesc.format = colorFormat;
    colorDesc.usage = ixrhi::IXRHITextureUsage::ColorAttachment | ixrhi::IXRHITextureUsage::Sampled;
    colorDesc.debugName = "Terrain:ReflectionColor";
    m_waterReflection.color = rhi.CreateTexture(colorDesc, nullptr, 0);
    if (!m_waterReflection.color)
    {
        DestroyWaterReflectionResources();
        return false;
    }

    ixrhi::IXRHITextureDesc depthDesc;
    depthDesc.width = width;
    depthDesc.height = height;
    depthDesc.format = depthFormat;
    depthDesc.usage = ixrhi::IXRHITextureUsage::DepthStencilAttachment;
    depthDesc.debugName = "Terrain:ReflectionDepth";
    m_waterReflection.depth = rhi.CreateTexture(depthDesc, nullptr, 0);
    if (!m_waterReflection.depth)
    {
        DestroyWaterReflectionResources();
        return false;
    }

    // Color clears to the scene's own backdrop (what the reflected "sky" must show: black made the
    // water dark at every grazing view), depth to 1.0; depth is not stored (DONT_CARE) since only
    // the color is sampled afterwards.
    ixrhi::IXRHIRenderTargetDesc targetDesc;
    targetDesc.color = m_waterReflection.color;
    targetDesc.depth = m_waterReflection.depth;
    targetDesc.colorLoad = ixrhi::IXRHILoadOp::Clear;
    targetDesc.colorStore = ixrhi::IXRHIStoreOp::Store;
    targetDesc.depthLoad = ixrhi::IXRHILoadOp::Clear;
    targetDesc.depthStore = ixrhi::IXRHIStoreOp::DontCare;
    targetDesc.clearColor[0] = kSceneClearColor[0];
    targetDesc.clearColor[1] = kSceneClearColor[1];
    targetDesc.clearColor[2] = kSceneClearColor[2];
    targetDesc.clearColor[3] = kSceneClearColor[3];
    targetDesc.clearDepth = 1.0f;
    targetDesc.debugName = "Terrain:Reflection";
    m_waterReflection.target = rhi.CreateRenderTarget(targetDesc);
    if (!m_waterReflection.target)
    {
        DestroyWaterReflectionResources();
        return false;
    }

    ixrhi::IXRHISamplerDesc samplerDesc;
    samplerDesc.minFilter = ixrhi::IXRHISamplerFilter::Linear;
    samplerDesc.magFilter = ixrhi::IXRHISamplerFilter::Linear;
    samplerDesc.mipmapFilter = ixrhi::IXRHISamplerFilter::Linear;
    samplerDesc.addressU = ixrhi::IXRHISamplerAddress::ClampToEdge;
    samplerDesc.addressV = ixrhi::IXRHISamplerAddress::ClampToEdge;
    samplerDesc.addressW = ixrhi::IXRHISamplerAddress::ClampToEdge;
    samplerDesc.maxLod = 1.0f;
    samplerDesc.debugName = "Terrain:ReflectionSampler";
    m_waterReflection.sampler = rhi.CreateSampler(samplerDesc);
    if (!m_waterReflection.sampler)
    {
        DestroyWaterReflectionResources();
        return false;
    }

    const char* qualityName = "Half";
    if (quality == WaterConfig::ReflectionQuality::Quarter)
        qualityName = "Quarter";
    else if (quality == WaterConfig::ReflectionQuality::Full)
        qualityName = "Full";
    Tracenf("[WATER-2] Reflection resources: %ux%u (quality=%s)", width, height, qualityName);
    return true;
}

bool TerrainRenderer::CreatePipeline(ixrhi::IXRHIDevice& rhi)
{
    if (!m_assets || !m_bindLayout)
        return false;

    auto vs = LoadShader(rhi, *m_assets, "assets/shaders/terrain_vs.spv",
        ixrhi::IXRHIShaderStage::Vertex, "VSMain");
    auto ps = LoadShader(rhi, *m_assets, "assets/shaders/terrain_ps.spv",
        ixrhi::IXRHIShaderStage::Fragment, "PSMain");
    if (!vs || !ps)
        return false;

    // Parity with the pre-migration state: triangle list, fill, cull-none,
    // depth test+write LEQUAL, standard alpha blend, push-constant
    // layerParams (vertex+fragment, 16 bytes), dynamic viewport/scissor.
    ixrhi::IXRHIGraphicsPipelineDesc desc;
    desc.vertexShader = vs;
    desc.fragmentShader = ps;
    desc.bindGroupLayouts = {m_bindLayout.get()};
    desc.pushRanges = {{ixrhi::IXRHIShaderStage::Vertex | ixrhi::IXRHIShaderStage::Fragment, 0, sizeof(float) * 4u}};
    desc.vertexBindings = {{0, sizeof(Vertex)}};
    desc.vertexAttributes = {
        {0, 0, ixrhi::IXRHIFormat::R32G32B32Float, offsetof(Vertex, position)},
        {1, 0, ixrhi::IXRHIFormat::R32G32Float, offsetof(Vertex, texUv)},
        {2, 0, ixrhi::IXRHIFormat::R32G32Float, offsetof(Vertex, maskUv)},
    };
    desc.topology = ixrhi::IXRHIPrimitiveTopology::TriangleList;
    desc.cullMode = ixrhi::IXRHICullMode::None;
    desc.frontFace = ixrhi::IXRHIFrontFace::Clockwise;
    desc.depthTestEnable = true;
    desc.depthWriteEnable = true;
    desc.depthCompareOp = ixrhi::IXRHICompareOp::LessOrEqual;
    desc.blendAttachments = {{true,
        ixrhi::IXRHIBlendFactor::SrcAlpha,
        ixrhi::IXRHIBlendFactor::OneMinusSrcAlpha,
        ixrhi::IXRHIBlendOp::Add,
        ixrhi::IXRHIBlendFactor::One,
        ixrhi::IXRHIBlendFactor::OneMinusSrcAlpha,
        ixrhi::IXRHIBlendOp::Add}};
    desc.sampleCount = 1;
    desc.targetRenderPass = m_targetPass;
    desc.debugName = "Terrain:Main";
    m_pipeline = rhi.CreateGraphicsPipeline(desc);
    return m_pipeline != nullptr;
}

bool TerrainRenderer::CreateWaterReflectionPipeline(ixrhi::IXRHIDevice& rhi)
{
    if (!m_assets || !m_bindLayout || !m_waterReflection.target)
        return false;

    DestroyWaterReflectionPipeline();

    auto vs = LoadShader(rhi, *m_assets, "assets/shaders/terrain_vs.spv",
        ixrhi::IXRHIShaderStage::Vertex, "VSMain");
    auto ps = LoadShader(rhi, *m_assets, "assets/shaders/terrain_ps.spv",
        ixrhi::IXRHIShaderStage::Fragment, "PSMain");
    if (!vs || !ps)
        return false;

    // Same terrain shaders as the main pipeline with blending off. The reflection draws the scene
    // mirrored across the water plane (ComputeMirrorCamera), which flips its winding: both faces
    // are drawn, depth sorts them.
    ixrhi::IXRHIGraphicsPipelineDesc desc;
    desc.vertexShader = vs;
    desc.fragmentShader = ps;
    desc.bindGroupLayouts = {m_bindLayout.get()};
    desc.pushRanges = {{ixrhi::IXRHIShaderStage::Vertex | ixrhi::IXRHIShaderStage::Fragment, 0, sizeof(float) * 4u}};
    desc.vertexBindings = {{0, sizeof(Vertex)}};
    desc.vertexAttributes = {
        {0, 0, ixrhi::IXRHIFormat::R32G32B32Float, offsetof(Vertex, position)},
        {1, 0, ixrhi::IXRHIFormat::R32G32Float, offsetof(Vertex, texUv)},
        {2, 0, ixrhi::IXRHIFormat::R32G32Float, offsetof(Vertex, maskUv)},
    };
    desc.topology = ixrhi::IXRHIPrimitiveTopology::TriangleList;
    desc.cullMode = ixrhi::IXRHICullMode::None;
    desc.frontFace = ixrhi::IXRHIFrontFace::Clockwise;
    desc.depthTestEnable = true;
    desc.depthWriteEnable = true;
    desc.depthCompareOp = ixrhi::IXRHICompareOp::LessOrEqual;
    desc.blendAttachments = {{false,
        ixrhi::IXRHIBlendFactor::One,
        ixrhi::IXRHIBlendFactor::Zero,
        ixrhi::IXRHIBlendOp::Add,
        ixrhi::IXRHIBlendFactor::One,
        ixrhi::IXRHIBlendFactor::Zero,
        ixrhi::IXRHIBlendOp::Add}};
    desc.sampleCount = 1;
    desc.targetRenderPass = m_waterReflection.target->GetPass();
    desc.debugName = "Terrain:Reflection";
    m_reflectionPipeline = rhi.CreateGraphicsPipeline(desc);
    return m_reflectionPipeline != nullptr;
}

bool TerrainRenderer::CreateWaterPipeline(ixrhi::IXRHIDevice& rhi)
{
    if (!m_assets || !m_waterBindLayout)
        return false;

    DestroyWaterPipeline();

    auto vs = LoadShader(rhi, *m_assets, "assets/shaders/water_vs.spv",
        ixrhi::IXRHIShaderStage::Vertex, "VSMain");
    auto ps = LoadShader(rhi, *m_assets, "assets/shaders/water_ps.spv",
        ixrhi::IXRHIShaderStage::Fragment, "PSMain");
    if (!vs || !ps)
        return false;

    // Parity: triangle list, fill, cull-none, depth test LEQUAL without
    // write (translucent water over terrain), standard alpha blend, no push
    // constants.
    ixrhi::IXRHIGraphicsPipelineDesc desc;
    desc.vertexShader = vs;
    desc.fragmentShader = ps;
    desc.bindGroupLayouts = {m_waterBindLayout.get()};
    desc.vertexBindings = {{0, sizeof(WaterVertex)}};
    desc.vertexAttributes = {
        {0, 0, ixrhi::IXRHIFormat::R32G32B32Float, offsetof(WaterVertex, position)},
        {1, 0, ixrhi::IXRHIFormat::R32G32Float, offsetof(WaterVertex, uv)},
        {2, 0, ixrhi::IXRHIFormat::R32Float, offsetof(WaterVertex, edgeAlpha)},
    };
    desc.topology = ixrhi::IXRHIPrimitiveTopology::TriangleList;
    desc.cullMode = ixrhi::IXRHICullMode::None;
    desc.frontFace = ixrhi::IXRHIFrontFace::Clockwise;
    desc.depthTestEnable = true;
    desc.depthWriteEnable = false;
    desc.depthCompareOp = ixrhi::IXRHICompareOp::LessOrEqual;
    desc.blendAttachments = {{true,
        ixrhi::IXRHIBlendFactor::SrcAlpha,
        ixrhi::IXRHIBlendFactor::OneMinusSrcAlpha,
        ixrhi::IXRHIBlendOp::Add,
        ixrhi::IXRHIBlendFactor::One,
        ixrhi::IXRHIBlendFactor::OneMinusSrcAlpha,
        ixrhi::IXRHIBlendOp::Add}};
    desc.sampleCount = 1;
    desc.targetRenderPass = m_targetPass;
    desc.debugName = "Terrain:Water";
    m_waterPipeline = rhi.CreateGraphicsPipeline(desc);
    return m_waterPipeline != nullptr;
}

bool TerrainRenderer::CreateShadowPipeline(ixrhi::IXRHIDevice& rhi)
{
    if (!m_assets || !m_shadowTargets[0])
        return false;

    auto vs = LoadShader(rhi, *m_assets, "assets/shaders/shadow_depth_vs.spv",
        ixrhi::IXRHIShaderStage::Vertex, "VSMain");
    if (!vs)
        return false;

    // Depth-only pipeline (no fragment shader, no color attachments):
    // position-only vertex input, front cull, fixed depth bias, push-constant
    // light view-projection (vertex stage, 64 bytes).
    ixrhi::IXRHIGraphicsPipelineDesc desc;
    desc.vertexShader = vs;
    desc.fragmentShader = nullptr;
    desc.pushRanges = {{ixrhi::IXRHIShaderStage::Vertex, 0, sizeof(WorldMat4)}};
    desc.vertexBindings = {{0, sizeof(Vertex)}};
    desc.vertexAttributes = {
        {0, 0, ixrhi::IXRHIFormat::R32G32B32Float, offsetof(Vertex, position)},
    };
    desc.topology = ixrhi::IXRHIPrimitiveTopology::TriangleList;
    desc.cullMode = ixrhi::IXRHICullMode::Front;
    desc.frontFace = ixrhi::IXRHIFrontFace::Clockwise;
    desc.depthTestEnable = true;
    desc.depthWriteEnable = true;
    desc.depthCompareOp = ixrhi::IXRHICompareOp::LessOrEqual;
    desc.depthBias.enable = true;
    desc.depthBias.constantFactor = 1.25f;
    desc.depthBias.slopeFactor = 1.75f;
    desc.sampleCount = 1;
    desc.targetRenderPass = m_shadowTargets[0]->GetPass();
    desc.debugName = "Terrain:Shadow";
    m_shadowPipeline = rhi.CreateGraphicsPipeline(desc);
    return m_shadowPipeline != nullptr;
}

void TerrainRenderer::DestroyPipeline()
{
    DestroyWaterPipeline();
    DestroyWaterReflectionPipeline();
    m_pipeline.reset();
}

void TerrainRenderer::DestroyWaterPipeline()
{
    m_waterPipeline.reset();
}

void TerrainRenderer::DestroyWaterReflectionPipeline()
{
    m_reflectionPipeline.reset();
}

void TerrainRenderer::DestroyWaterReflectionResources()
{
    m_waterReflection = {};
}

void TerrainRenderer::DestroyWaterResources()
{
    DestroyWaterPipeline();
    DestroyWaterReflectionPipeline();
    DestroyWaterReflectionResources();
    m_waterBindGroup.reset();
    m_waterBindLayout.reset();
    DestroyWaterBodyResources();
    m_waterNormalSmall = {};
    m_waterNormalLarge = {};
}

void TerrainRenderer::DestroyWaterBodyResources()
{
    for (WaterBodyGpu& waterBody : m_waterBodies)
        DestroyWaterBodyResources(waterBody);
    m_waterBodies.clear();
}

void TerrainRenderer::DestroyWaterBodyResources(WaterBodyGpu& waterBody)
{
    waterBody.vertexBuffer.reset();
    waterBody.indexBuffer.reset();
    for (auto& buffer : waterBody.uniformBuffers)
        buffer.reset();
    for (auto& buffer : waterBody.uniformBuffersSecondary)
        buffer.reset();
    waterBody.indexCount = 0;
}

void TerrainRenderer::DestroyShadowPipeline()
{
    m_shadowPipeline.reset();
}

void TerrainRenderer::DestroyShadowResources()
{
    DestroyShadowPipeline();
    for (auto& target : m_shadowTargets)
        target.reset();
    m_shadowTexture.reset();
    m_shadowSampler.reset();
}

void TerrainRenderer::DestroyTerrainLayers()
{
    m_layers.clear();
}

void TerrainRenderer::UpdateUniform(uint32_t frameIndex, const WorldCamera& camera, bool reflectionPass, uint32_t viewIndex)
{
    const std::array<std::shared_ptr<ixrhi::IXRHIBuffer>, kFramesInFlight>& targetBuffers =
        viewIndex == kReflectionUniformView ? m_uniformBuffersReflection
        : viewIndex == 0                    ? m_uniformBuffers
                                            : m_uniformBuffersSecondary;
    if (frameIndex >= kFramesInFlight || !targetBuffers[frameIndex])
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
        uniform.materialPbr[i][3] = xm::DegreesToRadians(m_paletteSlots[i].uvRotationDegrees);
    }
    uniform.terrainMaterialParams[0] = m_sceneTerrain.triplanarEnabled ? 1.0f : 0.0f;
    uniform.terrainMaterialParams[1] = std::clamp(m_sceneTerrain.triplanarSharpness, 1.0f, 16.0f);
    uniform.terrainMaterialParams[2] = std::clamp(m_sceneTerrain.triplanarSlopeThreshold, 0.0f, 1.0f);
    uniform.terrainMaterialParams[3] = std::clamp(m_sceneTerrain.triplanarSlopeTransition, 0.001f, 1.0f);
    uniform.activeLayerCount = static_cast<std::int32_t>(
        ActiveSplatLayerSpan());
    uniform.cameraPos[0] = camera.eye.x;
    uniform.cameraPos[1] = camera.eye.y;
    uniform.cameraPos[2] = camera.eye.z;
    uniform.cameraPos[3] = 1.0f;
    const DirectionalLight& directional = m_lightingState.directional;
    const AmbientLight& ambient = m_lightingState.ambient;
    const float azimuthRadians = xm::DegreesToRadians(std::clamp(directional.azimuthDegrees, 0.0f, 360.0f));
    const float elevationRadians = xm::DegreesToRadians(std::clamp(directional.elevationDegrees, 0.0f, 90.0f));
    const WorldVec3 sunDir = WorldDirectionFromAzimuthElevation(azimuthRadians, elevationRadians);
    const float sunEnabled = directional.enabled ? 1.0f : 0.0f;
    const float sunIntensity = std::max(0.0f, directional.intensity) * sunEnabled;
    const float ambientIntensity = std::max(0.0f, ambient.intensity);
    uniform.sunDir[0] = sunDir.x;
    uniform.sunDir[1] = sunDir.y;
    uniform.sunDir[2] = sunDir.z;
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
        m_shadowTexture != nullptr) ? 1.0f : 0.0f;
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
    targetBuffers[frameIndex]->Write(0, &uniform, sizeof(uniform));
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
            Tracenf("[TRIPLANAR] sample mode active, layers=%d", uniform.activeLayerCount);
        m_triplanarParamsDirty = false;
    }
#if defined(IXTREEME_DEBUG_LOGS)
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
        TraceDiagf("[TRI-PERF] avgActiveLayers=%.2f samples/fragment planar=%.1f triplanar=%.1f unconditionalPlanar=42 unconditionalTriplanar=122 layers=8 maps=diff,nor,ao,roughness,metallic axes=1|3 splat=2 shadow_pcf=25-50-independent",
            avgActiveLayers,
            gatedPlanarSamples,
            gatedTriplanarSamples);
        TraceDiagf("[TRIPLANAR-OPT] mode=selective slopeThreshold=%.3f transition=%.3f avgAxesPerLayer=%.2f samples/fragment=%.1f fps=%.1f",
            std::clamp(m_sceneTerrain.triplanarSlopeThreshold, 0.0f, 1.0f),
            std::clamp(m_sceneTerrain.triplanarSlopeTransition, 0.001f, 1.0f),
            avgAxesPerLayer,
            selectiveSamples,
            m_latestFps);
        TraceDiagf("[TRI-PERF] triplanar LOD mode=textureGrad-explicit mipUsed=%s forced-0=%s reason=%s",
            terrainMipsUsable ? "yes" : "no",
            terrainMipsUsable ? "no" : "yes",
            terrainMipsUsable ? "terrain-array-textures-have-full-mip-chain-and-branch-safe-gradients" : "one-or-more-terrain-array-textures-have-1-mip");
        TraceDiag("[TRI-PERF] layer sampling=weight-gated threshold=1/255 uvWork=after-weight-check derivativeSafe=textureGrad-explicit");
        TraceDiagf("[TRI-PERF] likely bottleneck class=%s",
            terrainMipsUsable ? "active-layer-count-and-remaining-triplanar-sample-count" : "fragment-texture-bandwidth-plus-mip0-cache-pressure");
        m_triPerfStaticLogged = true;
    }
#endif
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
    const float azimuthRadians = xm::DegreesToRadians(std::clamp(directional.azimuthDegrees, 0.0f, 360.0f));
    const float elevationRadians = xm::DegreesToRadians(std::clamp(directional.elevationDegrees, 0.0f, 90.0f));
    const WorldVec3 sunDir = WorldDirectionFromAzimuthElevation(azimuthRadians, elevationRadians);
    const float sunEnabled = directional.enabled ? 1.0f : 0.0f;
    const float sunIntensity = std::max(0.0f, directional.intensity) * sunEnabled;
    const float ambientIntensity = std::max(0.0f, ambient.intensity);
    uniform.sunDir[0] = sunDir.x;
    uniform.sunDir[1] = sunDir.y;
    uniform.sunDir[2] = sunDir.z;
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
    uniform.reflectionParams[0] = (reflectionTarget && water.reflectionEnabled && m_waterReflection.color) ? 1.0f : 0.0f;
    uniform.reflectionParams[1] = std::clamp(water.reflectionDistortionStrength, 0.0f, 0.2f);
    uniform.reflectionParams[2] = m_waterReflection.width > 0 ? static_cast<float>(m_waterReflection.width) : 1.0f;
    uniform.reflectionParams[3] = m_waterReflection.height > 0 ? static_cast<float>(m_waterReflection.height) : 1.0f;
    uniform.refractionParams[0] = (water.refractionEnabled && m_waterSceneColor && m_waterSceneDepth) ? 1.0f : 0.0f;
    uniform.refractionParams[1] = std::clamp(water.refractionStrength, 0.0f, 0.1f);
    uniform.refractionParams[2] = std::clamp(water.refractionDepthStrength, 0.0f, 2.0f);
    uniform.refractionParams[3] = m_waterSceneWidth > 0 ? static_cast<float>(m_waterSceneWidth) : 1.0f;
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
    uniform.depthParams[3] = m_waterSceneHeight > 0 ? static_cast<float>(m_waterSceneHeight) : 1.0f;
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

void TerrainRenderer::UploadWaterUniform(const std::shared_ptr<ixrhi::IXRHIBuffer>& buffer, const WaterUniformBlock& uniform)
{
    if (!buffer)
        return;
    buffer->Write(0, &uniform, sizeof(uniform));
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
                                             bool reflectionTarget,
                                             uint32_t viewIndex)
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
        uniform.textureParams[0] = textures->normalA.image ? 1.0f : 0.0f;
        uniform.textureParams[1] = textures->normalB.image ? 1.0f : 0.0f;
        uniform.textureParams[2] = textures->diffuse.image ? 1.0f : 0.0f;
    }
    if (material)
    {
        uniform.textureParams[3] = std::clamp(material->normalTiling, 0.001f, 100.0f);
        uniform.textureScroll[0] = material->scrollSpeedA[0];
        uniform.textureScroll[1] = material->scrollSpeedA[1];
        uniform.textureScroll[2] = material->scrollSpeedB[0];
        uniform.textureScroll[3] = material->scrollSpeedB[1];
        const bool hasCustomScroll = xm::Abs(uniform.textureScroll[0]) > 0.00001f ||
            xm::Abs(uniform.textureScroll[1]) > 0.00001f ||
            xm::Abs(uniform.textureScroll[2]) > 0.00001f ||
            xm::Abs(uniform.textureScroll[3]) > 0.00001f;
        if (!hasCustomScroll)
        {
            uniform.textureScroll[0] = 0.03f;
            uniform.textureScroll[1] = 0.014f;
            uniform.textureScroll[2] = -0.015f;
            uniform.textureScroll[3] = 0.02f;
        }
    }
    UploadWaterUniform(
        (viewIndex == 0) ? waterBody.uniformBuffers[frameIndex] : waterBody.uniformBuffersSecondary[frameIndex],
        uniform);
}
