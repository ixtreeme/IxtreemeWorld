#pragma once

#include <string>

#include "map/MapData.h"

// Thin concrete service over the shared HeightField asset. Answers ground
// height / walkability / world-extent questions for gameplay systems so they
// never touch map storage or the world runtime directly.
namespace gs::game {

class TerrainService {
public:
    TerrainService() = default;
    explicit TerrainService(mx::map::HeightField field);
    // Synthetic world seam (integrated-scale benchmarks): flat, fully
    // walkable terrain with an explicit extent. Production always loads the
    // real height field from the map root; a 100km world cannot ship a
    // 100km^2 heightfield asset, so benchmarks need this explicit extent.
    // The default (no terrain) fallback stays the historic 1000m.
    explicit TerrainService(float flat_extent_meters);

    // Loads the height field from a map root directory. Returns an invalid
    // (flat-fallback) service when the asset cannot be loaded.
    static TerrainService LoadFromMapRoot(const std::string& map_root);

    bool HasTerrain() const noexcept
    {
        return field_.IsValid();
    }

    float SampleGroundHeight(float world_x, float world_y) const noexcept;
    bool IsWalkable(float world_x, float world_y) const noexcept;
    float WorldExtentMeters() const noexcept;

private:
    mx::map::HeightField field_;
    float flat_extent_m_ = 1000.0f; // used when the height field is invalid
};

} // namespace gs::game
