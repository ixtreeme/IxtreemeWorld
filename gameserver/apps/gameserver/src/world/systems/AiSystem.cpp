#include "AiSystem.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "../WorldConstants.h"
#include "../components/AiComponents.h"
#include "../components/MobComponents.h"
#include "../components/MovementComponents.h"
#include "../components/NetworkComponents.h"
#include "../components/Tags.h"
#include "../components/TransformComponents.h"
#include "../spawn/MobPrototypeRegistry.h"
#include "../spawn/SpawnRandom.h"
#include "../zone/Zone.h"
#include "../zone/ZoneOwnership.h"

namespace gs::game {

void AiSystem::StepWander(Zone& zone, float dt, MobPrototypeRegistry& mob_types)
{
    AssertZoneOwner(zone, "zone mob wander ai");

    // Two-phase iteration with only narrow const queries: wide query-each()
    // with an entity plus 6+ components crashes MSVC 14.51 (ICE), so handles
    // are collected first and components are read/updated per entity after.
    // Ghosts carry MobTag but no WanderState and are skipped up front.
    std::vector<flecs::entity> mobs;
    mobs.reserve(static_cast<std::size_t>(zone.Diagnostics().mob_count.load(std::memory_order_relaxed)));
    zone.World().query<const MobTag>().each([&](flecs::entity entity, const MobTag&) {
        if (!entity.has<GhostTag>()) {
            mobs.push_back(entity);
        }
    });

    std::uint32_t moving_count = 0;
    std::uint32_t idle_count = 0;

    for (auto entity : mobs) {
        const auto type_ref = entity.get<MobTypeRef>();
        const auto net = entity.get<NetId>();
        const auto position = entity.get<Position>();
        auto wander = entity.get<WanderState>();
        auto intent = entity.get<MoveIntent>();

        const auto* type = mob_types.Find(type_ref.id);
        const float idle_min = type != nullptr ? type->wander_idle_min : 3.0f;
        const float idle_max = type != nullptr ? type->wander_idle_max : 8.0f;
        auto& rng = zone.MobRng(net.value, type_ref.id);

        if (wander.mode == WanderState::Mode::Idle) {
            wander.timer = std::max(0.0f, wander.timer - dt);
            intent.state = MoveState::Idle;
            if (wander.timer <= 0.0f && wander.spawn_radius > 0.0f) {
                wander.target = RandomPointInCircle(rng, wander.spawn_center, wander.spawn_radius);
                wander.mode = WanderState::Mode::Moving;
                wander.timer = 0.0f;
            }
        }

        if (wander.mode == WanderState::Mode::Moving) {
            const float dx = wander.target.x - position.x;
            const float dy = wander.target.y - position.y;
            const float distance_sq = dx * dx + dy * dy;
            if (distance_sq <= kWanderArrivalDistanceMeters * kWanderArrivalDistanceMeters) {
                wander.mode = WanderState::Mode::Idle;
                wander.timer = RandomRange(rng, idle_min, idle_max);
                intent.state = MoveState::Idle;
            } else {
                intent.dir_angle = std::atan2(dx, dy);
                intent.state = MoveState::Walking;
            }
        }

        if (wander.mode == WanderState::Mode::Moving) {
            ++moving_count;
        } else {
            ++idle_count;
        }
        entity.set<WanderState>(wander);
        entity.set<MoveIntent>(intent);
    }

    zone.Diagnostics().wandering_mob_count.store(moving_count, std::memory_order_relaxed);
    zone.Diagnostics().idle_mob_count.store(idle_count, std::memory_order_relaxed);
}

} // namespace gs::game
