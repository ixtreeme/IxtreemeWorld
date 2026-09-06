#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "../WorldConstants.h"
#include "../components/TransformComponents.h"

// AOI answers ONLY: "which net_ids are physically near the viewer?"
// It returns sorted, capped candidate ids. It never decides visibility
// (VisibilitySystem) and never sends anything (ReplicationSystem).
//
// The spatial index itself is maintained incrementally by lifecycle and
// movement code; RebuildInto reconstructs a grid from authority and exists
// for validators/repair -- it is NOT part of the tick hot path.
namespace gs::game {

class Zone;
class SpatialGrid;

// Ghost positions resolved once per zone tick (not once per viewer).
using GhostPositionCache = std::vector<std::pair<std::uint32_t, Position>>;

// Relevance tier by viewer distance (§29 seam). Boundaries are thirds of the
// AOI radius. Classification only -- send rates are unchanged today.
enum class RelevanceTier : std::uint8_t {
    Near = 0,
    Mid,
    Far,
};

inline RelevanceTier RelevanceTierForDistanceSq(float distance_sq) noexcept
{
    const float third = kAoiRadiusMeters / 3.0f;
    if (distance_sq <= third * third) {
        return RelevanceTier::Near;
    }
    const float two_thirds = third * 2.0f;
    return distance_sq <= two_thirds * two_thirds ? RelevanceTier::Mid : RelevanceTier::Far;
}

class AoiSystem {
public:
    static void RebuildInto(Zone& zone, SpatialGrid& grid);
    static GhostPositionCache BuildGhostCache(const Zone& zone);
    static std::vector<std::uint32_t> QueryCandidates(Zone& zone,
                                                      std::uint32_t viewer_net_id,
                                                      const Position& viewer_position,
                                                      const GhostPositionCache& ghosts);
};

} // namespace gs::game
