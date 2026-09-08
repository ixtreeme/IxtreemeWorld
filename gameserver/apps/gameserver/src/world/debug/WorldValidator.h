#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/Types.h"

#include "../activity/SpatialActivityField.h"
#include "../components/SimulationLod.h"
#include "../OwnerMap.h"

// Debug/test world consistency audit across all zones. Checks:
//  - every indexed NetId resolves to a live entity in exactly that zone
//  - no NetId is authoritative in two zones at once
//  - OwnerMap entries point at a live binding + entity in the recorded zone
//  - OwnerMap fast-path caches agree with global identity + directory
//  - no duplicate NetId across zones
//  - spatial index membership matches authority (per zone)
//  - mob RNG drivers have no orphan keys (leak check)
//  - ghosts are never indexed as residents and always carry GhostTag
//  - pending migration requests reference valid zone ids
//  - activity field sources resolve to live players, exactly once (§30)
//  - sleeping zones hold no Full/Reduced-worthy residents, brute-forced (§30)
// Never called on the production hot path. Release overhead: zero (the
// function simply never runs unless a test harness calls it).
namespace gs::game {

class ZoneManager;
class MigrationQueue;
class WorldDirectory;

bool ValidateWorldConsistency(ZoneManager& zones,
                              const OwnerMap& owners,
                              const MigrationQueue& migrations,
                              const WorldDirectory& directory,
                              const ActivityGrid* activity,
                              std::string& out_error);

// Strict field-vs-brute-force audit (§31): for a deterministic sample of
// mobs (zones in index order, nets ascending, first max_samples),
// compares the field tier against brute force over live player positions.
// Requires field_tier numerically <= brute_tier (stronger-or-equal):
// exact match for static entities, conservative-stronger allowed, weaker
// NEVER. Invoke only where entities don't move mid-audit (static test
// scenarios); roaming load races the 1Hz snapshot by construction.
// Fills per-sample results (matched by exact position) for exact per-mob
// asserts. max_samples 0 = all.
struct ActivitySampleResult {
    ZoneId zone_id = 0;
    std::uint32_t net_id = 0;
    float x = 0.0f;
    float y = 0.0f;
    SimulationTier field_tier = SimulationTier::Dormant;
    SimulationTier brute_tier = SimulationTier::Dormant;
};

bool ValidateActivityFieldDetailed(const ZoneManager& zones,
                                   const ActivityGrid& grid,
                                   std::vector<ActivitySampleResult>& out_samples,
                                   std::string& out_error,
                                   std::size_t max_samples = 0);

} // namespace gs::game
