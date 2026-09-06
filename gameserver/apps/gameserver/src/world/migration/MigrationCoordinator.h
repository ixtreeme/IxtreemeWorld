#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_set>
#include <vector>

#include "common/Types.h"

#include "../OwnerMap.h"
#include "../distributed/MigrationTransport.h"
#include "../distributed/WorldDirectory.h"
#include "EntityTransfer.h"
#include "MigrationQueue.h"

// Cross-zone ownership transfer orchestration (supervisor side).
// Processes MigrationRequests queued by zone-local movement steps and
// executes each transfer as explicit stages:
//
//   Validate -> Capture snapshot -> Release source -> Apply destination
//     -> Commit (OwnerMap)
//
// Remote-capable reading of the same lifecycle (local path runs every
// stage inline; behavior unchanged):
//   Prepared -> Validated -> SnapshotCaptured -> SourceReleased ->
//   DestinationApplied -> Committed (+ Failed/Quarantined branches)
// Remote future only adds: Prepared -> Sent -> DestinationAccepted ->
//   SourceReleased -> Committed. Source release always happens AFTER the
// destination accepted, never before -- authority can never fork.
//
// Idempotency: every request carries a MigrationId; committed ids are
// remembered in a bounded set, so a duplicate delivery (retry, late ACK)
// drops on the set instead of creating a second entity (exactly-once
// EFFECT; at-least-once delivery is acceptable).
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
class Zone;

enum class MigrationOutcome : std::uint8_t {
    Committed = 0,
    DroppedStale, // Validation failed: despawn/disconnect/moved-back/wrong target.
    Failed,       // Apply or restore failed (quarantined if unrestorable).
};

struct MigrationMetrics {
    std::atomic<std::uint64_t> committed{0};
    std::atomic<std::uint64_t> dropped_stale{0};
    std::atomic<std::uint64_t> duplicates{0};
    std::atomic<std::uint64_t> retries{0};
    std::atomic<std::uint64_t> failures{0};

    struct Snapshot {
        std::uint64_t committed = 0;
        std::uint64_t dropped_stale = 0;
        std::uint64_t duplicates = 0;
        std::uint64_t retries = 0;
        std::uint64_t failures = 0;
    };

    Snapshot TakeSnapshot() const noexcept
    {
        return Snapshot{committed.load(std::memory_order_relaxed),
                        dropped_stale.load(std::memory_order_relaxed),
                        duplicates.load(std::memory_order_relaxed),
                        retries.load(std::memory_order_relaxed),
                        failures.load(std::memory_order_relaxed)};
    }
};

class MigrationCoordinator {
public:
    MigrationCoordinator(ZoneManager& zones,
                         TerrainService& terrain,
                         OwnerMap& owners,
                         MigrationQueue& queue,
                         WorldDirectory& directory,
                         MigrationTransport& transport,
                         RuntimeIdentity identity);

    void ProcessMigrations(std::uint32_t world_tick);

    std::size_t QuarantinedCount() const noexcept
    {
        return quarantined_.size();
    }
    MigrationMetrics::Snapshot MetricsSnapshot() const
    {
        return metrics_.TakeSnapshot();
    }
    // Most recently committed id (for tests: lets a harness replay an
    // already-committed id and prove the duplicate drops). Relaxed atomic:
    // monotonicity per writer thread (supervisor) is all anyone needs.
    MigrationId LastCommittedId() const noexcept
    {
        return MigrationId{last_committed_id_.load(std::memory_order_relaxed)};
    }

private:
    // Resolves + validates the request's target location. Returns false for
    // stale requests OR for destinations that cannot take the entity right
    // now (remote without transport, draining, unavailable): the source
    // stays authoritative and the MigrateTo marker re-enqueues later.
    bool ResolveTarget(const MigrationRequest& request,
                       std::size_t& out_source_index,
                       std::size_t& out_target_index,
                       ZoneLocation& out_target_location);

    MigrationOutcome ExecuteMigration(std::size_t source_zone_index,
                                      std::size_t target_zone_index,
                                      ZoneLocation target_location,
                                      std::uint32_t net_id,
                                      std::uint32_t world_tick);

    MigrationOutcome MigratePlayer(Zone& source_zone,
                                   Zone& target_zone,
                                   std::size_t target_zone_index,
                                   ZoneLocation target_location,
                                   std::uint32_t net_id,
                                   std::uint32_t world_tick);
    MigrationOutcome MigrateMob(Zone& source_zone,
                                Zone& target_zone,
                                std::size_t target_zone_index,
                                ZoneLocation target_location,
                                std::uint32_t net_id,
                                std::uint32_t world_tick);

    // Deterministic recovery: re-apply a captured snapshot to the source
    // zone when destination apply failed. Returns false when even the
    // restore failed (snapshot stays quarantined).
    bool RestoreToSource(Zone& source_zone, EntityTransfer transfer);

    static constexpr std::size_t kCommittedIdCapacity = 8192;
    bool AlreadyCommitted(MigrationId id) const;
    void MarkCommitted(MigrationId id);

    ZoneManager& zones_;
    TerrainService& terrain_;
    OwnerMap& owners_;
    MigrationQueue& queue_;
    WorldDirectory& directory_;
    MigrationTransport& transport_;
    RuntimeIdentity identity_;

    // Bounded exactly-once-effect set: committed migration ids. Evicts
    // oldest past capacity (ids are monotonic; old entries are unreplayable
    // once their entities moved on or despawned).
    std::unordered_set<MigrationId> committed_ids_;
    std::deque<MigrationId> committed_order_;
    std::atomic<std::uint64_t> last_committed_id_{0};

    struct QuarantinedTransfer {
        EntityTransfer transfer;
        bool is_player = false;
    };
    std::vector<QuarantinedTransfer> quarantined_;
    MigrationMetrics metrics_;
};

} // namespace gs::game
