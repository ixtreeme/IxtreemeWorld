#include "CombatSystem.h"

#include <algorithm>
#include <cmath>

#include "common/Logging.h"

#include "../components/CombatComponents.h"
#include "../components/MobComponents.h"
#include "../components/MovementComponents.h"
#include "../components/NetworkComponents.h"
#include "../components/Tags.h"
#include "../components/TransformComponents.h"
#include "../replication/ProtocolEncoder.h"
#include "../spawn/MobPrototypeRegistry.h"
#include "../visibility/GhostSystem.h"
#include "../zone/Zone.h"
#include "../zone/ZoneOwnership.h"

namespace gs::game {

CombatSystem::AttackResult CombatSystem::ProcessAttack(Zone& zone,
                                                       gs::common::SessionId attacker_session_id,
                                                       std::uint32_t target_net_id,
                                                       ZoneTickContext& ctx)
{
    AssertZoneOwner(zone, "zone combat command");

    std::uint32_t attacker_net_id = 0;
    for (const auto& [net_id, binding] : zone.Players()) {
        if (binding.session && binding.session->Id() == attacker_session_id) {
            attacker_net_id = net_id;
            break;
        }
    }
    if (attacker_net_id == 0) {
        return {};
    }

    if (target_net_id == 0 || target_net_id == attacker_net_id) {
        return {};
    }

    const auto attacker_entity = zone.FindEntity(attacker_net_id);
    if (!attacker_entity.is_valid()) {
        return {};
    }

    const auto target_entity = zone.FindEntity(target_net_id);
    if (!target_entity.is_valid()) {
        for (const auto& ghost : zone.Ghosts()) {
            if (ghost.snapshot.net_id == target_net_id) {
                LOG_INFO("combat: rejected - target is ghost attacker={} target={}",
                         attacker_net_id,
                         target_net_id);
                break;
            }
        }
        return {};
    }
    if (target_entity.has<PlayerTag>()) {
        LOG_INFO("combat: rejected - PvP not allowed attacker={} target={}", attacker_net_id, target_net_id);
        return {};
    }

    if (attacker_entity.get<AttackCooldown>().remaining > 0.0f) {
        return {};
    }

    const auto attacker_position = attacker_entity.get<Position>();
    const auto attacker_stats = attacker_entity.get<CombatStats>();
    const auto target_position = target_entity.get<Position>();
    const auto target_stats = target_entity.get<CombatStats>();

    const float dx = target_position.x - attacker_position.x;
    const float dy = target_position.y - attacker_position.y;
    const float dist_sq = dx * dx + dy * dy;
    const float range = std::max(0.0f, attacker_stats.attack_range);
    if (dist_sq > range * range) {
        LOG_INFO("combat: rejected - out of range attacker={} target={} dist={}",
                 attacker_net_id,
                 target_net_id,
                 std::sqrt(dist_sq));
        return {};
    }

    const float damage_dealt = std::max(1.0f, attacker_stats.damage - target_stats.defense);
    auto target_hp = target_entity.get<Hp>();
    target_hp.current = std::max(0.0f, target_hp.current - damage_dealt);
    target_entity.set<Hp>(target_hp);
    auto attacker_cooldown = attacker_entity.get<AttackCooldown>();
    attacker_cooldown.remaining = std::max(0.0f, attacker_stats.attack_cooldown);
    attacker_entity.set<AttackCooldown>(attacker_cooldown);

    const auto health_payload = MakeHealthUpdate(target_net_id, target_hp);
    for (const auto& [viewer_net_id, viewer] : zone.Players()) {
        if (viewer.session &&
            (viewer_net_id == target_net_id || viewer.visible_net_ids.contains(target_net_id))) {
            ctx.send(viewer.session, health_payload);
        }
    }

    LOG_INFO("combat: net_id={} attacked net_id={} damage={} hp={}/{}",
             attacker_net_id,
             target_net_id,
             damage_dealt,
             target_hp.current,
             target_hp.max);

    AttackResult result;
    result.attacked = true;
    if (target_hp.current > 0.0f) {
        return result;
    }

    const auto death_payload = MakeDeath(target_net_id, attacker_net_id);
    const auto despawn_payload = MakeDespawn(target_net_id);
    for (auto& [viewer_net_id, viewer] : zone.Players()) {
        (void)viewer_net_id;
        if (!viewer.session || !viewer.visible_net_ids.contains(target_net_id)) {
            continue;
        }
        ctx.send(viewer.session, death_payload);
        ctx.send(viewer.session, despawn_payload);
        viewer.visible_net_ids.erase(target_net_id);
    }

    const std::uint32_t mob_type_id = target_entity.get<MobTypeRef>().id;
    const std::size_t spawn_point_index = target_entity.get<MobSpawnRef>().spawn_point_index;
    LOG_INFO("death: net_id={} killer_net_id={} damage_dealt={}", target_net_id, attacker_net_id, damage_dealt);

    const auto* type = ctx.mob_types.Find(mob_type_id);
    const float respawn_time = type != nullptr ? type->respawn_time_sec : 30.0f;
    ctx.respawn_later(spawn_point_index, respawn_time);

    GhostSystem::RemoveByNetId(zone, target_net_id);
    if (target_entity.is_valid()) {
        target_entity.destruct();
    }
    zone.UnindexEntity(target_net_id);
    zone.EraseMobRng(target_net_id);
    zone.RefreshResidentCounts();

    result.killed = true;
    return result;
}

} // namespace gs::game
