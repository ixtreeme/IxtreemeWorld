#pragma once

#include <cstddef>
#include <string>

// Phase 5A exact ghost equivalence validation (debug/test only; never on the
// production hot path). Two independent checks:
//
//  1. PUBLISHER FIDELITY: a zone's canonical border publish buffer must equal
//     an exact fresh build from its authoritative residents (same set, same
//     fields). This catches a missed dirty mark / missed refresh.
//
//  2. RECONCILE EQUIVALENCE: a zone's ghost set must equal the exact expected
//     set derived from the current neighbor publish buffers (first-wins
//     dedupe in neighbor order, local residents excluded) -- the historic
//     full-rebuild semantics. This catches KEEP/ADD/REMOVE diff errors.
//
// Both are read-only: the validator never mutates production state. Run them
// in the supervisor's quiescent window (no tick in flight) so authority,
// buffers and ghosts are stable.
namespace gs::game {

class Zone;
class ZoneManager;

// `out_publish_pending` (optional) is set when the resident set changed after
// the last publish: the next tick refills, so the buffer is legitimately up to
// one tick behind and fidelity is not asserted (reported as skipped).
bool ValidateGhostPublisherFidelity(Zone& zone,
                                    std::string& out_error,
                                    bool* out_publish_pending = nullptr);
// `out_topology_stale` (optional) is set when the zone has not reconciled
// since the last topology change: its cursor set no longer matches the graph,
// so the ghost set is legitimately up to one tick stale and equivalence is
// not asserted (reported as skipped, not as a pass).
bool ValidateGhostEquivalence(Zone& zone,
                              ZoneManager& zones,
                              std::string& out_error,
                              bool* out_topology_stale = nullptr);

// All zones. `out_zones_checked` / `out_ghosts_checked` are optional
// diagnostics; `out_zones_skipped` counts zones skipped for a legitimate
// one-tick lag (unreconciled topology change or unpublished resident set).
// Returns false on the first mismatch (out_error names it).
bool ValidateAllGhostEquivalence(ZoneManager& zones,
                                 std::string& out_error,
                                 std::size_t* out_zones_checked = nullptr,
                                 std::size_t* out_ghosts_checked = nullptr,
                                 std::size_t* out_zones_skipped = nullptr);

} // namespace gs::game
