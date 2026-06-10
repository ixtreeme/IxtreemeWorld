#include "map/MapData.h"

#include <algorithm>
#include <cstring>
#include <string>

#include <capnp/serialize.h>
#include <kj/array.h>

#include "schema/map_manifest.capnp.h"

namespace mx::map {
namespace {

constexpr std::uint32_t kChunkMagic = 0x3143584d; // "MXC1", little-endian.
constexpr std::uint16_t kChunkVersion = 2;
constexpr std::uint16_t kSectionHeight = 1;
constexpr std::uint16_t kSectionSplatA = 2;
constexpr std::uint16_t kSectionAttributes = 3;
constexpr std::uint16_t kSectionSplatB = 4;

struct ParsedChunk {
    std::vector<std::int16_t> heights;
    std::vector<std::uint16_t> attributes;
    std::uint32_t splat_width = 0;
    std::uint32_t splat_height = 0;
    std::vector<std::uint8_t> splat_a_rgba8;
    std::vector<std::uint8_t> splat_b_rgba8;
};

std::uint32_t ReadU32LE(const std::uint8_t* p);

std::string JoinAssetPath(std::string_view root, std::string_view leaf)
{
    std::string out(root);
    std::replace(out.begin(), out.end(), '\\', '/');
    while (!out.empty() && out.back() == '/') {
        out.pop_back();
    }
    out.push_back('/');
    out.append(leaf);
    return out;
}

std::uint8_t ReadU8(const std::vector<std::uint8_t>& bytes, std::size_t& offset)
{
    return offset < bytes.size() ? bytes[offset++] : 0;
}

std::uint32_t ReadU32(const std::vector<std::uint8_t>& bytes, std::size_t& offset)
{
    if (offset + sizeof(std::uint32_t) > bytes.size()) {
        offset = bytes.size();
        return 0;
    }
    const auto value = ReadU32LE(bytes.data() + offset);
    offset += sizeof(std::uint32_t);
    return value;
}

float ReadF32(const std::vector<std::uint8_t>& bytes, std::size_t& offset)
{
    if (offset + sizeof(float) > bytes.size()) {
        offset = bytes.size();
        return 0.0f;
    }
    float value = 0.0f;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    offset += sizeof(value);
    return value;
}

std::string ReadString(const std::vector<std::uint8_t>& bytes, std::size_t& offset)
{
    const auto length = ReadU8(bytes, offset);
    if (offset + length > bytes.size()) {
        offset = bytes.size();
        return {};
    }
    std::string value(reinterpret_cast<const char*>(bytes.data() + offset), length);
    offset += length;
    return value;
}

std::uint16_t ReadU16LE(const std::uint8_t* p)
{
    return static_cast<std::uint16_t>(p[0]) | (static_cast<std::uint16_t>(p[1]) << 8);
}

std::uint32_t ReadU32LE(const std::uint8_t* p)
{
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

bool ReadSplatSection(const std::uint8_t* src,
                      std::uint32_t byte_length,
                      std::uint32_t& width,
                      std::uint32_t& height,
                      std::vector<std::uint8_t>& out)
{
    if (byte_length < 4) {
        return false;
    }
    width = ReadU16LE(src);
    height = ReadU16LE(src + 2);
    const std::size_t pixel_bytes = static_cast<std::size_t>(width) * height * 4u;
    if (width == 0 || height == 0 || byte_length != 4u + pixel_bytes) {
        return false;
    }

    out.assign(src + 4, src + 4 + pixel_bytes);
    return true;
}

std::int16_t ReadI16LE(const std::uint8_t* p)
{
    return static_cast<std::int16_t>(ReadU16LE(p));
}

bool ParseChunk(const std::vector<std::uint8_t>& bytes,
                std::uint32_t expected_chunk_x,
                std::uint32_t expected_chunk_y,
                std::uint32_t expected_chunk_cells,
                ParsedChunk& chunk)
{
    constexpr std::size_t kTocEntrySize = 12;
    constexpr std::size_t kActualHeaderSize = 14;
    if (bytes.size() < kActualHeaderSize) {
        return false;
    }

    const auto magic = ReadU32LE(bytes.data());
    const auto version = ReadU16LE(bytes.data() + 4);
    const auto chunk_x = ReadU16LE(bytes.data() + 6);
    const auto chunk_y = ReadU16LE(bytes.data() + 8);
    const auto cells_per_side = ReadU16LE(bytes.data() + 10);
    const auto section_count = ReadU16LE(bytes.data() + 12);
    if (magic != kChunkMagic || version != kChunkVersion ||
        chunk_x != expected_chunk_x || chunk_y != expected_chunk_y ||
        cells_per_side != expected_chunk_cells) {
        return false;
    }

    const std::size_t toc_begin = kActualHeaderSize;
    if (bytes.size() < toc_begin + static_cast<std::size_t>(section_count) * kTocEntrySize) {
        return false;
    }

    bool found_height = false;
    for (std::uint16_t i = 0; i < section_count; ++i) {
        const std::uint8_t* entry = bytes.data() + toc_begin + static_cast<std::size_t>(i) * kTocEntrySize;
        const auto section_type = ReadU16LE(entry);
        const auto byte_offset = ReadU32LE(entry + 4);
        const auto byte_length = ReadU32LE(entry + 8);
        if (byte_offset > bytes.size() ||
            static_cast<std::size_t>(byte_offset) + byte_length > bytes.size()) {
            return false;
        }

        const std::uint8_t* src = bytes.data() + byte_offset;
        if (section_type == kSectionHeight) {
            const std::size_t expected_vertices =
                static_cast<std::size_t>(expected_chunk_cells + 1) * (expected_chunk_cells + 1);
            const std::size_t expected_bytes = expected_vertices * sizeof(std::int16_t);
            if (byte_length != expected_bytes) {
                return false;
            }

            chunk.heights.resize(expected_vertices);
            for (std::size_t h = 0; h < chunk.heights.size(); ++h) {
                chunk.heights[h] = ReadI16LE(src + h * sizeof(std::int16_t));
            }
            found_height = true;
        } else if (section_type == kSectionAttributes) {
            const std::size_t expected_cells =
                static_cast<std::size_t>(expected_chunk_cells) * expected_chunk_cells;
            const std::size_t expected_bytes = expected_cells * sizeof(std::uint16_t);
            if (byte_length != expected_bytes) {
                return false;
            }

            chunk.attributes.resize(expected_cells);
            for (std::size_t a = 0; a < chunk.attributes.size(); ++a) {
                chunk.attributes[a] = ReadU16LE(src + a * sizeof(std::uint16_t));
            }
        } else if (section_type == kSectionSplatA) {
            if (!ReadSplatSection(src, byte_length, chunk.splat_width, chunk.splat_height, chunk.splat_a_rgba8)) {
                return false;
            }
        } else if (section_type == kSectionSplatB) {
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            if (!ReadSplatSection(src, byte_length, width, height, chunk.splat_b_rgba8)) {
                return false;
            }
            if (chunk.splat_width != 0 &&
                (width != chunk.splat_width || height != chunk.splat_height)) {
                return false;
            }
            chunk.splat_width = width;
            chunk.splat_height = height;
        }
    }

    if (found_height && chunk.attributes.empty()) {
        chunk.attributes.assign(static_cast<std::size_t>(expected_chunk_cells) * expected_chunk_cells, 0);
    }
    return found_height && !chunk.splat_a_rgba8.empty() && !chunk.splat_b_rgba8.empty();
}

} // namespace

bool Rect::Contains(float x, float y) const noexcept
{
    return x >= min_x && x <= max_x && y >= min_y && y <= max_y;
}

float Rect::CenterX() const noexcept
{
    return (min_x + max_x) * 0.5f;
}

float Rect::CenterY() const noexcept
{
    return (min_y + max_y) * 0.5f;
}

bool HeightField::IsValid() const noexcept
{
    return width_vertices >= 2 && height_vertices >= 2 &&
           heights_cm.size() == static_cast<std::size_t>(width_vertices) * height_vertices &&
           (attributes.empty() ||
            attributes.size() == static_cast<std::size_t>(manifest.world_size_cells) * manifest.world_size_cells) &&
           (splat_a_rgba8.empty() ||
            splat_a_rgba8.size() == static_cast<std::size_t>(splat_width) * splat_height * 4u) &&
           (splat_b_rgba8.empty() ||
            splat_b_rgba8.size() == static_cast<std::size_t>(splat_width) * splat_height * 4u) &&
           manifest.cell_size_meters > 0.0f;
}

const Zone* WorldLogic::FindZone(float world_x_meters, float world_y_meters) const noexcept
{
    for (const auto& zone : zones) {
        if (zone.bounds.Contains(world_x_meters, world_y_meters)) {
            return &zone;
        }
    }
    return nullptr;
}

const SpawnRegion* WorldLogic::FirstSpawn() const noexcept
{
    return spawns.empty() ? nullptr : &spawns.front();
}

const WarpRegion* WorldLogic::FindWarp(float world_x_meters, float world_y_meters) const noexcept
{
    for (const auto& warp : warps) {
        if (warp.source.Contains(world_x_meters, world_y_meters)) {
            return &warp;
        }
    }
    return nullptr;
}

float HeightField::HeightMetersAt(std::uint32_t x, std::uint32_t y) const noexcept
{
    if (!IsValid() || x >= width_vertices || y >= height_vertices) {
        return 0.0f;
    }
    return static_cast<float>(heights_cm[static_cast<std::size_t>(y) * width_vertices + x]) * 0.01f;
}

float HeightField::SampleHeightMeters(float world_x_meters, float world_y_meters) const noexcept
{
    if (!IsValid()) {
        return 0.0f;
    }

    const float max_x = static_cast<float>(width_vertices - 1) * manifest.cell_size_meters;
    const float max_y = static_cast<float>(height_vertices - 1) * manifest.cell_size_meters;
    world_x_meters = std::clamp(world_x_meters, 0.0f, max_x);
    world_y_meters = std::clamp(world_y_meters, 0.0f, max_y);

    const float gx = world_x_meters / manifest.cell_size_meters;
    const float gy = world_y_meters / manifest.cell_size_meters;
    const auto x0 = std::min(static_cast<std::uint32_t>(gx), width_vertices - 2);
    const auto y0 = std::min(static_cast<std::uint32_t>(gy), height_vertices - 2);
    const auto x1 = x0 + 1;
    const auto y1 = y0 + 1;
    const float tx = gx - static_cast<float>(x0);
    const float ty = gy - static_cast<float>(y0);

    const float h00 = HeightMetersAt(x0, y0);
    const float h10 = HeightMetersAt(x1, y0);
    const float h01 = HeightMetersAt(x0, y1);
    const float h11 = HeightMetersAt(x1, y1);
    const float h0 = h00 + (h10 - h00) * tx;
    const float h1 = h01 + (h11 - h01) * tx;
    return h0 + (h1 - h0) * ty;
}

bool HeightField::IsWalkable(float world_x_meters, float world_y_meters) const noexcept
{
    if (!IsValid() || attributes.empty()) {
        return true;
    }

    const float max_coord = static_cast<float>(manifest.world_size_cells) * manifest.cell_size_meters;
    if (world_x_meters < 0.0f || world_y_meters < 0.0f ||
        world_x_meters > max_coord || world_y_meters > max_coord) {
        return false;
    }

    const auto cell_x = std::min(static_cast<std::uint32_t>(world_x_meters / manifest.cell_size_meters),
                                 manifest.world_size_cells - 1);
    const auto cell_y = std::min(static_cast<std::uint32_t>(world_y_meters / manifest.cell_size_meters),
                                 manifest.world_size_cells - 1);
    const auto attr = attributes[static_cast<std::size_t>(cell_y) * manifest.world_size_cells + cell_x];
    return (attr & kAttributeBlocked) == 0;
}

std::optional<Manifest> LoadManifest(const AssetReadFn& read, std::string_view map_root)
{
    const auto bytes = read(JoinAssetPath(map_root, "map.manifest"));
    if (!bytes || bytes->empty() || bytes->size() % sizeof(capnp::word) != 0) {
        return std::nullopt;
    }

    auto words = kj::ArrayPtr<const capnp::word>(
        reinterpret_cast<const capnp::word*>(bytes->data()),
        bytes->size() / sizeof(capnp::word));
    capnp::FlatArrayMessageReader reader(words);
    const auto root = reader.getRoot<schema::MapManifest>();
    if (root.getFormatVersion() != 2 ||
        root.getHeightUnit() != schema::HeightUnit::CENTIMETERS) {
        return std::nullopt;
    }

    Manifest manifest;
    manifest.format_version = root.getFormatVersion();
    manifest.world_id = root.getWorldId().cStr();
    manifest.world_name = root.getWorldName().cStr();
    manifest.world_size_cells = root.getWorldSizeCells();
    manifest.cell_size_meters = root.getCellSizeMeters();
    manifest.chunk_size_cells = root.getChunkSizeCells();
    manifest.zone_grid_x = root.getZoneGridDims().getX();
    manifest.zone_grid_y = root.getZoneGridDims().getY();
    manifest.zone_size_cells = root.getZoneSizeCells();
    const auto palette = root.getTexturePalette();
    manifest.texture_palette_paths.reserve(palette.size());
    manifest.texture_palette_tiling_x.reserve(palette.size());
    manifest.texture_palette_tiling_y.reserve(palette.size());
    manifest.texture_palette_normal_strength.reserve(palette.size());
    manifest.texture_palette_roughness_strength.reserve(palette.size());
    manifest.texture_palette_tint_r.reserve(palette.size());
    manifest.texture_palette_tint_g.reserve(palette.size());
    manifest.texture_palette_tint_b.reserve(palette.size());
    manifest.texture_palette_metallic_strength.reserve(palette.size());
    manifest.texture_palette_ao_strength.reserve(palette.size());
    manifest.texture_palette_uv_offset_x.reserve(palette.size());
    manifest.texture_palette_uv_offset_y.reserve(palette.size());
    manifest.texture_palette_uv_rotation_degrees.reserve(palette.size());
    for (auto entry : palette) {
        manifest.texture_palette_paths.emplace_back(entry.getPath().cStr());
        manifest.texture_palette_tiling_x.emplace_back(entry.getTilingX());
        manifest.texture_palette_tiling_y.emplace_back(entry.getTilingY());
        manifest.texture_palette_normal_strength.emplace_back(entry.getNormalStrength());
        manifest.texture_palette_roughness_strength.emplace_back(entry.getRoughnessStrength());
        manifest.texture_palette_tint_r.emplace_back(entry.getTintR());
        manifest.texture_palette_tint_g.emplace_back(entry.getTintG());
        manifest.texture_palette_tint_b.emplace_back(entry.getTintB());
        manifest.texture_palette_metallic_strength.emplace_back(entry.getMetallicStrength());
        manifest.texture_palette_ao_strength.emplace_back(entry.getAoStrength());
        manifest.texture_palette_uv_offset_x.emplace_back(entry.getUvOffsetX());
        manifest.texture_palette_uv_offset_y.emplace_back(entry.getUvOffsetY());
        manifest.texture_palette_uv_rotation_degrees.emplace_back(entry.getUvRotationDegrees());
    }
    return manifest;
}

std::optional<HeightField> LoadHeightField(const AssetReadFn& read, std::string_view map_root)
{
    auto manifest = LoadManifest(read, map_root);
    if (!manifest || manifest->world_size_cells == 0 || manifest->chunk_size_cells == 0 ||
        manifest->zone_grid_x == 0 || manifest->zone_grid_y == 0) {
        return std::nullopt;
    }

    HeightField field;
    field.manifest = *manifest;
    field.width_vertices = manifest->world_size_cells + 1;
    field.height_vertices = manifest->world_size_cells + 1;
    field.heights_cm.assign(static_cast<std::size_t>(field.width_vertices) * field.height_vertices, 0);
    field.attributes.assign(static_cast<std::size_t>(manifest->world_size_cells) * manifest->world_size_cells, 0);
    std::uint32_t chunk_splat_width = 0;
    std::uint32_t chunk_splat_height = 0;

    for (std::uint32_t cy = 0; cy < manifest->zone_grid_y; ++cy) {
        for (std::uint32_t cx = 0; cx < manifest->zone_grid_x; ++cx) {
            const std::string chunk_name =
                "chunks/chunk_" + std::to_string(cx) + "_" + std::to_string(cy) + ".mxchunk";
            const auto bytes = read(JoinAssetPath(map_root, chunk_name));
            if (!bytes) {
                return std::nullopt;
            }

            ParsedChunk chunk;
            if (!ParseChunk(*bytes, cx, cy, manifest->chunk_size_cells, chunk)) {
                return std::nullopt;
            }
            if (chunk_splat_width == 0) {
                chunk_splat_width = chunk.splat_width;
                chunk_splat_height = chunk.splat_height;
                field.splat_width = manifest->zone_grid_x * chunk_splat_width;
                field.splat_height = manifest->zone_grid_y * chunk_splat_height;
                const std::size_t splat_bytes =
                    static_cast<std::size_t>(field.splat_width) * field.splat_height * 4u;
                field.splat_a_rgba8.assign(splat_bytes, 0);
                field.splat_b_rgba8.assign(splat_bytes, 0);
            } else if (chunk.splat_width != chunk_splat_width ||
                       chunk.splat_height != chunk_splat_height) {
                return std::nullopt;
            }

            const auto chunk_vertices = manifest->chunk_size_cells + 1;
            const auto dst_x0 = cx * manifest->chunk_size_cells;
            const auto dst_y0 = cy * manifest->chunk_size_cells;
            for (std::uint32_t y = 0; y < chunk_vertices; ++y) {
                for (std::uint32_t x = 0; x < chunk_vertices; ++x) {
                    const auto dst_x = dst_x0 + x;
                    const auto dst_y = dst_y0 + y;
                    if (dst_x >= field.width_vertices || dst_y >= field.height_vertices) {
                        continue;
                    }
                    field.heights_cm[static_cast<std::size_t>(dst_y) * field.width_vertices + dst_x] =
                        chunk.heights[static_cast<std::size_t>(y) * chunk_vertices + x];
                }
            }

            for (std::uint32_t y = 0; y < manifest->chunk_size_cells; ++y) {
                for (std::uint32_t x = 0; x < manifest->chunk_size_cells; ++x) {
                    const auto dst_x = dst_x0 + x;
                    const auto dst_y = dst_y0 + y;
                    if (dst_x >= manifest->world_size_cells || dst_y >= manifest->world_size_cells) {
                        continue;
                    }
                    field.attributes[static_cast<std::size_t>(dst_y) * manifest->world_size_cells + dst_x] =
                        chunk.attributes[static_cast<std::size_t>(y) * manifest->chunk_size_cells + x];
                }
            }

            const auto splat_dst_x0 = cx * chunk_splat_width;
            const auto splat_dst_y0 = cy * chunk_splat_height;
            for (std::uint32_t y = 0; y < chunk_splat_height; ++y) {
                for (std::uint32_t x = 0; x < chunk_splat_width; ++x) {
                    const auto dst_x = splat_dst_x0 + x;
                    const auto dst_y = splat_dst_y0 + y;
                    const std::size_t src = (static_cast<std::size_t>(y) * chunk_splat_width + x) * 4u;
                    const std::size_t dst = (static_cast<std::size_t>(dst_y) * field.splat_width + dst_x) * 4u;
                    std::memcpy(field.splat_a_rgba8.data() + dst, chunk.splat_a_rgba8.data() + src, 4);
                    std::memcpy(field.splat_b_rgba8.data() + dst, chunk.splat_b_rgba8.data() + src, 4);
                }
            }
        }
    }

    return field.IsValid() ? std::optional<HeightField>(std::move(field)) : std::nullopt;
}

std::optional<WorldLogic> LoadWorldLogic(const AssetReadFn& read, std::string_view map_root)
{
    const auto bytes = read(JoinAssetPath(map_root, "worldlogic.dat"));
    if (!bytes || bytes->size() < 16) {
        return std::nullopt;
    }

    std::size_t offset = 0;
    const auto magic = ReadU32(*bytes, offset);
    const auto version = ReadU32(*bytes, offset);
    const auto zone_count = ReadU32(*bytes, offset);
    const auto spawn_count = ReadU32(*bytes, offset);
    const auto warp_count = ReadU32(*bytes, offset);
    if (magic != 0x314c584d || version != 1 ||
        zone_count > 1024 || spawn_count > 1024 || warp_count > 1024) {
        return std::nullopt;
    }

    WorldLogic logic;
    logic.zones.reserve(zone_count);
    logic.spawns.reserve(spawn_count);
    logic.warps.reserve(warp_count);

    for (std::uint32_t i = 0; i < zone_count; ++i) {
        Zone zone;
        zone.id = ReadU32(*bytes, offset);
        zone.name = ReadString(*bytes, offset);
        zone.bounds = Rect{ReadF32(*bytes, offset),
                           ReadF32(*bytes, offset),
                           ReadF32(*bytes, offset),
                           ReadF32(*bytes, offset)};
        if (offset > bytes->size()) {
            return std::nullopt;
        }
        logic.zones.push_back(std::move(zone));
    }

    for (std::uint32_t i = 0; i < spawn_count; ++i) {
        SpawnRegion spawn;
        spawn.id = ReadU32(*bytes, offset);
        spawn.zone_id = ReadU32(*bytes, offset);
        spawn.bounds = Rect{ReadF32(*bytes, offset),
                            ReadF32(*bytes, offset),
                            ReadF32(*bytes, offset),
                            ReadF32(*bytes, offset)};
        if (offset > bytes->size()) {
            return std::nullopt;
        }
        logic.spawns.push_back(spawn);
    }

    for (std::uint32_t i = 0; i < warp_count; ++i) {
        WarpRegion warp;
        warp.id = ReadU32(*bytes, offset);
        warp.source = Rect{ReadF32(*bytes, offset),
                           ReadF32(*bytes, offset),
                           ReadF32(*bytes, offset),
                           ReadF32(*bytes, offset)};
        warp.target_x = ReadF32(*bytes, offset);
        warp.target_y = ReadF32(*bytes, offset);
        if (offset > bytes->size()) {
            return std::nullopt;
        }
        logic.warps.push_back(warp);
    }

    return logic;
}

} // namespace mx::map
