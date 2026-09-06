#include "MigrationCoordinator.h"

#include <algorithm>
#include <exception>
#include <utility>
#include <vector>

#include "common/Logging.h"

#include "../WorldConstants.h"
#include "../components/MigrationComponents.h"
#include "../components/NetworkComponents.h"
#include "../components/Tags.h"
#include "../components/TransformComponents.h"
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
                                           MigrationQueue& queue)
    : zones_(zones)
    , terrain_(terrain)
    , owners_(owners)
    , queue_(queue)
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
        const std::size_t source_index = zones_.FindIndexById(request.source_zone_id);
        const std::size_t target_index = zones_.FindIndexById(request.target_zone_id);
        bool done = false;
        if (source_index < zones_.ZoneCount() && target_index < zones_.ZoneCount()) {
            // Each execution revalidates everything; stale requests (despawn,
            // disconnect, moved-back entities) are dropped safely inside.
            try {
                done = ExecuteMigration(source_index, target_index, request.net_id, world_tick);
            } catch (const std::exception& error) {
                LOG_ERROR("migration: net_id={} failed with exception: {}",
                          request.net_id,
                          error.what());
            } catch (...) {
                LOG_ERROR("migration: net_id={} failed with unknown exception", request.net_id);
            }
        }
        (void)done;
        queue_.Complete(request.net_id);
    }
}

bool MigrationCoordinator::ExecuteMigration(std::size_t source_zone_index,
                                            std::size_t target_zone_index,
                                            std::uint32_t net_id,
                                            std::uint32_t world_tick)
{
    if (source_zone_index >= zones_.ZoneCount() || target_zone_index >= zones_.ZoneCount() ||
        source_zone_index == target_zone_index || zones_.AnyTickInProgress()) {
        return false;
    }

    bool committed = false;
    // Ordered acquisition (by index) keeps the two-zone scope deadlock-free.
    if (source_zone_index < target_zone_index) {
        ZoneWriteGuard source_guard(zones_.GetZone(source_zone_index), "migration source");
        ZoneWriteGuard target_guard(zones_.GetZone(target_zone_index), "migration target");
        auto& source_zone = zones_.GetZone(source_zone_index);
        auto& target_zone = zones_.GetZone(target_zone_index);
        committed = source_zone.FindPlayer(net_id) != nullptr
                        ? MigratePlayer(source_zone, target_zone, target_zone_index, net_id, world_tick)
                        : MigrateMob(source_zone, target_zone, target_zone_index, net_id, world_tick);
    } else {
        ZoneWriteGuard target_guard(zones_.GetZone(target_zone_index), "migration target");
        ZoneWriteGuard source_guard(zones_.GetZone(source_zone_index), "migration source");
        auto& source_zone = zones_.GetZone(source_zone_index);
        auto& target_zone = zones_.GetZone(target_zone_index);
        committed = source_zone.FindPlayer(net_id) != nullptr
                        ? MigratePlayer(source_zone, target_zone, target_zone_index, net_id, world_tick)
                        : MigrateMob(source_zone, target_zone, target_zone_index, net_id, world_tick);
    }
    return committed;
}

bool MigrationCoordinator::MigratePlayer(Zone& source_zone,
                                         Zone& target_zone,
                                         std::size_t target_zone_index,
                                         std::uint32_t net_id,
                                         std::uint32_t world_tick)
{
    // Stage 1+2: validate routing and authority.
    auto* binding = source_zone.FindPlayer(net_id);
    if (binding == nullptr || !binding->session) {
        return false;
    }
    const auto owner_it = owners_.find(binding->session->Id());
    if (owner_it == owners_.end() || owner_it->second.net_id != net_id ||
        owner_it->second.zone_index != zones_.FindIndexById(source_zone.Id())) {
        return false;
    }

    const auto entity = source_zone.FindEntity(net_id);
    if (!entity.is_valid()) {
        return false;
    }
    if (entity.get<MigrateTo>().target_zone != target_zone.Id()) {
        return false;
    }
    const auto position = entity.get<Position>();
    if (zones_.FindIndexForPosition(position.x, position.y) != target_zone_index ||
        DistanceOutsideRect(source_zone.Bounds(), position) <= kMigrationHysteresisMeters) {
        entity.set<MigrateTo>({0});
        return false;
    }

    // Stage 3: capture the complete snapshot BEFORE releasing the source.
    EntityTransfer transfer;
    try {
        transfer = BuildTransfer(entity, true);
    } catch (const std::exception& error) {
        LOG_ERROR("migration: net_id={} snapshot capture failed, source untouched: {}",
                  net_id,
                  error.what());
        return false;
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
        return false;
    }

    // Stage 6: commit routing.
    const auto session_id = moved_binding.session ? moved_binding.session->Id() : 0;
    target_zone.InsertPlayerBinding(net_id, std::move(moved_binding));
    target_zone.RefreshResidentCounts();
    if (session_id != 0) {
        owners_[session_id] = OwnerInfo{target_zone_index, net_id};
    }

    source_zone.Diagnostics().migrations_since_diag.fetch_add(1, std::memory_order_relaxed);
    LOG_INFO("migration: net_id={} (player) from_zone={} to_zone={} tick={}",
             net_id,
             source_zone.Id(),
             target_zone.Id(),
             world_tick);
    return true;
}

bool MigrationCoordinator::MigrateMob(Zone& source_zone,
                                      Zone& target_zone,
                                      std::size_t target_zone_index,
                                      std::uint32_t net_id,
                                      std::uint32_t world_tick)
{
    const auto entity = source_zone.FindEntity(net_id);
    if (!entity.is_valid() || entity.has<PlayerTag>()) {
        return false;
    }
    if (entity.get<MigrateTo>().target_zone != target_zone.Id()) {
        return false;
    }
    const auto position = entity.get<Position>();
    if (zones_.FindIndexForPosition(position.x, position.y) != target_zone_index ||
        DistanceOutsideRect(source_zone.Bounds(), position) <= kMigrationHysteresisMeters) {
        entity.set<MigrateTo>({0});
        return false;
    }

    EntityTransfer transfer;
    try {
        transfer = BuildTransfer(entity, false);
    } catch (const std::exception& error) {
        LOG_ERROR("migration: net_id={} snapshot capture failed, source untouched: {}",
                  net_id,
                  error.what());
        return false;
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
        return false;
    }
    if (moved_rng) {
        target_zone.InsertMobRng(net_id, std::move(*moved_rng));
    }
    target_zone.RefreshResidentCounts();

    source_zone.Diagnostics().migrations_since_diag.fetch_add(1, std::memory_order_relaxed);
    LOG_INFO("migration: net_id={} (mob, type={}) from_zone={} to_zone={} tick={}",
             net_id,
             transfer.mob_type_id,
             source_zone.Id(),
             target_zone.Id(),
             world_tick);
    return true;
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

} // namespace gs::game
