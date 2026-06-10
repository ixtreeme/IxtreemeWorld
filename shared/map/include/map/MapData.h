#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mx::map {

using AssetReadFn = std::function<std::optional<std::vector<std::uint8_t>>(std::string_view path)>;

struct Manifest {
    std::uint32_t format_version = 0;
    std::string world_id;
    std::string world_name;
    std::uint32_t world_size_cells = 0;
    float cell_size_meters = 1.0f;
    std::uint32_t chunk_size_cells = 0;
    std::uint32_t zone_grid_x = 0;
    std::uint32_t zone_grid_y = 0;
    std::uint32_t zone_size_cells = 0;
    std::vector<std::string> texture_palette_paths;
    std::vector<float> texture_palette_tiling_x;
    std::vector<float> texture_palette_tiling_y;
    std::vector<float> texture_palette_normal_strength;
    std::vector<float> texture_palette_roughness_strength;
    std::vector<float> texture_palette_tint_r;
    std::vector<float> texture_palette_tint_g;
    std::vector<float> texture_palette_tint_b;
    std::vector<float> texture_palette_metallic_strength;
    std::vector<float> texture_palette_ao_strength;
    std::vector<float> texture_palette_uv_offset_x;
    std::vector<float> texture_palette_uv_offset_y;
    std::vector<float> texture_palette_uv_rotation_degrees;
};

struct HeightField {
    static constexpr std::uint16_t kAttributeBlocked = 0x0001;

    Manifest manifest;
    std::uint32_t width_vertices = 0;
    std::uint32_t height_vertices = 0;
    std::uint32_t splat_width = 0;
    std::uint32_t splat_height = 0;
    std::vector<std::int16_t> heights_cm;
    std::vector<std::uint16_t> attributes;
    std::vector<std::uint8_t> splat_a_rgba8;
    std::vector<std::uint8_t> splat_b_rgba8;

    bool IsValid() const noexcept;
    float SampleHeightMeters(float world_x_meters, float world_y_meters) const noexcept;
    float HeightMetersAt(std::uint32_t x, std::uint32_t y) const noexcept;
    bool IsWalkable(float world_x_meters, float world_y_meters) const noexcept;
};

struct Rect {
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;

    bool Contains(float x, float y) const noexcept;
    float CenterX() const noexcept;
    float CenterY() const noexcept;
};

struct Zone {
    std::uint32_t id = 0;
    std::string name;
    Rect bounds;
};

struct SpawnRegion {
    std::uint32_t id = 0;
    std::uint32_t zone_id = 0;
    Rect bounds;
};

struct WarpRegion {
    std::uint32_t id = 0;
    Rect source;
    float target_x = 0.0f;
    float target_y = 0.0f;
};

struct WorldLogic {
    std::vector<Zone> zones;
    std::vector<SpawnRegion> spawns;
    std::vector<WarpRegion> warps;

    const Zone* FindZone(float world_x_meters, float world_y_meters) const noexcept;
    const SpawnRegion* FirstSpawn() const noexcept;
    const WarpRegion* FindWarp(float world_x_meters, float world_y_meters) const noexcept;
};

std::optional<Manifest> LoadManifest(const AssetReadFn& read, std::string_view map_root);
std::optional<HeightField> LoadHeightField(const AssetReadFn& read, std::string_view map_root);
std::optional<WorldLogic> LoadWorldLogic(const AssetReadFn& read, std::string_view map_root);

} // namespace mx::map
