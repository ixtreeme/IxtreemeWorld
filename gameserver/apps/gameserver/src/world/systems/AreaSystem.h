#pragma once

#include <cstdint>
#include <optional>

#include "map/MapData.h"
#include "map/WorldAreas.h"

namespace gs::game {

class Zone;
class TerrainService;
struct ZoneTickContext;

// The content area at a point (docs/map-data-format.md section 15): a layered
// volume that belongs to an area as a whole first, then the area raster of a
// package with nested areas, or the MXL1 rectangle that holds the point in a
// package without area layers (flat areas, world-default rules). Outside the
// world: area 0. nullopt: the terrain has no data there yet (not resident);
// the caller keeps the previous area and asks again later.
std::optional<mx::map::AreaId> ResolveArea(const TerrainService& terrain,
                                           const mx::map::WorldLogic& logic,
                                           const mx::map::AreaTable* areas,
                                           float x,
                                           float y,
                                           std::uint32_t volume_id) noexcept;

class AreaSystem {
public:
    // After movement: re-resolves every player that moved since its last
    // resolution, or has none yet. A different area -- or the first one --
    // is a transition: counted, logged and, for a protocol 3 session, sent
    // to the player as S2cAreaChanged.
    static void Step(Zone& zone, ZoneTickContext& ctx);
};

} // namespace gs::game
