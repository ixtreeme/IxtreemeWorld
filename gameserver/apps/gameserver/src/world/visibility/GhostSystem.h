#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "BorderSnapshot.h"

// Maintains a zone's read-only ghost copies of neighbor-zone border
// residents. Ghosts are NEVER authoritative and NEVER written back; they
// exist so AOI/visibility/replication can see across zone borders.
// Cross-zone reads use snapshot buffers only (no access into the neighbor's
// flecs storage).
//
// PHASE 5A — incremental maintenance:
//  - BorderPublisher publishes incrementally (only dirty entities) and bumps
//    a publish generation only when the buffer content actually changed.
//  - GhostSystem::Reconcile diffs the neighbor buffers against the current
//    ghost set: unchanged ghosts are KEPT (never destroy+recreate), only
//    membership changes ADD/REMOVE. A neighbor whose publish generation is
//    unchanged is skipped entirely.
//  - GhostSystem::RebuildExact keeps the historic full rebuild as the
//    reference/validation/fallback algorithm; the production tick never
//    calls it.
namespace gs::game {

class Zone;
class ZoneManager;

// One consumer-side cursor per neighbor zone: the publish generation already
// reconciled. Structural state only (zone-local, written under the zone write
// guard by GhostSystem).
struct GhostNeighborCursor {
    std::size_t zone_index = 0;             // neighbor zone slot
    std::uint64_t publish_generation = 0;   // last reconciled neighbor publish
    // True when this neighbor's buffer was processed in the current pass (its
    // ghosts must have been seen); false when skipped as unchanged (its
    // ghosts stay valid without being re-stamped).
    bool changed_this_pass = false;
};

// Zone-local ghost maintenance state. Plain data; the zone owns it and only
// GhostSystem touches it (under the zone write guard).
struct GhostMaintenanceState {
    // Bumped once per reconcile; stamps each kept ghost (last_seen_generation).
    std::uint32_t reconcile_generation = 0;
    std::vector<GhostNeighborCursor> neighbors;
    // Reused buffer for copying one neighbor's publish buffer (capacity kept
    // across ticks: no steady-state allocation). On the delta path it holds
    // only the snapshots named by the neighbor's latest delta.
    std::vector<BorderEntitySnapshot> neighbor_scratch;
    // Reused buffer for one neighbor's removed-net delta (delta path only).
    std::vector<std::uint32_t> neighbor_removed_scratch;
    // Local residency changes (spawn/despawn/transfer) invalidate the fast
    // path: a net that became local must lose its ghost copy even when the
    // source neighbor did not republish.
    std::uint64_t last_local_residency_generation = 0;
    // First reconcile, topology change or explicit repair: process every
    // neighbor buffer regardless of its publish generation.
    bool force_full = true;
};

class GhostSystem {
public:
    // Production hot path: incremental reconcile (KEEP/ADD/REMOVE diff).
    static void Reconcile(Zone& zone, ZoneManager& zones);
    // Reference / fallback: the historic full exact rebuild (drop everything,
    // recreate from the neighbor buffers). Used by the equivalence validator's
    // repair path and debug/benchmark force-rebuilds.
    static void RebuildExact(Zone& zone, ZoneManager& zones);
    // Drops all ghost state (zone sleep / retire / no players).
    static void Clear(Zone& zone);
    // Explicit invalidation (migration/despawn/death): remove one ghost copy
    // immediately, independent of the reconcile schedule.
    static void RemoveByNetId(Zone& zone, std::uint32_t net_id);
};

} // namespace gs::game
