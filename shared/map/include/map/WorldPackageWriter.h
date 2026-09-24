#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "map/MapData.h"
#include "schema/map_manifest.capnp.h"

// World-package writer (MAP-1): the single implementation of the format
// contract's write side, shared by mapgen_test_zone and the test fixtures.
// Byte layouts are written field by field in little-endian order -- never by
// dumping in-memory structs.
namespace mx::map {

struct PackageWriteSpec {
    std::uint32_t format_version = 3; // 3 = current; 2 = legacy layout for the v2-only client
    std::string world_id;
    std::string world_name;
    std::uint32_t size_cells = 0; // square worlds (MAP-1)
    float cell_size_m = 1.0f;
    std::uint32_t chunk_size_cells = 0;
    // Samples: vertex (vx, vy) in [0, size], cell (cx, cy) in [0, size).
    std::function<std::int16_t(std::uint32_t vx, std::uint32_t vy)> height_cm;
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
    // Existing map.manifest in the target is never replaced unless set.
    bool overwrite = false;
    // Test hook: last-moment edits of the manifest message (corrupt fixtures).
    std::function<void(schema::MapManifest::Builder&)> patch_manifest;
};

struct PackageWriteResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> files; // package-relative, in write order
};

PackageWriteResult WritePackage(const std::filesystem::path& out_dir, const PackageWriteSpec& spec);

std::vector<std::uint8_t> EncodeWorldLogic(const WorldLogic& logic);
std::vector<std::uint8_t> EncodeChunk(const PackageWriteSpec& spec, std::uint32_t chunk_x, std::uint32_t chunk_y);

} // namespace mx::map
