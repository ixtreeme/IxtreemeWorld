#pragma once

#include <cstddef>

#include "map/MapData.h"

// Thin concrete service over the shared HeightField asset. Answers ground
// height / walkability / world-extent questions for gameplay systems so they
// never touch map storage or the world runtime directly.
namespace gs::game {

class TerrainService {
public:
    TerrainService() = default;
    // File-backed terrain from a validated world package (MAP-1:
    // WorldPackageLoader). Throws std::invalid_argument for an invalid field
    // -- there is no silent flat fallback any more.
    explicit TerrainService(mx::map::HeightField field);
    // Synthetic world seam (explicit synthetic mode / integrated-scale
    // benchmarks): flat, fully walkable terrain with an explicit extent.
    explicit TerrainService(float flat_extent_meters);

    bool HasTerrain() const noexcept
    {
        return field_.IsValid();
    }

    float SampleGroundHeight(float world_x, float world_y) const noexcept;
    bool IsWalkable(float world_x, float world_y) const noexcept;
    float WorldExtentMeters() const noexcept;
    // Bytes held by the resident height + attribute arrays.
    std::size_t ResidentBytes() const noexcept;

private:
    mx::map::HeightField field_;
    float flat_extent_m_ = 1000.0f; // synthetic / not-yet-initialized service
};

} // namespace gs::game
