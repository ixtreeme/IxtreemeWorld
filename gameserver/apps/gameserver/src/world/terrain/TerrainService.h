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
};

} // namespace gs::game
