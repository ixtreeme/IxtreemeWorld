#include "AiSystem.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "../WorldConstants.h"
#include "../components/AiComponents.h"
#include "../components/MobComponents.h"
#include "../components/MovementComponents.h"
#include "../components/NetworkComponents.h"
#include "../components/SimulationLod.h"
#include "../components/Tags.h"
#include "../components/TransformComponents.h"
#include "../spawn/MobPrototypeRegistry.h"
#include "../spawn/SpawnRandom.h"
#include "../zone/Zone.h"
#include "../zone/ZoneOwnership.h"
#include "LodSystem.h"

namespace gs::game {

void AiSystem::StepWander(Zone& zone,
                          float dt,
                          MobPrototypeRegistry& mob_types,
                          const LodConfig* lod_config)
{
    AssertZoneOwner(zone, "zone mob wander ai");
    // LOD timebase: the zone's own tick counter (see SimulationLod.h).
    const std::uint32_t now_tick = zone.TickIndex();

    // LOD disabled (null config): byte-identical legacy behavior, every mob
    // decides every tick. Otherwise only due tiers decide; classification
    // below still runs for all so diagnostics stay exact.
    const bool use_lod = lod_config != nullptr && lod_config->enabled;

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
    std::uint64_t decided = 0;

    for (auto entity : mobs) {
        auto wander = entity.get<WanderState>();
        if (wander.mode == WanderState::Mode::Moving) {
            ++moving_count;
        } else {
            ++idle_count;
        }

        // LOD gate: Dormant never decides; Reduced/Low only on schedule.
        // Skipped mobs keep their last intent (movement integrates it with
        // scaled dt when due, so no time is lost and nothing teleports).
        float dt_eff = dt;
        if (use_lod) {
            if (!entity.has<SimulationLod>()) {
                // Strays simulate fully (safe direction); the validator
                // flags lod-less mobs so the creation path gets fixed.
                assert(false && "AI decision on mob without SimulationLod");
            } else {
                const auto lod = entity.get<SimulationLod>();
                if (!LodSystem::IsDue(lod, now_tick)) {
                    continue;
                }
                dt_eff = dt * static_cast<float>(LodPeriodTicks(lod.tier, *lod_config));
            }
        }

        const auto type_ref = entity.get<MobTypeRef>();
        const auto net = entity.get<NetId>();
        const auto position = entity.get<Position>();
        auto intent = entity.get<MoveIntent>();

        const auto* type = mob_types.Find(type_ref.id);
        const float idle_min = type != nullptr ? type->wander_idle_min : 3.0f;
        const float idle_max = type != nullptr ? type->wander_idle_max : 8.0f;
        auto& rng = zone.MobRng(net.value, type_ref.id);

        if (wander.mode == WanderState::Mode::Idle) {
            wander.timer = std::max(0.0f, wander.timer - dt_eff);
            intent.state = MoveState::Idle;
            if (wander.timer <= 0.0f && wander.spawn_radius > 0.0f) {
                wander.target = RandomPointInCircle(rng, wander.spawn_center, wander.spawn_radius);
                wander.mode = WanderState::Mode::Moving;
                wander.timer = 0.0f;
                ++moving_count;
                --idle_count;
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
                --moving_count;
                ++idle_count;
            } else {
                intent.dir_angle = std::atan2(dx, dy);
                intent.state = MoveState::Walking;
            }
        }

        entity.set<WanderState>(wander);
        entity.set<MoveIntent>(intent);
        ++decided;
    }

    zone.Diagnostics().wandering_mob_count.store(moving_count, std::memory_order_relaxed);
    zone.Diagnostics().idle_mob_count.store(idle_count, std::memory_order_relaxed);
    zone.Diagnostics().lod_ai_updates_since_diag.fetch_add(decided, std::memory_order_relaxed);
}

} // namespace gs::game
