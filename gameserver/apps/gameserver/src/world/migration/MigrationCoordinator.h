#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "common/Types.h"

#include "../OwnerMap.h"
#include "EntityTransfer.h"

// Cross-zone ownership transfer orchestration (supervisor side).
// Processes MigrationRequests queued by zone-local movement steps and
// executes each transfer as explicit stages:
//
//   Validate -> Capture snapshot -> Release source -> Apply destination
//     -> Commit (OwnerMap)
//
// At most one zone is authoritative for a NetId at any time; flecs handles
// never cross zones (see EntityTransfer). If Apply fails after the source
// was released, the captured snapshot is restored to the source (rollback);
// if the restore also fails, the snapshot is quarantined -- never silently
// lost -- and visible in diagnostics/validation.
// Single-threaded: called only from the supervisor loop when no zone tick is
// in progress. Concrete class, no interface (single implementation).
namespace gs::game {

class ZoneManager;
class TerrainService;
class MigrationQueue;
class Zone;

class MigrationCoordinator {
public:
    MigrationCoordinator(ZoneManager& zones,
                         TerrainService& terrain,
                         OwnerMap& owners,
                         MigrationQueue& queue);

    void ProcessMigrations(std::uint32_t world_tick);

    std::size_t QuarantinedCount() const noexcept
    {
        return quarantined_.size();
    }

private:
    // Stale requests are dropped safely (returns false, nothing changed).
    bool ExecuteMigration(std::size_t source_zone_index,
                          std::size_t target_zone_index,
                          std::uint32_t net_id,
                          std::uint32_t world_tick);

    bool MigratePlayer(Zone& source_zone,
                       Zone& target_zone,
                       std::size_t target_zone_index,
                       std::uint32_t net_id,
                       std::uint32_t world_tick);
    bool MigrateMob(Zone& source_zone,
                    Zone& target_zone,
                    std::size_t target_zone_index,
                    std::uint32_t net_id,
                    std::uint32_t world_tick);

    // Deterministic recovery: re-apply a captured snapshot to the source
    // zone when destination apply failed. Returns false when even the
    // restore failed (snapshot stays quarantined).
    bool RestoreToSource(Zone& source_zone, EntityTransfer transfer);

    ZoneManager& zones_;
    TerrainService& terrain_;
    OwnerMap& owners_;
    MigrationQueue& queue_;

    struct QuarantinedTransfer {
        EntityTransfer transfer;
        bool is_player = false;
    };
    std::vector<QuarantinedTransfer> quarantined_;
};

} // namespace gs::game
