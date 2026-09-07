#pragma once

#include <string>
#include <vector>

#include "map/MapData.h"

#include "PartitionTypes.h"

// Stable LOGICAL geography. Regions never split, never merge, and live as
// long as the world. They are the roots of the dynamic partition trees:
// each region owns one forest of simulation zones (ZonePartition) whose
// leaves reshape with load. World geometry (persistent) vs simulation
// topology (disposable) stay separate concepts.
namespace gs::game {

struct RegionDefinition {
    RegionId id = 0;
    std::string name;
    mx::map::Rect bounds;
    std::uint8_t max_partition_depth = 4;
    // Gameplay/AOI-driven floor: must stay comfortably above AOI radius +
    // ghost border width so cross-boundary visibility keeps working.
    float min_zone_size = 500.0f; // meters
};

// Default 100km x 100km world as four 50km quadrants. Semantic names are
// kept (North/East/South/West); geometry is plain quadrants so it fits any
// rectangular map frame. Bounds are configuration, not forever hardcode:
// callers may supply their own vector instead.
inline std::vector<RegionDefinition> DefaultRegions()
{
    // 100km x 100km in the engine's +X/+Y meter frame.
    constexpr float kWorld = 100000.0f;
    constexpr float kHalf = kWorld * 0.5f;

    return {
        {1, "North", mx::map::Rect{0.0f, kHalf, kHalf, kWorld}, 4, 500.0f},
        {2, "East", mx::map::Rect{kHalf, kHalf, kWorld, kWorld}, 4, 500.0f},
        {3, "South", mx::map::Rect{0.0f, 0.0f, kHalf, kHalf}, 4, 500.0f},
        {4, "West", mx::map::Rect{kHalf, 0.0f, kWorld, kHalf}, 4, 500.0f},
    };
}

inline const RegionDefinition* FindRegionContaining(const std::vector<RegionDefinition>& regions,
                                                    float world_x,
                                                    float world_y)
{
    for (const auto& region : regions) {
        if (region.bounds.Contains(world_x, world_y)) {
            return &region;
        }
    }
    return nullptr;
}

} // namespace gs::game
