#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "map/LayeredWorld.h"

namespace mx::map {

// Adapter-neutral input produced by a collision mesh or navmesh importer.
// One record is a bounded, walkable surface component; the generator merges
// adjacent records into stable layer volumes.
struct LayerSourceSurface {
    std::uint32_t source_id = 0;
    std::string name;
    Rect bounds;
    float min_z = 0.0f;
    float max_z = 0.0f;
    std::uint32_t tags = VolumeTagNone;
    bool supports_ground_movement = true;
};

struct LayerGenerationOptions {
    Rect world_bounds;
    float merge_xy_gap = 0.05f;
    float merge_z_gap = 0.25f;
    // Collision-derived rectangles must retain their proven footprints and
    // height bands; legacy gap-based AABB merging may invent uncovered floor.
    bool require_exact_footprints = false;
};

struct LayerGenerationReport {
    std::size_t surfaces_seen = 0;
    std::size_t surfaces_merged = 0;
    std::size_t volumes_generated = 0;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;

    bool Ok() const noexcept
    {
        return errors.empty();
    }
};

// Generates volume bounds, stable ids, logical layer ids and semantic kind
// from adapter surfaces. Portals remain explicit because geometry alone
// cannot safely decide whether a nearby vertical relation is a stair, lift,
// teleport, or blocked wall.
bool GenerateLayeredWorld(const std::vector<LayerSourceSurface>& surfaces,
                          const LayerGenerationOptions& options,
                          LayeredWorld& output,
                          LayerGenerationReport& report);

// Text adapter format for offline authoring/import pipelines:
// `surface id name min_x min_y max_x max_y min_z max_z tag|tag 0|1`.
// Blank lines and # comments are ignored; names contain no whitespace.
bool ReadLayeredSurfaceSource(const std::filesystem::path& path,
                              std::vector<LayerSourceSurface>& surfaces,
                              std::vector<std::string>& errors);

} // namespace mx::map
