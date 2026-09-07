#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "RuntimeIds.h"
#include "ZoneLocation.h"
#include "../partition/ZonePartition.h"

// World-wide LOGICAL routing information: which (node, process) owns each
// ZoneId, plus process health. This is deliberately NOT the ZoneManager:
//   ZoneManager   = local runtime OBJECTS (flecs worlds, queues)
//   WorldDirectory = logical routing TABLE (stable ids -> locations)
//
// Single-process default: every configured zone maps to the local identity,
// so every lookup resolves local and behavior is unchanged. A future load
// balancer (or the benchmark's logical-distribution emulation) re-points
// entries via SetAssignment -- no gameplay code changes needed.
//
// Only the supervisor thread mutates; reads happen on the supervisor thread
// (routing decisions). A mutex guards against future/test threads anyway;
// sections are tiny and uncontended.
namespace gs::game {

enum class ProcessStatus : std::uint8_t {
    Alive = 0,    // Accepts new work (spawns, migrations, commands).
    Draining,     // Finishes in-flight work; no NEW migrations/spawns routed in.
    Unavailable,  // Treated as down: route around, retry later.
};

class ZoneManager;

class WorldDirectory {
public:
    explicit WorldDirectory(RuntimeIdentity identity);

    // (Re)builds the table from local zones: every zone -> local location.
    // Called after ZoneManager::BuildFromWorldLogic; preserves any explicit
    // overrides for zones that still exist.
    void RebuildFromManager(const ZoneManager& zones);

    std::optional<ZoneLocation> ResolveZone(ZoneId zone) const;
    // Tree-aware resolution: descends the partition forest to the active
    // leaf covering a position. Null when nothing covers it.
    std::optional<ZoneLocation> ResolveZoneForPosition(
        float world_x,
        float world_y,
        const std::vector<std::unique_ptr<ZonePartition>>& partition_roots) const;
    bool IsLocal(ZoneLocation location) const;
    bool IsDraining(ZoneLocation location) const;

    RuntimeIdentity LocalIdentity() const noexcept
    {
        return identity_;
    }
    ProcessStatus LocalStatus() const;
    void SetLocalStatus(ProcessStatus status);

    // Override one zone's assignment (future load balancer, bench emulation).
    void SetAssignment(ZoneId zone, ZoneLocation location);
    // Drop the override: the zone is local again.
    void ClearAssignment(ZoneId zone);

    // Merge/split support: keep the entry for in-flight migration
    // completion, but mark retired so no NEW work routes there.
    void RetireZones(const std::vector<ZoneId>& zone_ids);

    // Mark/unmark a zone as drained (rolling restart / load migration).
    // Drained LOCAL zones still tick and serve residents; they just stop
    // receiving NEW migrations (spawn-gating is a documented TODO: spawn
    // position determines the zone, so refusing needs balancer-driven
    // placement, not a local veto).
    void SetZoneDrained(ZoneId zone, bool drained);

    // Manual liveness for remote processes. Production source: the future
    // directory sync/heartbeat (§25, not implemented). Test source: the
    // benchmark's logical-distribution emulation. Unknown remotes default
    // to draining (safe: no new work is sent where health is unknown).
    void NoteRemoteAlive(NodeId node, ProcessId process);
    void ForgetRemote(NodeId node, ProcessId process);

    std::size_t KnownZoneCount() const;

private:
    RuntimeIdentity identity_;
    mutable std::mutex mutex_;
    std::unordered_map<ZoneId, ZoneLocation> assignments_;
    std::unordered_set<ZoneId> drained_zones_;
    std::unordered_set<ZoneId> retired_zones_;
    std::set<std::pair<std::uint32_t, std::uint32_t>> alive_remotes_;
    ProcessStatus local_status_ = ProcessStatus::Alive;
};

} // namespace gs::game
