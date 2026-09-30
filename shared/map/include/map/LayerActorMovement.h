#pragma once

#include "map/LayerGroundSupport.h"

namespace mx::map {

// 3D-4B: an upright capsule actor. It is covered by a cooked clearance
// profile when it is not larger in either dimension.
struct LayerActorProfile {
    float radius_m = 0.35f;
    float height_m = 1.8f;
};

// A clearance cell is PASSABLE for a volume when it is clear in the volume's
// own grid, or lies inside the proven step zone of one of the volume's
// proven portals (every overlapped corridor cell free above the two planes'
// upper envelope). Inside a step zone the reported height is the claimed
// volume's plane; a physical controller that is stepping may be up to the
// portal's max_step_m away from it.

// Ground placement (as ResolveLayerGroundPlacement) plus proven free space:
// every cell the point touches must be passable for the actor.
LayerGroundResult ResolveLayerActorPlacement(const LayeredWorld& world,
    const LayerActorProfile& actor, VolumeId volume_id, double x, double y) noexcept;

// Moves a grounded actor on a passable cell along the straight segment to (x, y).
//  * Same volume: every clearance cell the segment touches must be passable
//    (no tunnelling through thin walls, no gaps between cell samples).
//  * Different volume: only across a PROVEN portal joining the two volumes
//    whose shared edge the segment crosses inside the portal span; both
//    parts of the segment must be passable in their own volume. One portal
//    per move; any other volume change answers TransitionRequired.
// Every failure returns the current state unchanged.
LayerGroundResult ResolveLayerActorMove(const LayeredWorld& world,
    const LayerActorProfile& actor, const LayerGroundState& current,
    VolumeId target_volume, double x, double y) noexcept;

} // namespace mx::map
