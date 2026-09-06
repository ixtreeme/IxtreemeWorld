#include "MigrationCoordinator.h"

#include <algorithm>
#include <cassert>
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

namespace gs::game {

MigrationCoordinator::MigrationCoordinator(ZoneManager& zones,
                                           TerrainService& terrain,
                                           OwnerMap& owners)
    : zones_(zones)
    , terrain_(terrain)
    , owners_(owners)
{
}

void MigrationCoordinator::ProcessMigrations(std::uint32_t world_tick)
{
    if (zones_.AnyTickInProgress()) {
        return;
    }

    struct MigrationPlan {
        std::uint32_t net_id = 0;
        std::size_t source_zone_index = 0;
        std::size_t target_zone_index = 0;
    };

    std::vector<MigrationPlan> plans;
    for (std::size_t zone_index = 0; zone_index < zones_.ZoneCount(); ++zone_index) {
        auto& zone = zones_.GetZone(zone_index);
        ZoneWriteGuard guard(zone, "migration scan");
        for (const auto& [net_id, binding] : zone.Players()) {
            if (!binding.session) {
                continue;
            }
            const auto owner_it = owners_.find(binding.session->Id());
            if (owner_it == owners_.end() || owner_it->second.net_id != net_id ||
                owner_it->second.zone_index != zone_index) {
                continue;
            }
            const auto entity = zone.FindEntity(net_id);
            if (!entity.is_valid()) {
                continue;
            }
            const auto migration = entity.get<MigrateTo>();
            if (migration.target_zone == 0) {
                continue;
            }
            const std::size_t target_index = zones_.FindIndexById(migration.target_zone);
            if (target_index >= zones_.ZoneCount() || target_index == zone_index) {
                continue;
            }
            plans.push_back(MigrationPlan{net_id, zone_index, target_index});
        }
        zone.World().query<const MobTag, const NetId, const MigrateTo>().each(
            [&](const MobTag&, const NetId& id, const MigrateTo& migration) {
                if (migration.target_zone == 0) {
                    return;
                }
                const std::size_t target_index = zones_.FindIndexById(migration.target_zone);
                if (target_index >= zones_.ZoneCount() || target_index == zone_index) {
                    return;
                }
                plans.push_back(MigrationPlan{id.value, zone_index, target_index});
            });
    }

    std::sort(plans.begin(), plans.end(), [](const MigrationPlan& lhs, const MigrationPlan& rhs) {
        return lhs.net_id < rhs.net_id;
    });

    for (const auto& plan : plans) {
        ExecuteMigration(plan.source_zone_index, plan.target_zone_index, plan.net_id, world_tick);
    }
}

void MigrationCoordinator::ExecuteMigration(std::size_t source_zone_index,
                                            std::size_t target_zone_index,
                                            std::uint32_t net_id,
                                            std::uint32_t world_tick)
{
    assert(source_zone_index != target_zone_index);
    if (source_zone_index >= zones_.ZoneCount() || target_zone_index >= zones_.ZoneCount() ||
        source_zone_index == target_zone_index || zones_.AnyTickInProgress()) {
        return;
    }

    auto migrate = [&]() {
        auto& source_zone = zones_.GetZone(source_zone_index);
        auto& target_zone = zones_.GetZone(target_zone_index);

        if (auto* binding = source_zone.FindPlayer(net_id)) {
            if (!binding->session) {
                return;
            }
            const auto owner_it = owners_.find(binding->session->Id());
            if (owner_it == owners_.end() || owner_it->second.net_id != net_id ||
                owner_it->second.zone_index != source_zone_index) {
                return;
            }

            const auto entity = source_zone.FindEntity(net_id);
            if (!entity.is_valid()) {
                return;
            }
            const auto migration = entity.get<MigrateTo>();
            if (migration.target_zone != target_zone.Id()) {
                return;
            }

            const auto position = entity.get<Position>();
            const std::size_t actual_target = zones_.FindIndexForPosition(position.x, position.y);
            if (actual_target != target_zone_index ||
                DistanceOutsideRect(source_zone.Bounds(), position) <= kMigrationHysteresisMeters) {
                entity.set<MigrateTo>({0});
                return;
            }

            GhostSystem::RemoveByNetId(target_zone, net_id);

            auto transfer = BuildTransfer(entity, true);
            auto moved_binding = source_zone.ExtractPlayerBinding(net_id);
            // Source authority ends here: the entity is destroyed before the
            // destination entity is created, so ownership never overlaps.
            if (entity.is_valid()) {
                entity.destruct();
            }
            source_zone.UnindexEntity(net_id);
            source_zone.RefreshResidentCounts();

            transfer.position.z = terrain_.SampleGroundHeight(transfer.position.x, transfer.position.y);
            auto new_entity = ApplyTransfer(target_zone.World(), transfer);
            target_zone.IndexEntity(net_id, new_entity);
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
            return;
        }

        const auto entity = source_zone.FindEntity(net_id);
        if (!entity.is_valid() || entity.has<PlayerTag>()) {
            return;
        }

        const auto migration = entity.get<MigrateTo>();
        if (migration.target_zone != target_zone.Id()) {
            return;
        }

        const auto position = entity.get<Position>();
        const std::size_t actual_target = zones_.FindIndexForPosition(position.x, position.y);
        if (actual_target != target_zone_index ||
            DistanceOutsideRect(source_zone.Bounds(), position) <= kMigrationHysteresisMeters) {
            entity.set<MigrateTo>({0});
            return;
        }

        GhostSystem::RemoveByNetId(target_zone, net_id);

        auto transfer = BuildTransfer(entity, false);
        auto moved_rng = source_zone.ExtractMobRng(net_id);
        // Source authority ends here: the entity is destroyed before the
        // destination entity is created, so ownership never overlaps.
        if (entity.is_valid()) {
            entity.destruct();
        }
        source_zone.UnindexEntity(net_id);
        source_zone.EraseMobRng(net_id);
        source_zone.RefreshResidentCounts();

        transfer.position.z = terrain_.SampleGroundHeight(transfer.position.x, transfer.position.y);
        auto new_entity = ApplyTransfer(target_zone.World(), transfer);
        target_zone.IndexEntity(net_id, new_entity);
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
    };

    if (source_zone_index < target_zone_index) {
        ZoneWriteGuard source_guard(zones_.GetZone(source_zone_index), "migration source");
        ZoneWriteGuard target_guard(zones_.GetZone(target_zone_index), "migration target");
        migrate();
    } else {
        ZoneWriteGuard target_guard(zones_.GetZone(target_zone_index), "migration target");
        ZoneWriteGuard source_guard(zones_.GetZone(source_zone_index), "migration source");
        migrate();
    }
}

} // namespace gs::game
