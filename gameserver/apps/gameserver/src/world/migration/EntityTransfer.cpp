#include "EntityTransfer.h"

#include <cassert>

#include "../components/Tags.h"

namespace gs::game {

EntityTransfer BuildTransfer(flecs::entity entity, bool is_player, std::uint16_t namespace_id)
{
    EntityTransfer transfer;
    transfer.net_id = entity.get<NetId>().value;
    transfer.entity_id = ToGlobalEntityId(transfer.net_id, namespace_id);
    transfer.is_player = is_player;
    transfer.position = entity.get<Position>();
    transfer.heading = entity.get<Heading>();
    transfer.velocity = entity.get<Velocity>();
    transfer.move_intent = entity.get<MoveIntent>();
    transfer.move_speed = entity.get<MoveSpeed>();
    transfer.hp = entity.get<Hp>();
    transfer.combat_stats = entity.get<CombatStats>();
    transfer.attack_cooldown = entity.get<AttackCooldown>();
    if (is_player) {
        transfer.session = entity.get<SessionRef>().session;
    } else {
        transfer.wander = entity.get<WanderState>();
        transfer.mob_type_id = entity.get<MobTypeRef>().id;
        transfer.spawn_point_index = entity.get<MobSpawnRef>().spawn_point_index;
        transfer.profile = entity.get<MobProfile>();
        // LOD rides along only when present; absence means "treat as fresh
        // Full" on apply (the validator flags lod-less mobs loudly).
        if (entity.has<SimulationLod>()) {
            transfer.sim_lod = entity.get<SimulationLod>();
        } else {
            transfer.sim_lod = SimulationLod{};
        }
    }
    return transfer;
}

flecs::entity ApplyTransfer(flecs::world& world, const EntityTransfer& transfer)
{
    // A transfer without identity must never become an entity. Both ids
    // must agree: entity_id is the cross-process truth, net_id its local
    // low bits (kept for wire/component compatibility).
    assert(transfer.net_id != 0);
    assert(transfer.entity_id.IsValid());
    assert(ToNetId(transfer.entity_id) == transfer.net_id);
    auto entity = world.entity()
                      .set<Position>(transfer.position)
                      .set<Heading>(transfer.heading)
                      .set<Velocity>(transfer.velocity)
                      .set<MoveIntent>(transfer.move_intent)
                      .set<MoveSpeed>(transfer.move_speed)
                      .set<Hp>(transfer.hp)
                      .set<CombatStats>(transfer.combat_stats)
                      .set<AttackCooldown>(transfer.attack_cooldown)
                      .set<NetId>({transfer.net_id})
                      .set<MigrateTo>({0});
    if (transfer.is_player) {
        entity.set<SessionRef>({transfer.session}).add<PlayerTag>();
    } else {
        entity.set<WanderState>(transfer.wander)
            .set<MobTypeRef>({transfer.mob_type_id})
            .set<MobSpawnRef>({transfer.spawn_point_index})
            .set<MobProfile>(transfer.profile)
            .set<SimulationLod>(transfer.sim_lod)
            .add<MobTag>();
    }
    return entity;
}

} // namespace gs::game
