#pragma once

#include <cstdint>
#include <string>
#include <type_traits>

#include <flecs.h>

#include "../components/AiComponents.h"
#include "../components/CombatComponents.h"
#include "../components/MigrationComponents.h"
#include "../components/MobComponents.h"
#include "../components/MovementComponents.h"
#include "../components/NetworkComponents.h"
#include "../components/TransformComponents.h"
#include "../distributed/GlobalEntityId.h"

// Explicit cross-zone ownership transfer payload. A transfer is a snapshot of
// every authoritative component on the source entity; the supervisor destroys
// the source entity and recreates it in the target zone from this struct.
// Invariant: at most one zone is authoritative for a net_id at any time;
// flecs handles are never treated as global (they are zone-local).
//
// SERIALIZATION CONTRACT (cross-process audit, §9/§36): this DTO is designed
// so a future TCP/IPC message can carry it without redesign. Field audit:
//   entity_id ......... GlobalEntityId (stable, hashable, 0 = invalid)
//   net_id ............ uint32 local id (low bits of entity_id; kept for
//                       wire/component compatibility, NOT as identity)
//   position/heading/velocity/move_intent/move_speed/hp/combat_stats/
//   attack_cooldown/wander ... plain POD structs (asserted below)
//   session ........... SessionId integer (never Session*)
//   mob_type_id ....... stable prototype id
//   spawn_point_index . size_t -- PROCESS-LOCAL INDEX, valid only for
//                       same-process respawn routing. A cross-process schema
//                       must replace it with a stable spawn id (TODO).
//   profile.name ...... std::string (length-prefixed in a future schema)
// FORBIDDEN here: flecs::entity, flecs::world*, Zone*, raw owning pointers,
// function pointers, iterators, std::function, process-local handles.
namespace gs::game {

static_assert(std::is_standard_layout_v<Position>);
static_assert(std::is_standard_layout_v<Heading>);
static_assert(std::is_standard_layout_v<Velocity>);
static_assert(std::is_standard_layout_v<MoveIntent>);
static_assert(std::is_standard_layout_v<MoveSpeed>);
static_assert(std::is_standard_layout_v<Hp>);
static_assert(std::is_standard_layout_v<CombatStats>);
static_assert(std::is_standard_layout_v<AttackCooldown>);
static_assert(std::is_standard_layout_v<WanderState>);

struct EntityTransfer {
    GlobalEntityId entity_id;
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

// NOTE on persistence (§41): this is a RUNTIME handoff DTO, not a save
// format. DB persistence of player state is a separate domain; migration
// must never be confused with (or blocked on) a database write.
EntityTransfer BuildTransfer(flecs::entity entity, bool is_player, std::uint16_t namespace_id = 0);
flecs::entity ApplyTransfer(flecs::world& world, const EntityTransfer& transfer);

} // namespace gs::game
