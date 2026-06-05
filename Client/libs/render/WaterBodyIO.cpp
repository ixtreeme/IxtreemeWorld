#include "WaterBodyIO.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace client::render {
namespace {

constexpr std::uint32_t kMagic = 0x4257584d; // "MXWB", little-endian.
constexpr std::uint32_t kMaxWaterBodies = 64;
constexpr std::uint32_t kMaxMaskPixels = 1024u * 1024u;

void SetError(std::string* error, const std::string& message)
{
    if (error)
        *error = message;
}

void WriteU8(std::vector<std::uint8_t>& out, std::uint8_t value)
{
    out.push_back(value);
}

void WriteU32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xffu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xffu));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xffu));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xffu));
}

void WriteF32(std::vector<std::uint8_t>& out, float value)
{
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(float));
}

void WriteString(std::vector<std::uint8_t>& out, const std::string& value)
{
    const std::uint32_t length = static_cast<std::uint32_t>(std::min<std::size_t>(value.size(), 4096u));
    WriteU32(out, length);
    out.insert(out.end(), value.begin(), value.begin() + length);
}

bool ReadU8(const std::vector<std::uint8_t>& bytes, std::size_t& offset, std::uint8_t& value)
{
    if (offset + 1 > bytes.size())
        return false;
    value = bytes[offset++];
    return true;
}

bool ReadU32(const std::vector<std::uint8_t>& bytes, std::size_t& offset, std::uint32_t& value)
{
    if (offset + sizeof(std::uint32_t) > bytes.size())
        return false;
    value = static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
        (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
        (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
    offset += sizeof(std::uint32_t);
    return true;
}

bool ReadF32(const std::vector<std::uint8_t>& bytes, std::size_t& offset, float& value)
{
    if (offset + sizeof(float) > bytes.size())
        return false;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    offset += sizeof(float);
    return true;
}

bool ReadString(const std::vector<std::uint8_t>& bytes, std::size_t& offset, std::string& value)
{
    std::uint32_t length = 0;
    if (!ReadU32(bytes, offset, length) || length > 4096u || offset + length > bytes.size())
        return false;
    value.assign(reinterpret_cast<const char*>(bytes.data() + offset), length);
    offset += length;
    return true;
}

void WriteConfig(std::vector<std::uint8_t>& out, const WaterConfig& config)
{
    WriteU8(out, config.enabled ? 1u : 0u);
    WriteF32(out, config.waterLevelY);
    for (float value : config.baseColor) WriteF32(out, value);
    WriteF32(out, config.waveScaleSmall);
    WriteF32(out, config.waveScaleLarge);
    WriteF32(out, config.waveSpeedSmall);
    WriteF32(out, config.waveSpeedLarge);
    WriteF32(out, config.normalStrength);
    WriteF32(out, config.fresnelPower);
    WriteF32(out, config.fresnelMin);
    for (float value : config.reflectionColor) WriteF32(out, value);
    WriteU8(out, config.reflectionEnabled ? 1u : 0u);
    WriteU32(out, static_cast<std::uint32_t>(config.reflectionQuality));
    WriteF32(out, config.reflectionDistortionStrength);
    WriteU8(out, config.refractionEnabled ? 1u : 0u);
    for (float value : config.shallowColor) WriteF32(out, value);
    for (float value : config.deepColor) WriteF32(out, value);
    WriteF32(out, config.depthColorMin);
    WriteF32(out, config.depthColorMax);
    WriteF32(out, config.depthFadeDistance);
    WriteF32(out, config.refractionStrength);
    WriteF32(out, config.refractionDepthStrength);
    WriteU8(out, config.foamEnabled ? 1u : 0u);
    WriteF32(out, config.foamDistance);
    WriteF32(out, config.foamSoftness);
    WriteF32(out, config.foamIntensity);
    WriteF32(out, config.foamScrollSpeed);
    WriteF32(out, config.foamScale);
    WriteF32(out, config.foamTerrainThickness);
    WriteU32(out, static_cast<std::uint32_t>(config.causticMode));
    WriteF32(out, config.causticIntensity);
    WriteF32(out, config.causticScale);
    WriteF32(out, config.causticSpeed);
    WriteF32(out, config.causticMaxDepth);
}

bool ReadConfig(const std::vector<std::uint8_t>& bytes, std::size_t& offset, WaterConfig& config)
{
    std::uint8_t flag = 0;
    std::uint32_t enumValue = 0;
    if (!ReadU8(bytes, offset, flag)) return false;
    config.enabled = flag != 0;
    if (!ReadF32(bytes, offset, config.waterLevelY)) return false;
    for (float& value : config.baseColor) if (!ReadF32(bytes, offset, value)) return false;
    if (!ReadF32(bytes, offset, config.waveScaleSmall)) return false;
    if (!ReadF32(bytes, offset, config.waveScaleLarge)) return false;
    if (!ReadF32(bytes, offset, config.waveSpeedSmall)) return false;
    if (!ReadF32(bytes, offset, config.waveSpeedLarge)) return false;
    if (!ReadF32(bytes, offset, config.normalStrength)) return false;
    if (!ReadF32(bytes, offset, config.fresnelPower)) return false;
    if (!ReadF32(bytes, offset, config.fresnelMin)) return false;
    for (float& value : config.reflectionColor) if (!ReadF32(bytes, offset, value)) return false;
    if (!ReadU8(bytes, offset, flag)) return false;
    config.reflectionEnabled = flag != 0;
    if (!ReadU32(bytes, offset, enumValue)) return false;
    config.reflectionQuality = enumValue <= 2u
        ? static_cast<WaterConfig::ReflectionQuality>(enumValue)
        : WaterConfig::ReflectionQuality::Half;
    if (!ReadF32(bytes, offset, config.reflectionDistortionStrength)) return false;
    if (!ReadU8(bytes, offset, flag)) return false;
    config.refractionEnabled = flag != 0;
    for (float& value : config.shallowColor) if (!ReadF32(bytes, offset, value)) return false;
    for (float& value : config.deepColor) if (!ReadF32(bytes, offset, value)) return false;
    if (!ReadF32(bytes, offset, config.depthColorMin)) return false;
    if (!ReadF32(bytes, offset, config.depthColorMax)) return false;
    if (!ReadF32(bytes, offset, config.depthFadeDistance)) return false;
    if (!ReadF32(bytes, offset, config.refractionStrength)) return false;
    if (!ReadF32(bytes, offset, config.refractionDepthStrength)) return false;
    if (!ReadU8(bytes, offset, flag)) return false;
    config.foamEnabled = flag != 0;
    if (!ReadF32(bytes, offset, config.foamDistance)) return false;
    if (!ReadF32(bytes, offset, config.foamSoftness)) return false;
    if (!ReadF32(bytes, offset, config.foamIntensity)) return false;
    if (!ReadF32(bytes, offset, config.foamScrollSpeed)) return false;
    if (!ReadF32(bytes, offset, config.foamScale)) return false;
    if (!ReadF32(bytes, offset, config.foamTerrainThickness)) return false;
    if (!ReadU32(bytes, offset, enumValue)) return false;
    config.causticMode = enumValue <= 2u
        ? static_cast<WaterConfig::CausticMode>(enumValue)
        : WaterConfig::CausticMode::AnimatedTexture;
    if (!ReadF32(bytes, offset, config.causticIntensity)) return false;
    if (!ReadF32(bytes, offset, config.causticScale)) return false;
    if (!ReadF32(bytes, offset, config.causticSpeed)) return false;
    if (!ReadF32(bytes, offset, config.causticMaxDepth)) return false;
    return true;
}

WaterBody MakeRectBody(std::uint32_t id,
                       const char* name,
                       float minX,
                       float minZ,
                       float maxX,
                       float maxZ,
                       float levelY,
                       std::uint32_t width,
                       std::uint32_t height,
                       float r,
                       float g,
                       float b)
{
    WaterBody body;
    body.id = id;
    body.name = name;
    body.bboxMin[0] = minX;
    body.bboxMin[1] = minZ;
    body.bboxMax[0] = maxX;
    body.bboxMax[1] = maxZ;
    body.waterLevelY = levelY;
    body.maskWidth = width;
    body.maskHeight = height;
    body.shapeMask.assign(static_cast<std::size_t>(width) * height, 255u);
    body.config.waterLevelY = levelY;
    body.config.baseColor[0] = r;
    body.config.baseColor[1] = g;
    body.config.baseColor[2] = b;
    body.config.foamDistance = 0.12f;
    body.config.foamSoftness = 0.05f;
    return body;
}

} // namespace

bool LoadWaterBodiesBinary(const std::vector<std::uint8_t>& bytes,
                           std::vector<WaterBody>& outBodies,
                           std::string* error)
{
    outBodies.clear();
    std::size_t offset = 0;
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t count = 0;
    if (!ReadU32(bytes, offset, magic) || !ReadU32(bytes, offset, version) || !ReadU32(bytes, offset, count))
    {
        SetError(error, "water body file is truncated");
        return false;
    }
    if (magic != kMagic)
    {
        SetError(error, "water body file has invalid magic");
        return false;
    }
    if (version != kWaterBodiesFormatVersion)
    {
        SetError(error, "unsupported water body file version");
        return false;
    }
    if (count > kMaxWaterBodies)
    {
        SetError(error, "water body count exceeds renderer limit");
        return false;
    }

    outBodies.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i)
    {
        WaterBody body;
        if (!ReadU32(bytes, offset, body.id) ||
            !ReadString(bytes, offset, body.name) ||
            !ReadF32(bytes, offset, body.bboxMin[0]) ||
            !ReadF32(bytes, offset, body.bboxMin[1]) ||
            !ReadF32(bytes, offset, body.bboxMax[0]) ||
            !ReadF32(bytes, offset, body.bboxMax[1]) ||
            !ReadF32(bytes, offset, body.waterLevelY) ||
            !ReadU32(bytes, offset, body.maskWidth) ||
            !ReadU32(bytes, offset, body.maskHeight))
        {
            SetError(error, "water body entry is truncated");
            return false;
        }
        const std::uint64_t pixelCount =
            static_cast<std::uint64_t>(body.maskWidth) * static_cast<std::uint64_t>(body.maskHeight);
        if (body.maskWidth == 0 || body.maskHeight == 0 || pixelCount > kMaxMaskPixels ||
            offset + static_cast<std::size_t>(pixelCount) > bytes.size())
        {
            SetError(error, "water body mask is invalid");
            return false;
        }
        body.shapeMask.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                              bytes.begin() + static_cast<std::ptrdiff_t>(offset + pixelCount));
        offset += static_cast<std::size_t>(pixelCount);
        if (!ReadConfig(bytes, offset, body.config))
        {
            SetError(error, "water body config is truncated");
            return false;
        }
        body.config.waterLevelY = body.waterLevelY;
        outBodies.push_back(std::move(body));
    }
    return true;
}

bool SaveWaterBodiesBinary(const std::filesystem::path& path,
                           const std::vector<WaterBody>& bodies,
                           std::string* error)
{
    if (bodies.size() > kMaxWaterBodies)
    {
        SetError(error, "too many water bodies");
        return false;
    }

    std::vector<std::uint8_t> bytes;
    WriteU32(bytes, kMagic);
    WriteU32(bytes, kWaterBodiesFormatVersion);
    WriteU32(bytes, static_cast<std::uint32_t>(bodies.size()));
    for (const WaterBody& body : bodies)
    {
        const std::uint64_t pixelCount =
            static_cast<std::uint64_t>(body.maskWidth) * static_cast<std::uint64_t>(body.maskHeight);
        if (body.maskWidth == 0 || body.maskHeight == 0 || pixelCount > kMaxMaskPixels ||
            body.shapeMask.size() != static_cast<std::size_t>(pixelCount))
        {
            SetError(error, "invalid water body mask");
            return false;
        }

        WaterConfig config = body.config;
        config.waterLevelY = body.waterLevelY;
        WriteU32(bytes, body.id);
        WriteString(bytes, body.name);
        WriteF32(bytes, body.bboxMin[0]);
        WriteF32(bytes, body.bboxMin[1]);
        WriteF32(bytes, body.bboxMax[0]);
        WriteF32(bytes, body.bboxMax[1]);
        WriteF32(bytes, body.waterLevelY);
        WriteU32(bytes, body.maskWidth);
        WriteU32(bytes, body.maskHeight);
        bytes.insert(bytes.end(), body.shapeMask.begin(), body.shapeMask.end());
        WriteConfig(bytes, config);
    }

    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    if (!out)
    {
        SetError(error, "failed to open water body file for writing");
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out)
    {
        SetError(error, "failed to write water body file");
        return false;
    }
    return true;
}

std::vector<WaterBody> CreateWaterBodyTestSet()
{
    std::vector<WaterBody> bodies;
    bodies.push_back(MakeRectBody(1, "north_pond", 8.0f, 8.0f, 34.0f, 28.0f, 0.4f, 26, 20, 0.10f, 0.35f, 0.55f));
    bodies.push_back(MakeRectBody(2, "south_lake", 42.0f, 18.0f, 76.0f, 54.0f, -0.2f, 34, 36, 0.08f, 0.42f, 0.62f));
    bodies.push_back(MakeRectBody(3, "marsh_pool", 18.0f, 58.0f, 42.0f, 80.0f, 0.1f, 24, 22, 0.18f, 0.44f, 0.40f));
    if (!bodies.empty())
    {
        WaterBody& oval = bodies.back();
        const float cx = static_cast<float>(oval.maskWidth) * 0.5f;
        const float cy = static_cast<float>(oval.maskHeight) * 0.5f;
        const float rx = std::max(1.0f, cx - 1.0f);
        const float ry = std::max(1.0f, cy - 1.0f);
        for (std::uint32_t y = 0; y < oval.maskHeight; ++y)
        {
            for (std::uint32_t x = 0; x < oval.maskWidth; ++x)
            {
                const float dx = (static_cast<float>(x) + 0.5f - cx) / rx;
                const float dy = (static_cast<float>(y) + 0.5f - cy) / ry;
                oval.shapeMask[static_cast<std::size_t>(y) * oval.maskWidth + x] =
                    (dx * dx + dy * dy <= 1.0f) ? 255u : 0u;
            }
        }
    }
    return bodies;
}

} // namespace client::render
