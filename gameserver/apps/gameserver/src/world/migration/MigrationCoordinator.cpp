#include "MigrationCoordinator.h"

#include <algorithm>
#include <exception>
#include <utility>
#include <vector>

#include "common/Logging.h"

#include "../OwnerMap.h"
#include "../WorldConstants.h"
#include "../components/MigrationComponents.h"
#include "../components/NetworkComponents.h"
#include "../components/Tags.h"
#include "../components/TransformComponents.h"
#include "../distributed/MigrationTransport.h"
#include "../distributed/WorldDirectory.h"
#include "../spatial/SpatialTypes.h"
#include "../visibility/GhostSystem.h"
#include "../zone/Zone.h"
#include "../zone/ZoneManager.h"
#include "../zone/ZoneOwnership.h"
#include "../terrain/TerrainService.h"
#include "EntityTransfer.h"
#include "MigrationQueue.h"

namespace gs::game {

MigrationCoordinator::MigrationCoordinator(ZoneManager& zones,
                                           TerrainService& terrain,
                                           OwnerMap& owners,
                                           MigrationQueue& queue,
                                           WorldDirectory& directory,
                                           MigrationTransport& transport,
                                           RuntimeIdentity identity)
    : zones_(zones)
    , terrain_(terrain)
    , owners_(owners)
    , queue_(queue)
    , directory_(directory)
    , transport_(transport)
    , identity_(identity)
{
}

void MigrationCoordinator::ProcessMigrations(std::uint32_t world_tick)
{
    if (zones_.AnyTickInProgress()) {
        return;
    }

    // Event-driven: only queued border crossings are processed.
    // Complexity is O(actual migrations), not O(zones x entities).
    auto requests = queue_.TakeAll();
    if (requests.empty()) {
        return;
    }

    std::sort(requests.begin(), requests.end(), [](const MigrationRequest& lhs,
                                                   const MigrationRequest& rhs) {
        return lhs.net_id < rhs.net_id;
    });

    for (const auto& request : requests) {
        // Idempotency first: a replayed id (retry, late duplicate) can never
        // create a second entity, even if it somehow passed validation.
        if (request.migration_id.IsValid() && AlreadyCommitted(request.migration_id)) {
            metrics_.duplicates.fetch_add(1, std::memory_order_relaxed);
            queue_.Complete(request.net_id);
            continue;
        }
        std::size_t source_index = zones_.ZoneCount();
        std::size_t target_index = zones_.ZoneCount();
        ZoneLocation target_location{};
        if (!ResolveTarget(request, source_index, target_index, target_location)) {
            queue_.Complete(request.net_id);
            continue;
        }
        MigrationOutcome outcome = MigrationOutcome::DroppedStale;
        try {
            outcome = ExecuteMigration(source_index, target_index, target_location, request.net_id,
                                       world_tick);
        } catch (const std::exception& error) {
            LOG_ERROR("migration: net_id={} failed with exception: {}", request.net_id, error.what());
            outcome = MigrationOutcome::Failed;
        } catch (...) {
            LOG_ERROR("migration: net_id={} failed with unknown exception", request.net_id);
            outcome = MigrationOutcome::Failed;
        }
        if (outcome == MigrationOutcome::Committed) {
            metrics_.committed.fetch_add(1, std::memory_order_relaxed);
            if (request.migration_id.IsValid()) {
                MarkCommitted(request.migration_id);
                last_committed_id_.store(request.migration_id.value, std::memory_order_relaxed);
            }
        } else if (outcome == MigrationOutcome::DroppedStale) {
            metrics_.dropped_stale.fetch_add(1, std::memory_order_relaxed);
        } else {
            metrics_.failures.fetch_add(1, std::memory_order_relaxed);
        }
        queue_.Complete(request.net_id);
    }
}

bool MigrationCoordinator::ResolveTarget(const MigrationRequest& request,
                                         std::size_t& out_source_index,
                                         std::size_t& out_target_index,
                                         ZoneLocation& out_target_location)
{
    out_source_index = zones_.FindIndexById(request.source_zone_id);
    out_target_index = zones_.FindIndexById(request.target_zone_id);
    if (out_source_index >= zones_.ZoneCount() || out_target_index >= zones_.ZoneCount() ||
        out_source_index == out_target_index) {
        return false;
    }
    const auto target_location = directory_.ResolveZone(request.target_zone_id);
    if (!target_location) {
        // Stale directory entry (zone removed/reassigned mid-flight).
        return false;
    }
    out_target_location = *target_location;
    if (directory_.IsDraining(*target_location)) {
        // Rolling restart / load migration: no NEW work into draining
        // destinations. Source stays authoritative; the marker re-enqueues.
        metrics_.retries.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!directory_.IsLocal(*target_location)) {
        // Cross-process destination: goes through the transport seam, never
        // through direct world access. Local behavior is untouched.
        const EntityTransfer probe{};
        const auto result = transport_.Send(*target_location, probe, request.migration_id.value);
        if (result != TransportOutcome::EmulatedLoopback) {
            // RemoteNotSupported: keep the source authoritative and retry
            // later (the marker persists and re-enqueues).
            metrics_.retries.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        // EmulatedLoopback: same process, counted as remote -- fall through
        // to the normal staged path below.
    }
    return true;
}

MigrationOutcome MigrationCoordinator::ExecuteMigration(std::size_t source_zone_index,
                                                        std::size_t target_zone_index,
                                                        ZoneLocation target_location,
                                                        std::uint32_t net_id,
                                                        std::uint32_t world_tick)
{
    if (source_zone_index >= zones_.ZoneCount() || target_zone_index >= zones_.ZoneCount() ||
        source_zone_index == target_zone_index || zones_.AnyTickInProgress()) {
        return MigrationOutcome::DroppedStale;
    }

    MigrationOutcome outcome = MigrationOutcome::DroppedStale;
    // Ordered acquisition (by index) keeps the two-zone scope deadlock-free.
    if (source_zone_index < target_zone_index) {
        ZoneWriteGuard source_guard(zones_.GetZone(source_zone_index), "migration source");
        ZoneWriteGuard target_guard(zones_.GetZone(target_zone_index), "migration target");
        auto& source_zone = zones_.GetZone(source_zone_index);
        auto& target_zone = zones_.GetZone(target_zone_index);
        outcome = source_zone.FindPlayer(net_id) != nullptr
                      ? MigratePlayer(source_zone, target_zone, target_zone_index, target_location,
                                      net_id, world_tick)
                      : MigrateMob(source_zone, target_zone, target_zone_index, target_location,
                                   net_id, world_tick);
    } else {
        ZoneWriteGuard target_guard(zones_.GetZone(target_zone_index), "migration target");
        ZoneWriteGuard source_guard(zones_.GetZone(source_zone_index), "migration source");
        auto& source_zone = zones_.GetZone(source_zone_index);
        auto& target_zone = zones_.GetZone(target_zone_index);
        outcome = source_zone.FindPlayer(net_id) != nullptr
                      ? MigratePlayer(source_zone, target_zone, target_zone_index, target_location,
                                      net_id, world_tick)
                      : MigrateMob(source_zone, target_zone, target_zone_index, target_location,
                                   net_id, world_tick);
    }
    return outcome;
}

MigrationOutcome MigrationCoordinator::MigratePlayer(Zone& source_zone,
                                                     Zone& target_zone,
                                                     std::size_t target_zone_index,
                                                     ZoneLocation target_location,
                                                     std::uint32_t net_id,
                                                     std::uint32_t world_tick)
{
    // Stage 1+2: validate routing and authority.
    auto* binding = source_zone.FindPlayer(net_id);
    if (binding == nullptr || !binding->session) {
        return MigrationOutcome::DroppedStale;
    }
    const auto owner_it = owners_.find(binding->session->Id());
    if (owner_it == owners_.end() || owner_it->second.net_id != net_id ||
        owner_it->second.zone_index != zones_.FindIndexById(source_zone.Id())) {
        return MigrationOutcome::DroppedStale;
    }

    const auto entity = source_zone.FindEntity(net_id);
    if (!entity.is_valid()) {
        return MigrationOutcome::DroppedStale;
    }
    if (entity.get<MigrateTo>().target_zone != target_zone.Id()) {
        return MigrationOutcome::DroppedStale;
    }
    const auto position = entity.get<Position>();
    if (zones_.FindIndexForPosition(position.x, position.y) != target_zone_index ||
        DistanceOutsideRect(source_zone.Bounds(), position) <= kMigrationHysteresisMeters) {
        entity.set<MigrateTo>({0});
        return MigrationOutcome::DroppedStale;
    }

    // Stage 3: capture the complete snapshot BEFORE releasing the source.
    EntityTransfer transfer;
    try {
        transfer = BuildTransfer(entity, true, NamespaceFor(identity_));
    } catch (const std::exception& error) {
        LOG_ERROR("migration: net_id={} snapshot capture failed, source untouched: {}",
                  net_id,
                  error.what());
        return MigrationOutcome::DroppedStale;
    }
    auto moved_binding = source_zone.ExtractPlayerBinding(net_id);

    GhostSystem::RemoveByNetId(target_zone, net_id);

    // Stage 4: release source authority. From here on the transfer snapshot
    // (+ moved binding) is the only copy of the entity.
    source_zone.Grid().Remove(net_id, transfer.position);
    entity.destruct();
    source_zone.UnindexEntity(net_id);
    source_zone.RefreshResidentCounts();

    // Stage 5: apply at the destination, with deterministic recovery.
    transfer.position.z = terrain_.SampleGroundHeight(transfer.position.x, transfer.position.y);
    try {
        auto new_entity = ApplyTransfer(target_zone.World(), transfer);
        target_zone.IndexEntity(net_id, new_entity);
        target_zone.Grid().Insert(net_id, transfer.position);
    } catch (const std::exception& error) {
        LOG_ERROR("migration: net_id={} destination apply failed: {}", net_id, error.what());
        Zone::PlayerBinding restore_binding = std::move(moved_binding);
        if (!RestoreToSource(source_zone, transfer)) {
            quarantined_.push_back(QuarantinedTransfer{std::move(transfer), true});
            LOG_ERROR("migration: net_id={} source restore failed, transfer QUARANTINED", net_id);
        } else {
            source_zone.InsertPlayerBinding(net_id, std::move(restore_binding));
            source_zone.RefreshResidentCounts();
        }
        return MigrationOutcome::Failed;
    }

    // Stage 6: commit routing. The global identity and logical location are
    // the cross-process truth; the local caches keep the hot path fast.
    const auto session_id = moved_binding.session ? moved_binding.session->Id() : 0;
    target_zone.InsertPlayerBinding(net_id, std::move(moved_binding));
    target_zone.RefreshResidentCounts();
    if (session_id != 0) {
        OwnerInfo owner;
        owner.entity = transfer.entity_id;
        owner.location = target_location;
        owner.zone_index = target_zone_index;
        owner.net_id = net_id;
        owners_[session_id] = owner;
    }

    source_zone.Diagnostics().migrations_since_diag.fetch_add(1, std::memory_order_relaxed);
    LOG_INFO("migration: net_id={} (player) from_zone={} to_zone={} tick={}",
             net_id,
             source_zone.Id(),
             target_zone.Id(),
             world_tick);
    return MigrationOutcome::Committed;
}

MigrationOutcome MigrationCoordinator::MigrateMob(Zone& source_zone,
                                                  Zone& target_zone,
                                                  std::size_t target_zone_index,
                                                  ZoneLocation target_location,
                                                  std::uint32_t net_id,
                                                  std::uint32_t world_tick)
{
    (void)target_location; // Mobs carry no session routing; the location is
                           // recorded for future cross-process mob ownership.
    const auto entity = source_zone.FindEntity(net_id);
    if (!entity.is_valid() || entity.has<PlayerTag>()) {
        return MigrationOutcome::DroppedStale;
    }
    if (entity.get<MigrateTo>().target_zone != target_zone.Id()) {
        return MigrationOutcome::DroppedStale;
    }
    const auto position = entity.get<Position>();
    if (zones_.FindIndexForPosition(position.x, position.y) != target_zone_index ||
        DistanceOutsideRect(source_zone.Bounds(), position) <= kMigrationHysteresisMeters) {
        entity.set<MigrateTo>({0});
        return MigrationOutcome::DroppedStale;
    }

    EntityTransfer transfer;
    try {
        transfer = BuildTransfer(entity, false, NamespaceFor(identity_));
    } catch (const std::exception& error) {
        LOG_ERROR("migration: net_id={} snapshot capture failed, source untouched: {}",
                  net_id,
                  error.what());
        return MigrationOutcome::DroppedStale;
    }
    auto moved_rng = source_zone.ExtractMobRng(net_id);

    GhostSystem::RemoveByNetId(target_zone, net_id);

    source_zone.Grid().Remove(net_id, transfer.position);
    entity.destruct();
    source_zone.UnindexEntity(net_id);
    source_zone.EraseMobRng(net_id);
    source_zone.RefreshResidentCounts();

    transfer.position.z = terrain_.SampleGroundHeight(transfer.position.x, transfer.position.y);
    try {
        auto new_entity = ApplyTransfer(target_zone.World(), transfer);
        target_zone.IndexEntity(net_id, new_entity);
        target_zone.Grid().Insert(net_id, transfer.position);
    } catch (const std::exception& error) {
        LOG_ERROR("migration: net_id={} destination apply failed: {}", net_id, error.what());
        if (!RestoreToSource(source_zone, transfer)) {
            quarantined_.push_back(QuarantinedTransfer{std::move(transfer), false});
            LOG_ERROR("migration: net_id={} source restore failed, transfer QUARANTINED", net_id);
        } else if (moved_rng) {
            source_zone.InsertMobRng(net_id, std::move(*moved_rng));
            source_zone.RefreshResidentCounts();
        }
        return MigrationOutcome::Failed;
    }
    if (moved_rng) {
        target_zone.InsertMobRng(net_id, std::move(*moved_rng));
    }
    // The LOD state rode along in the transfer payload; count it so sleep
    // decisions stay exact before the next evaluation recount.
    target_zone.NoteLodInsert(transfer.sim_lod.tier);
    target_zone.RefreshResidentCounts();

    source_zone.Diagnostics().migrations_since_diag.fetch_add(1, std::memory_order_relaxed);
    LOG_INFO("migration: net_id={} (mob, type={}) from_zone={} to_zone={} tick={}",
             net_id,
             transfer.mob_type_id,
             source_zone.Id(),
             target_zone.Id(),
             world_tick);
    return MigrationOutcome::Committed;
}

bool MigrationCoordinator::RestoreToSource(Zone& source_zone, EntityTransfer transfer)
{
    try {
        auto restored = ApplyTransfer(source_zone.World(), transfer);
        source_zone.IndexEntity(transfer.net_id, restored);
        source_zone.Grid().Insert(transfer.net_id, transfer.position);
        source_zone.RefreshResidentCounts();
        return true;
    } catch (const std::exception& error) {
        LOG_ERROR("migration: net_id={} restore to source failed: {}",
                  transfer.net_id,
                  error.what());
        return false;
    } catch (...) {
        LOG_ERROR("migration: net_id={} restore to source failed (unknown)", transfer.net_id);
        return false;
    }
}

bool MigrationCoordinator::AlreadyCommitted(MigrationId id) const
{
    return id.IsValid() && committed_ids_.find(id) != committed_ids_.end();
}

void MigrationCoordinator::MarkCommitted(MigrationId id)
{
    if (!id.IsValid()) {
        return;
    }
    if (committed_ids_.insert(id).second) {
        committed_order_.push_back(id);
        while (committed_order_.size() > kCommittedIdCapacity) {
            committed_ids_.erase(committed_order_.front());
            committed_order_.pop_front();
        }
    }
}

} // namespace gs::game
