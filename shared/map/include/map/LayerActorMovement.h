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

// 3D-5B2: an actor crossing a proven terrain edge. Ok proves the VOLUME side
// only; the caller, which owns the terrain, must still check the terrain
// side: known and walkable terrain, a clear terrain path, and a terrain
// height at the crossing within `step_m` of `plane_z`.
struct LayerTerrainCrossing {
    GroundSupportStatus status = GroundSupportStatus::NotAvailable;
    std::uint32_t edge_id = 0;
    VolumeId volume_id = 0;
    LayerId layer_id = 0;
    double x = 0.0; // crossing point on the edge
    double y = 0.0;
    double plane_z = 0.0; // support height at the crossing point
    double step_m = 0.0;  // the profile step height
    LayerGroundState inside; // enter only: the grounded pose at the target

    bool Ok() const noexcept { return status == GroundSupportStatus::Ok; }
};

// A grounded actor on `current` moving to (x, y) OUTSIDE its volume's
// footprint: Ok when the segment first leaves through a terrain edge of that
// volume inside its span, the edge-adjacent corridor cell there is proven
// clear and the volume part of the segment is passable. TransitionRequired
// when no terrain edge is crossed, Blocked when the crossing is not proven.
LayerTerrainCrossing ResolveLayerActorExitToTerrain(const LayeredWorld& world,
    const LayerActorProfile& actor, const LayerGroundState& current, double x, double y) noexcept;

// A terrain actor at (from_x, from_y), outside the footprint it enters,
// moving to (x, y): Ok when the segment first enters a volume through one of
// its terrain edges (from the terrain side), the edge-adjacent corridor cell
// is proven clear, the volume part is passable and the actor fits at (x, y).
// NotAvailable when no terrain edge is entered (stay a terrain actor).
LayerTerrainCrossing ResolveLayerActorEnterFromTerrain(const LayeredWorld& world,
    const LayerActorProfile& actor, double from_x, double from_y, double x, double y) noexcept;

} // namespace mx::map
