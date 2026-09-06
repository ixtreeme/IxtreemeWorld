#pragma once

#include <cstdint>

#include <flecs.h>

#include "../components/AiComponents.h"
#include "../components/CombatComponents.h"
#include "../components/MigrationComponents.h"
#include "../components/MobComponents.h"
#include "../components/MovementComponents.h"
#include "../components/NetworkComponents.h"
#include "../components/TransformComponents.h"

// Explicit cross-zone ownership transfer payload. A transfer is a snapshot of
// every authoritative component on the source entity; the supervisor destroys
// the source entity and recreates it in the target zone from this struct.
// Invariant: at most one zone is authoritative for a net_id at any time;
// flecs handles are never treated as global (they are zone-local).
namespace gs::game {

struct EntityTransfer {
    std::uint32_t net_id = 0;
    bool is_player = false;
    Position position;
    Heading heading;
    Velocity velocity;
    MoveIntent move_intent;
    MoveSpeed move_speed;
    Hp hp;
    CombatStats combat_stats;
    AttackCooldown attack_cooldown;
    gs::common::SessionId session = 0;
    // Mob-only fields:
    WanderState wander;
    std::uint32_t mob_type_id = 0;
    std::size_t spawn_point_index = 0;
    MobProfile profile;
};

EntityTransfer BuildTransfer(flecs::entity entity, bool is_player);
flecs::entity ApplyTransfer(flecs::world& world, const EntityTransfer& transfer);

} // namespace gs::game
