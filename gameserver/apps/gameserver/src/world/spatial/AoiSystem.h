#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include <flecs.h>

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

// One AOI candidate: distance for ordering/cap, identity, and the zone-local
// entity handle (resident or ghost). The handle lets the visibility reconcile
// read the transform version without a hash lookup or a ghost scan; the
// version itself is deliberately NOT cached here (it changes every tick and
// reading it for every candidate would put two component lookups on the hot
// AOI path).
struct AoiCandidate {
    float distance_sq = 0.0f;
    std::uint32_t net_id = 0;
    flecs::entity entity;
};

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
    // Returns the capped, distance-ordered candidate list (nearest first,
    // deterministic distance,net tie-break). The returned reference points at
    // a per-thread scratch buffer: it is valid until the next AOI query on
    // the same thread (the caller consumes it immediately).
    //
    // `partial_cap` selects the top-k reduction (partial_sort) instead of a
    // full sort when the candidate set exceeds kAoiEntityCap. Both produce
    // exactly the same selected set -- the prefilter is a grid superset and
    // the exact distance filter never changes.
    // `reference_positions` is the phase 5D A/B switch: true reads each
    // candidate's position from the authoritative component (pre-5D path),
    // false reads the grid entry's synced copy (optimized). Identical result.
    // `count_metrics` is false for shadow-validation queries (they must not
    // pollute the production AOI diagnostics they are validating).
    static const std::vector<AoiCandidate>& QueryCandidates(Zone& zone,
                                                            std::uint32_t viewer_net_id,
                                                            const Position& viewer_position,
                                                            bool partial_cap,
                                                            bool reference_positions = false,
                                                            bool nth_element = true,
                                                            bool count_metrics = true);
};

} // namespace gs::game
