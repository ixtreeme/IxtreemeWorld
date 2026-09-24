#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "map/WorldPackage.h"
#include "map/WorldPackageWriter.h"

// Generates the procedural "test_zone" world package through the shared
// package writer (docs/map-data-format.md). The output directory is
// mandatory and an existing package is never replaced without --force, so
// the checked-in Client/assets/Maps/test_zone cannot be overwritten by
// accident.
//
//   mapgen_test_zone --out <dir> [--format 2|3] [--server-only] [--no-spawns] [--force]
//
//   --format 3     (default) current format: layer list + chunk index + CRC-32.
//   --format 2     legacy layout for the v2-only client renderer/editor.
//   --server-only  v3 without client render data (no splat sections, no palette).
namespace {

constexpr std::uint32_t kWorldSizeCells = 500;
constexpr std::uint32_t kChunkSizeCells = 125;
constexpr std::uint32_t kSplatSize = 128;
constexpr float kCellSizeMeters = 2.0f;

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

float SmoothWeight(float value, float center, float radius)
{
    const float d = std::abs(value - center) / std::max(radius, 0.0001f);
    const float t = std::clamp(1.0f - d, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

void AddSplatPixel(std::vector<std::uint8_t>& splat_a, std::vector<std::uint8_t>& splat_b, float world_x, float world_y)
{
    const float h = static_cast<float>(HeightCm(world_x, world_y));
    const float h_norm = std::clamp((h + 1200.0f) / 3600.0f, 0.0f, 1.0f);
    const float dhdx = static_cast<float>(HeightCm(world_x + kCellSizeMeters, world_y) -
                                          HeightCm(world_x - kCellSizeMeters, world_y)) *
                       0.01f / (2.0f * kCellSizeMeters);
    const float dhdy = static_cast<float>(HeightCm(world_x, world_y + kCellSizeMeters) -
                                          HeightCm(world_x, world_y - kCellSizeMeters)) *
                       0.01f / (2.0f * kCellSizeMeters);
    const float slope = std::clamp(std::sqrt(dhdx * dhdx + dhdy * dhdy) / 1.6f, 0.0f, 1.0f);
    const float noise = 0.5f + 0.5f * std::sin(world_x * 0.073f + std::cos(world_y * 0.051f) * 3.0f);

    float w[8] = {};
    w[0] = SmoothWeight(h_norm, 0.04f, 0.12f);
    w[1] = SmoothWeight(h_norm, 0.18f, 0.16f);
    w[2] = SmoothWeight(h_norm, 0.42f, 0.24f);
    w[3] = SmoothWeight(h_norm, 0.62f, 0.20f);
    w[4] = SmoothWeight(h_norm, 0.82f, 0.18f);
    w[5] = slope * 1.8f;
    w[6] = (1.0f - slope) * std::max(0.0f, noise - 0.72f) * 0.75f;
    w[7] = SmoothWeight(h_norm, 0.96f, 0.10f);

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
    splat_a.push_back(q(w[0]));
    splat_a.push_back(q(w[1]));
    splat_a.push_back(q(w[2]));
    splat_a.push_back(q(w[3]));
    splat_b.push_back(q(w[4]));
    splat_b.push_back(q(w[5]));
    splat_b.push_back(q(w[6]));
    splat_b.push_back(q(w[7]));
}

mx::map::WorldLogic TestZoneLogic()
{
    mx::map::WorldLogic logic;
    logic.zones.push_back({1, "South Meadow", {0.0f, 0.0f, 500.0f, 500.0f}});
    logic.zones.push_back({2, "North Ridge", {0.0f, 500.0f, 500.0f, 1000.0f}});
    logic.zones.push_back({3, "East Field", {500.0f, 0.0f, 1000.0f, 1000.0f}});
    logic.spawns.push_back({1, 1, {80.0f, 80.0f, 120.0f, 120.0f}});
    logic.warps.push_back({1, {220.0f, 210.0f, 235.0f, 225.0f}, 760.0f, 760.0f});
    logic.warps.push_back({2, {760.0f, 760.0f, 775.0f, 775.0f}, 110.0f, 110.0f});
    return logic;
}

constexpr const char* kTestZoneSpawns =
    "# test_zone idle mob spawns (mob_spawns format v1).\n"
    "# mob_type_id=<id> x=<meters> y=<meters> count=<n> radius=<meters>\n"
    "\n"
    "mob_type_id=1 x=95.0 y=95.0 count=3 radius=10.0\n"
    "mob_type_id=2 x=242.0 y=120.0 count=2 radius=8.0\n"
    "mob_type_id=1 x=255.0 y=255.0 count=2 radius=12.0\n"
    "mob_type_id=2 x=380.0 y=252.0 count=2 radius=6.0\n";

int Usage()
{
    std::cerr << "usage: mapgen_test_zone --out <dir> [--format 2|3] [--server-only] [--no-spawns] [--force]\n";
    return 2;
}

} // namespace

int main(int argc, char** argv)
{
    std::filesystem::path out_dir;
    std::uint32_t format = mx::map::kManifestVersionCurrent;
    bool server_only = false;
    bool spawns = true;
    bool force = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--out" && i + 1 < argc) {
            out_dir = argv[++i];
        } else if (arg == "--format" && i + 1 < argc) {
            const std::string value = argv[++i];
            if (value != "2" && value != "3") {
                return Usage();
            }
            format = value == "2" ? 2u : 3u;
        } else if (arg == "--server-only") {
            server_only = true;
        } else if (arg == "--no-spawns") {
            spawns = false;
        } else if (arg == "--force") {
            force = true;
        } else {
            return Usage();
        }
    }
    if (out_dir.empty() || (server_only && format == 2)) {
        return Usage();
    }

    mx::map::PackageWriteSpec spec;
    spec.format_version = format;
    spec.world_id = "test_zone";
    spec.world_name = "IxtreemeWorld Test Zone";
    spec.size_cells = kWorldSizeCells;
    spec.cell_size_m = kCellSizeMeters;
    spec.chunk_size_cells = kChunkSizeCells;
    spec.height_cm = [](std::uint32_t vx, std::uint32_t vy) {
        return HeightCm(static_cast<float>(vx) * kCellSizeMeters, static_cast<float>(vy) * kCellSizeMeters);
    };
    spec.attributes = [](std::uint32_t cx, std::uint32_t cy) -> std::uint16_t {
        return IsBlockedCell(cx, cy) ? 0x0001 : 0;
    };
    if (!server_only) {
        spec.splat_size = kSplatSize;
        spec.splat = [](std::uint32_t chunk_x, std::uint32_t chunk_y, std::vector<std::uint8_t>& a,
                        std::vector<std::uint8_t>& b) {
            a.reserve(kSplatSize * kSplatSize * 4u);
            b.reserve(kSplatSize * kSplatSize * 4u);
            for (std::uint32_t y = 0; y < kSplatSize; ++y) {
                for (std::uint32_t x = 0; x < kSplatSize; ++x) {
                    const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(kSplatSize);
                    const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(kSplatSize);
                    const float world_x =
                        (static_cast<float>(chunk_x * kChunkSizeCells) + u * static_cast<float>(kChunkSizeCells)) *
                        kCellSizeMeters;
                    const float world_y =
                        (static_cast<float>(chunk_y * kChunkSizeCells) + v * static_cast<float>(kChunkSizeCells)) *
                        kCellSizeMeters;
                    AddSplatPixel(a, b, world_x, world_y);
                }
            }
        };
        spec.texture_palette = {
            "assets/Textures/homokos/sand01.dds",
            "assets/Textures/homokos/beach sand 01.dds",
            "assets/Textures/fu/grass 01.dds",
            "assets/Textures/foldes/field 03.dds",
            "assets/Textures/szikla/stone01.dds",
            "assets/Textures/szikla/n_snow_m_stone01.dds",
            "assets/Textures/fu/grass 03_03.dds",
            "assets/Textures/ho/snow01.dds",
        };
    }
    spec.logic = TestZoneLogic();
    if (spawns) {
        spec.mob_spawns = std::string(kTestZoneSpawns);
    }
    spec.overwrite = force;

    const auto result = mx::map::WritePackage(out_dir, spec);
    if (!result.ok) {
        std::cerr << "mapgen_test_zone: " << result.error << "\n";
        return 1;
    }
    std::cout << "Generated test_zone (format " << format << (server_only ? ", server-only" : "") << ", "
              << result.files.size() << " files) at " << out_dir.string() << "\n";
    return 0;
}
