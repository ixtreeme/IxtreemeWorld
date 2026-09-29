#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "map/MapData.h"

#include "../activity/ActivityTypes.h"
#include "PartitionTypes.h"

// Stable LOGICAL geography of the server partition. Regions never split,
// never merge, and live as long as the world. They are the roots of the
// dynamic partition trees: each region owns one forest of simulation zones
// (ZonePartition) whose leaves reshape with load.
//
// MAP-2: regions and the initial leaves come from the SERVER configuration
// applied to the LOADED world bounds -- not from the map's areas and not from
// fixed constants. Axes: +X = east, +Y = north. All rectangles are half-open.
namespace gs::game {

// Aggregate bootstrap cap: total initial leaf zones (regions_x * leaves_x *
// regions_y * leaves_y, or explicit leaves). Every initial leaf is a live
// zone from the first tick: measured ~1.3 MB working set and a supervisor
// pass that grows faster than linearly with the zone count (empty 100 km
// world: 256 zones = 332 MB / 0.18 ms idle pass, 1024 = 1.3 GB / 1.5 ms,
// 4096 = 5.2 GB / 77 ms -- above the 50 ms tick; worldbench --mode
// bootstrap). The per-axis limits alone would admit ~1M initial zones. The
// cap keeps the bootstrap at <= ~1.3 GB of empty zones and an idle
// supervisor pass of ~3% of the tick; finer partitions come from ASF splits
// at runtime, not from bootstrap.
inline constexpr std::uint32_t kMaxInitialLeafZones = 1024;

struct RegionDefinition {
    RegionId id = 0;
    std::string name;
    mx::map::Rect bounds;
    std::uint8_t max_partition_depth = 4;
    // Gameplay/AOI-driven floor: must stay comfortably above AOI radius +
    // ghost border width so cross-boundary visibility keeps working.
    float min_zone_size = 500.0f; // meters
};

// Initial partition, from server config (partition_regions /
// partition_initial_leaves): a regions_x x regions_y grid of regions over
// the world bounds, each tiled by leaves_x x leaves_y initial leaf zones.
// Alternatively `explicit_leaves` (tests / explicit bootstrap descriptions):
// one region covering the world, tiled exactly by the given rectangles.
struct PartitionLayout {
    std::uint32_t regions_x = 2;
    std::uint32_t regions_y = 2;
    std::uint32_t leaves_x = 1; // per region
    std::uint32_t leaves_y = 1;
    std::vector<mx::map::Rect> explicit_leaves;
};

// One initial leaf: its region and bounds (row-major within its region).
struct InitialLeaf {
    RegionId region = 0;
    std::string name;
    mx::map::Rect bounds;
};

struct InitialPartition {
    std::vector<RegionDefinition> regions;
    std::vector<InitialLeaf> leaves;
};

// Compass name of region (ix, iy) in an nx x ny grid (+X east, +Y north):
// 1x1 "World"; 2x1 "West"/"East"; 1x2 "South"/"North"; 2x2 "SouthWest",
// "SouthEast", "NorthWest", "NorthEast"; any other grid "R<ix>_<iy>".
std::string RegionName(std::uint32_t ix, std::uint32_t iy, std::uint32_t nx, std::uint32_t ny);

// Builds the initial partition for `bounds`. Returns false with `error` when
// the layout cannot tile the world safely: an empty/invalid grid, an initial
// leaf narrower than `min_leaf_m` (the AOI safety floor, 2x AOI radius), or
// explicit leaves that do not tile the world exactly (gap or overlap).
bool BuildInitialPartition(const WorldBounds& bounds,
                           const PartitionLayout& layout,
                           float min_leaf_m,
                           InitialPartition& out,
                           std::string& error);

} // namespace gs::game
