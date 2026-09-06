#pragma once

#include <string>
#include <unordered_map>

#include "common/Types.h"

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
                              std::string& out_error);

} // namespace gs::game
