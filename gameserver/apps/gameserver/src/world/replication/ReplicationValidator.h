#pragma once

#include <cstddef>
#include <string>

// Phase 5B exact/shadow validation of the optimized replication state.
// Read-only; run it in the supervisor's quiescent window (no tick in flight),
// never during a performance measurement.
//
//  1. INTEREST EQUIVALENCE: every viewer's interest set equals the exact
//     brute-force AOI set over the same universe the spatial grid indexes
//     (authoritative residents + ghosts), with the same exact distance test,
//     nearest-first ordering and NetId tie-break, capped identically. Catches
//     prefilter false negatives, grid drift, cap divergence, stale entries.
//  2. RECIPIENT COVERAGE: for every (viewer, visible net) pair the viewer's
//     last-sent transform version is not older than the entity's current
//     version -- a recipient can never hold stale transform state at the tick
//     boundary (dirty replication must not suppress a required update).
//  3. IDENTITY: a visible net resolves to exactly one representation
//     (resident XOR ghost), is never the viewer itself, and every visible
//     replicated entity carries a transform version.
namespace gs::game {

class ZoneManager;

bool ValidateReplicationShadow(ZoneManager& zones,
                               std::string& out_error,
                               std::size_t* out_viewers_checked = nullptr,
                               std::size_t* out_relationships_checked = nullptr);

} // namespace gs::game
