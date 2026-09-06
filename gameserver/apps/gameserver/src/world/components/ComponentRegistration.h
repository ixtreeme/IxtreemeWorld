#pragma once

#include <flecs.h>

#include "AiComponents.h"
#include "CombatComponents.h"
#include "MigrationComponents.h"
#include "MobComponents.h"
#include "MovementComponents.h"
#include "NetworkComponents.h"
#include "Tags.h"
#include "TransformComponents.h"

// Registers every component type with a zone-local flecs world.
// Called once per Zone construction.
namespace gs::game {

inline void RegisterWorldComponents(flecs::world& world)
{
    world.component<Position>();
    world.component<Heading>();
    world.component<Velocity>();
    world.component<MoveIntent>();
    world.component<MoveSpeed>();
    world.component<Hp>();
    world.component<CombatStats>();
    world.component<AttackCooldown>();
    world.component<NetId>();
    world.component<SessionRef>();
    world.component<PlayerTag>();
    world.component<MobTag>();
    world.component<MobTypeRef>();
    world.component<MobSpawnRef>();
    world.component<MobProfile>();
    world.component<WanderState>();
    world.component<GhostTag>();
    world.component<MigrateTo>();
}

} // namespace gs::game
