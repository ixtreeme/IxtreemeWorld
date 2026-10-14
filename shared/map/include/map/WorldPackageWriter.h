#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "map/MapData.h"
#include "map/LayeredWorld.h"
#include "map/ServerTerrain.h"
#include "map/ServerWater.h"
#include "schema/world_package_manifest.capnp.h"

// World-package writer: the single implementation of the format contract's
// write side, shared by mapgen_test_zone and the test fixtures. Byte layouts
// are written field by field in little-endian order -- never by dumping
// in-memory structs.
namespace mx::map {

struct PackageWriteSpec {
    std::uint32_t format_version = 3; // 3 = current; 2 = legacy layout for the v2-only client
    std::string world_id;
    std::string world_name;
    std::uint32_t size_cells_x = 0;
    std::uint32_t size_cells_y = 0; // 0 = square (size_cells_x); v2 is always square
    double origin_x = 0.0;          // v3 only (v2 is always at (0, 0))
    double origin_y = 0.0;
    float cell_size_m = 1.0f;
    std::uint32_t chunk_size_cells = 0; // v3: the last chunk per axis may be partial
    // Explicit encoding: bilinear emits required height v2, triangle-main-
    // diagonal emits required v3; nullopt preserves implicit height v1.
    // Height v1 is int16, 0.01 m per unit, offset 0. The required v3 version
    // makes older servers reject the new surface contract.
    std::optional<HeightEncoding> height_encoding;
    // Raw stored height at global sample (vx, vy), vx in [0, size_x], vy in
    // [0, size_y]; meters = offset + raw * meters_per_unit. Must fit the
    // sample type (int16 unless the encoding says int32).
    std::function<std::int32_t(std::uint32_t vx, std::uint32_t vy)> height_raw;
    // Attribute bits of cell (cx, cy), cx in [0, size_x), cy in [0, size_y).
    std::function<std::uint16_t(std::uint32_t cx, std::uint32_t cy)> attributes;
    // Client render data. splat_size == 0: server-only package (no splat
    // sections, no splat layers).
    std::uint32_t splat_size = 0;
    std::function<void(std::uint32_t chunk_x, std::uint32_t chunk_y, std::vector<std::uint8_t>& rgba_a,
                       std::vector<std::uint8_t>& rgba_b)>
        splat;
    std::vector<std::string> texture_palette; // client render data (manifest)
    WorldLogic logic;                         // worldlogic.dat (required layer)
    std::optional<std::string> mob_spawns;    // mob_spawns.conf text (mobSpawns layer)
    bool mob_spawns_required = false;
    std::uint16_t mob_spawns_version = 1; // v2 adds required spawn_id and area_id
    // Water capability (v3, MAP-3): Undeclared writes no declaration; Bodies
    // also writes water_bodies.mxws (MXWS v1) + its waterBodies layer.
    WaterModel water_model = WaterModel::Undeclared;
    double sea_level_m = 0.0;
    std::vector<WaterBodyRect> water_bodies;
    // Optional 3D-2 sidecar. Empty keeps the package fully legacy-compatible.
    std::optional<LayeredWorld> layered_world;
    // Existing map.manifest in the target is never replaced unless set.
    bool overwrite = false;
    // Test hook: last-moment edits of the manifest message (corrupt fixtures).
    std::function<void(package_schema::MapManifest::Builder&)> patch_manifest;

    std::uint32_t SizeY() const noexcept
    {
        return size_cells_y != 0 ? size_cells_y : size_cells_x;
    }
};

struct PackageWriteResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> files; // package-relative, in write order
};

PackageWriteResult WritePackage(const std::filesystem::path& out_dir, const PackageWriteSpec& spec);

std::vector<std::uint8_t> EncodeWorldLogic(const WorldLogic& logic);
std::vector<std::uint8_t> EncodeWaterBodies(const std::vector<WaterBodyRect>& bodies);
// One chunk file; a partial edge chunk carries only its in-world cells.
std::vector<std::uint8_t> EncodeChunk(const PackageWriteSpec& spec, std::uint32_t chunk_x, std::uint32_t chunk_y);

} // namespace mx::map
