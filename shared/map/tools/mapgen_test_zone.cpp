#include <cmath>
#include <cstdint>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <cstring>

#include <capnp/message.h>
#include <capnp/serialize.h>

#include "schema/map_manifest.capnp.h"

namespace {

constexpr std::uint32_t kWorldSizeCells = 500;
constexpr std::uint32_t kChunkSizeCells = 125;
constexpr std::uint32_t kSplatSize = 128;
constexpr float kCellSizeMeters = 2.0f;
constexpr std::uint16_t kAttributeBlocked = 0x0001;
constexpr std::uint16_t kChunkVersion = 2;

void WriteU8(std::vector<std::uint8_t>& out, std::uint8_t value)
{
    out.push_back(value);
}

void WriteU16(std::vector<std::uint8_t>& out, std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
}

void WriteI16(std::vector<std::uint8_t>& out, std::int16_t value)
{
    WriteU16(out, static_cast<std::uint16_t>(value));
}

void WriteU32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
}

void WriteF32(std::vector<std::uint8_t>& out, float value)
{
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    WriteU32(out, bits);
}

void WriteString(std::vector<std::uint8_t>& out, std::string_view value)
{
    WriteU8(out, static_cast<std::uint8_t>(std::min<std::size_t>(value.size(), 255)));
    out.insert(out.end(), value.begin(), value.begin() + std::min<std::size_t>(value.size(), 255));
}

std::int16_t HeightCm(float world_x, float world_y)
{
    const float hill1 = 900.0f * std::exp(-((world_x - 320.0f) * (world_x - 320.0f) +
                                            (world_y - 360.0f) * (world_y - 360.0f)) /
                                          55000.0f);
    const float hill2 = 520.0f * std::exp(-((world_x - 710.0f) * (world_x - 710.0f) +
                                            (world_y - 620.0f) * (world_y - 620.0f)) /
                                          72000.0f);
    const float waves = 120.0f * std::sin(world_x * 0.025f) * std::cos(world_y * 0.018f);
    const float height = hill1 + hill2 + waves - 80.0f;
    return static_cast<std::int16_t>(std::lround(std::clamp(height, -1200.0f, 2400.0f)));
}

bool IsBlockedCell(std::uint32_t cell_x, std::uint32_t cell_y)
{
    const bool wall = cell_x >= 180 && cell_x <= 320 && cell_y >= 245 && cell_y <= 247;
    const bool block = cell_x >= 360 && cell_x <= 410 && cell_y >= 120 && cell_y <= 170;
    return wall || block;
}

void WriteBinaryFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void WriteManifest(const std::filesystem::path& path)
{
    capnp::MallocMessageBuilder msg;
    auto manifest = msg.initRoot<mx::map::schema::MapManifest>();
    manifest.setFormatVersion(2);
    manifest.setWorldId("test_zone");
    manifest.setWorldName("IxtreemeWorld Test Zone");
    manifest.setWorldSizeCells(kWorldSizeCells);
    manifest.setCellSizeMeters(kCellSizeMeters);
    manifest.setHeightUnit(mx::map::schema::HeightUnit::CENTIMETERS);
    manifest.setChunkSizeCells(kChunkSizeCells);
    auto dims = manifest.initZoneGridDims();
    dims.setX(4);
    dims.setY(4);
    manifest.setZoneSizeCells(kWorldSizeCells);
    auto palette = manifest.initTexturePalette(8);
    const char* paths[8] = {
        "assets/Textures/homokos/sand01.dds",
        "assets/Textures/homokos/beach sand 01.dds",
        "assets/Textures/fu/grass 01.dds",
        "assets/Textures/foldes/field 03.dds",
        "assets/Textures/szikla/stone01.dds",
        "assets/Textures/szikla/n_snow_m_stone01.dds",
        "assets/Textures/fu/grass 03_03.dds",
        "assets/Textures/ho/snow01.dds",
    };
    for (std::uint16_t i = 0; i < 8; ++i) {
        palette[i].setId(i);
        palette[i].setPath(paths[i]);
    }
    manifest.setWorldLogicFile("");
    manifest.setEnvironmentFile("");

    const auto words = capnp::messageToFlatArray(msg);
    const auto bytes = words.asBytes();
    WriteBinaryFile(path, std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
}

float SmoothWeight(float value, float center, float radius)
{
    const float d = std::abs(value - center) / std::max(radius, 0.0001f);
    const float t = std::clamp(1.0f - d, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

void AddSplatPixel(std::vector<std::uint8_t>& splatA,
                   std::vector<std::uint8_t>& splatB,
                   float world_x,
                   float world_y)
{
    const float h = static_cast<float>(HeightCm(world_x, world_y));
    const float hNorm = std::clamp((h + 1200.0f) / 3600.0f, 0.0f, 1.0f);
    const float dhdx = static_cast<float>(HeightCm(world_x + kCellSizeMeters, world_y) -
                                          HeightCm(world_x - kCellSizeMeters, world_y)) *
                       0.01f / (2.0f * kCellSizeMeters);
    const float dhdy = static_cast<float>(HeightCm(world_x, world_y + kCellSizeMeters) -
                                          HeightCm(world_x, world_y - kCellSizeMeters)) *
                       0.01f / (2.0f * kCellSizeMeters);
    const float slope = std::clamp(std::sqrt(dhdx * dhdx + dhdy * dhdy) / 1.6f, 0.0f, 1.0f);
    const float noise = 0.5f + 0.5f * std::sin(world_x * 0.073f + std::cos(world_y * 0.051f) * 3.0f);

    float w[8] = {};
    w[0] = SmoothWeight(hNorm, 0.04f, 0.12f);
    w[1] = SmoothWeight(hNorm, 0.18f, 0.16f);
    w[2] = SmoothWeight(hNorm, 0.42f, 0.24f);
    w[3] = SmoothWeight(hNorm, 0.62f, 0.20f);
    w[4] = SmoothWeight(hNorm, 0.82f, 0.18f);
    w[5] = slope * 1.8f;
    w[6] = (1.0f - slope) * std::max(0.0f, noise - 0.72f) * 0.75f;
    w[7] = SmoothWeight(hNorm, 0.96f, 0.10f);

    float total = 0.0f;
    for (float v : w) {
        total += v;
    }
    if (total <= 0.0001f) {
        w[2] = 1.0f;
        total = 1.0f;
    }
    for (float& v : w) {
        v /= total;
    }

    auto q = [](float value) {
        return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    splatA.push_back(q(w[0]));
    splatA.push_back(q(w[1]));
    splatA.push_back(q(w[2]));
    splatA.push_back(q(w[3]));
    splatB.push_back(q(w[4]));
    splatB.push_back(q(w[5]));
    splatB.push_back(q(w[6]));
    splatB.push_back(q(w[7]));
}

void WriteWorldLogic(const std::filesystem::path& path)
{
    std::vector<std::uint8_t> out;
    WriteU32(out, 0x314c584d); // MXL1
    WriteU32(out, 1);
    WriteU32(out, 3); // zones
    WriteU32(out, 1); // spawns
    WriteU32(out, 2); // warps

    auto rect = [&out](float min_x, float min_y, float max_x, float max_y) {
        WriteF32(out, min_x);
        WriteF32(out, min_y);
        WriteF32(out, max_x);
        WriteF32(out, max_y);
    };

    WriteU32(out, 1);
    WriteString(out, "South Meadow");
    rect(0.0f, 0.0f, 500.0f, 500.0f);
    WriteU32(out, 2);
    WriteString(out, "North Ridge");
    rect(0.0f, 500.0f, 500.0f, 1000.0f);
    WriteU32(out, 3);
    WriteString(out, "East Field");
    rect(500.0f, 0.0f, 1000.0f, 1000.0f);

    WriteU32(out, 1);
    WriteU32(out, 1);
    rect(80.0f, 80.0f, 120.0f, 120.0f);

    WriteU32(out, 1);
    rect(220.0f, 210.0f, 235.0f, 225.0f);
    WriteF32(out, 760.0f);
    WriteF32(out, 760.0f);
    WriteU32(out, 2);
    rect(760.0f, 760.0f, 775.0f, 775.0f);
    WriteF32(out, 110.0f);
    WriteF32(out, 110.0f);

    WriteBinaryFile(path, out);
}

void WriteChunk(const std::filesystem::path& path, std::uint16_t chunk_x, std::uint16_t chunk_y)
{
    constexpr std::uint16_t kSectionCount = 4;
    constexpr std::uint16_t kSectionHeight = 1;
    constexpr std::uint16_t kSectionSplatA = 2;
    constexpr std::uint16_t kSectionAttributes = 3;
    constexpr std::uint16_t kSectionSplatB = 4;
    constexpr std::size_t kHeaderSize = 14;
    constexpr std::size_t kTocSize = kSectionCount * 12;
    constexpr std::size_t kHeightBytes =
        static_cast<std::size_t>(kChunkSizeCells + 1) * (kChunkSizeCells + 1) * sizeof(std::int16_t);
    constexpr std::size_t kAttributeBytes =
        static_cast<std::size_t>(kChunkSizeCells) * kChunkSizeCells * sizeof(std::uint16_t);
    constexpr std::size_t kSplatBytes = 4u + static_cast<std::size_t>(kSplatSize) * kSplatSize * 4u;
    const auto height_offset = static_cast<std::uint32_t>(kHeaderSize + kTocSize);
    const auto attributes_offset = static_cast<std::uint32_t>(height_offset + kHeightBytes);
    const auto splat_a_offset = static_cast<std::uint32_t>(attributes_offset + kAttributeBytes);
    const auto splat_b_offset = static_cast<std::uint32_t>(splat_a_offset + kSplatBytes);

    std::vector<std::uint8_t> attributes;
    attributes.reserve(kAttributeBytes);
    for (std::uint32_t y = 0; y < kChunkSizeCells; ++y) {
        for (std::uint32_t x = 0; x < kChunkSizeCells; ++x) {
            const auto cell_x = static_cast<std::uint32_t>(chunk_x) * kChunkSizeCells + x;
            const auto cell_y = static_cast<std::uint32_t>(chunk_y) * kChunkSizeCells + y;
            WriteU16(attributes, IsBlockedCell(cell_x, cell_y) ? kAttributeBlocked : 0);
        }
    }

    std::vector<std::uint8_t> out;
    out.reserve(kHeaderSize + kTocSize + kHeightBytes + kAttributeBytes + kSplatBytes * 2u);

    WriteU32(out, 0x3143584d); // MXC1
    WriteU16(out, kChunkVersion);
    WriteU16(out, chunk_x);
    WriteU16(out, chunk_y);
    WriteU16(out, static_cast<std::uint16_t>(kChunkSizeCells));
    WriteU16(out, kSectionCount);

    WriteU16(out, kSectionHeight);
    WriteU8(out, 1); // int16
    WriteU8(out, 0);
    WriteU32(out, height_offset);
    WriteU32(out, static_cast<std::uint32_t>(kHeightBytes));

    WriteU16(out, kSectionAttributes);
    WriteU8(out, 3); // uint16 bitfield
    WriteU8(out, 0);
    WriteU32(out, attributes_offset);
    WriteU32(out, static_cast<std::uint32_t>(kAttributeBytes));

    WriteU16(out, kSectionSplatA);
    WriteU8(out, 4); // RGBA8 splat A
    WriteU8(out, 0);
    WriteU32(out, splat_a_offset);
    WriteU32(out, static_cast<std::uint32_t>(kSplatBytes));

    WriteU16(out, kSectionSplatB);
    WriteU8(out, 4); // RGBA8 splat B
    WriteU8(out, 0);
    WriteU32(out, splat_b_offset);
    WriteU32(out, static_cast<std::uint32_t>(kSplatBytes));

    for (std::uint32_t y = 0; y <= kChunkSizeCells; ++y) {
        for (std::uint32_t x = 0; x <= kChunkSizeCells; ++x) {
            const float world_x = static_cast<float>(chunk_x * kChunkSizeCells + x) * kCellSizeMeters;
            const float world_y = static_cast<float>(chunk_y * kChunkSizeCells + y) * kCellSizeMeters;
            WriteI16(out, HeightCm(world_x, world_y));
        }
    }

    out.insert(out.end(), attributes.begin(), attributes.end());

    std::vector<std::uint8_t> splatA;
    std::vector<std::uint8_t> splatB;
    splatA.reserve(kSplatSize * kSplatSize * 4u);
    splatB.reserve(kSplatSize * kSplatSize * 4u);
    for (std::uint32_t y = 0; y < kSplatSize; ++y) {
        for (std::uint32_t x = 0; x < kSplatSize; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(kSplatSize);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(kSplatSize);
            const float world_x = (static_cast<float>(chunk_x * kChunkSizeCells) +
                                   u * static_cast<float>(kChunkSizeCells)) *
                                  kCellSizeMeters;
            const float world_y = (static_cast<float>(chunk_y * kChunkSizeCells) +
                                   v * static_cast<float>(kChunkSizeCells)) *
                                  kCellSizeMeters;
            AddSplatPixel(splatA, splatB, world_x, world_y);
        }
    }
    WriteU16(out, static_cast<std::uint16_t>(kSplatSize));
    WriteU16(out, static_cast<std::uint16_t>(kSplatSize));
    out.insert(out.end(), splatA.begin(), splatA.end());
    WriteU16(out, static_cast<std::uint16_t>(kSplatSize));
    WriteU16(out, static_cast<std::uint16_t>(kSplatSize));
    out.insert(out.end(), splatB.begin(), splatB.end());
    WriteBinaryFile(path, out);
}

} // namespace

int main(int argc, char** argv)
{
    const std::filesystem::path out_dir = argc > 1 ? argv[1] : "Client/assets/Maps/test_zone";
    WriteManifest(out_dir / "map.manifest");
    WriteWorldLogic(out_dir / "worldlogic.dat");
    for (std::uint16_t y = 0; y < 4; ++y) {
        for (std::uint16_t x = 0; x < 4; ++x) {
            WriteChunk(out_dir / "chunks" / ("chunk_" + std::to_string(x) + "_" + std::to_string(y) + ".mxchunk"),
                       x,
                       y);
        }
    }
    std::cout << "Generated test zone at " << out_dir.string() << "\n";
    return 0;
}
