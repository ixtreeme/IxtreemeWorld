#include "MovementSystem.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <vector>

#include "common/Logging.h"

#include "../components/AiComponents.h"
#include "../components/MobComponents.h"
#include "../components/MovementComponents.h"
#include "../components/NetworkComponents.h"
#include "../components/SimulationLod.h"
#include "../components/Tags.h"
#include "../components/TransformComponents.h"
#include "../migration/MigrationSystem.h"
#include "../spatial/SpatialTypes.h"
#include "../terrain/TerrainService.h"
#include "../zone/Zone.h"
#include "../zone/ZoneManager.h"
#include "../zone/ZoneOwnership.h"
#include "LodSystem.h"

namespace gs::game {
namespace {

float IntentSpeed(const MoveIntent& intent, const MoveSpeed& speed)
{
    if (intent.state == MoveState::Running) {
        return speed.run;
    }
    if (intent.state == MoveState::Walking) {
        return speed.walk;
    }
    return 0.0f;
}

void TryApplyWarp(Zone& zone, ZoneTickContext& ctx, Position& position, gs::common::SessionId session_id)
{
    AssertZoneOwner(zone, "zone warp application");

    const auto* warp = ctx.world_logic.FindWarp(position.x, position.y);
    if (warp == nullptr) {
        return;
    }

    if (!ctx.terrain.IsWalkable(warp->target_x, warp->target_y)) {
        LOG_WARN("Warp {} target {}, {} is blocked; ignoring warp", warp->id, warp->target_x, warp->target_y);
        return;
    }

    const std::size_t target_zone_index = ctx.zones.FindIndexForPosition(warp->target_x, warp->target_y);
    if (target_zone_index >= ctx.zones.ZoneCount() ||
        ctx.zones.GetZone(target_zone_index).Id() != zone.Id()) {
        LOG_WARN("Warp {} target {}, {} leaves home zone {}; cross-zone warp is not enabled in M4",
                 warp->id,
                 warp->target_x,
                 warp->target_y,
                 zone.Id());
        return;
    }

    LOG_INFO("Session {} triggered warp {} in home zone {} from {}, {} to {}, {}",
             session_id,
             warp->id,
             zone.Id(),
             position.x,
             position.y,
             warp->target_x,
             warp->target_y);
    position.x = warp->target_x;
    position.y = warp->target_y;
}

} // namespace

void MovementSystem::Step(Zone& zone, float dt, ZoneTickContext& ctx)
{
    AssertZoneOwner(zone, "zone movement integration");

    // Two-phase iteration with only narrow const queries: wide query-each()
    // with an entity plus 6+ components crashes MSVC 14.51 (ICE), so handles
    // are collected first and components are read/updated per entity after.
    const float max_extent = ctx.terrain.WorldExtentMeters();

    std::vector<flecs::entity> players;
    players.reserve(static_cast<std::size_t>(zone.Diagnostics().player_count.load(std::memory_order_relaxed)));
    zone.World().query<const PlayerTag>().each([&](flecs::entity entity, const PlayerTag&) {
        players.push_back(entity);
    });

    std::uint64_t moved_entities = 0;
    auto note_moved = [&](const Position& before, const Position& after) {
        // Dirty-transform signal (§27): sub-centimeter moves (warp/clamp
        // rounding) don't count; real displacement does. Feeds the
        // dirty-vs-sent ratio in diagnostics; sending itself is unchanged
        // (wire protocol still takes full frames -- TODO).
        const float dx = after.x - before.x;
        const float dy = after.y - before.y;
        if (dx * dx + dy * dy > 0.0001f) {
            ++moved_entities;
        }
    };

    for (auto entity : players) {
        const auto net = entity.get<NetId>();
        const auto speed = entity.get<MoveSpeed>();
        auto position = entity.get<Position>();
        const Position before_move = position;
        auto heading = entity.get<Heading>();
        auto velocity = entity.get<Velocity>();
        auto intent = entity.get<MoveIntent>();
        const std::int64_t old_cell = SpatialCellKey(SpatialCellCoord(position.x),
                                                    SpatialCellCoord(position.y));

        const float move_speed = IntentSpeed(intent, speed);
        heading.angle = intent.dir_angle;
        velocity.x = std::sin(intent.dir_angle) * move_speed;
        velocity.y = std::cos(intent.dir_angle) * move_speed;
        velocity.z = 0.0f;

        const float dx = velocity.x * dt;
        const float dy = velocity.y * dt;
        const float next_x = std::clamp(position.x + dx, 0.0f, max_extent);
        if (ctx.terrain.IsWalkable(next_x, position.y)) {
            position.x = next_x;
        }

        const float next_y = std::clamp(position.y + dy, 0.0f, max_extent);
        if (ctx.terrain.IsWalkable(position.x, next_y)) {
            position.y = next_y;
        }

        gs::common::SessionId session_id = 0;
        if (const auto* binding = zone.FindPlayer(net.value)) {
            session_id = binding->session ? binding->session->Id() : 0;
        }
        TryApplyWarp(zone, ctx, position, session_id);
        position.z = ctx.terrain.SampleGroundHeight(position.x, position.y);
        note_moved(before_move, position);
        entity.set<Position>(position);
        entity.set<Heading>(heading);
        entity.set<Velocity>(velocity);
        entity.set<MoveIntent>(intent);
        zone.Grid().Move(net.value, old_cell, position);
        MigrationSystem::UpdateMarker(zone, ctx.zones, ctx.migration_queue, net.value, entity, position);
    }

    // Ghosts carry MobTag but no WanderState, so they are skipped explicitly
    // and this loop stays over authoritative mobs only.
    std::vector<flecs::entity> mobs;
    mobs.reserve(static_cast<std::size_t>(zone.Diagnostics().mob_count.load(std::memory_order_relaxed)));
    zone.World().query<const MobTag>().each([&](flecs::entity entity, const MobTag&) {
        if (!entity.has<GhostTag>()) {
            mobs.push_back(entity);
        }
    });

    // Simulation LOD: mobs integrate only when due, with dt scaled by the
    // tier period. Skipped ticks lose no time (the due tick integrates the
    // whole interval) and cause no teleports (velocity-bounded steps).
    // Players above always integrate every tick. Null/disabled config =
    // legacy behavior.
    const bool use_lod = ctx.lod != nullptr && ctx.lod->enabled;
    // LOD timebase: the zone's own tick counter (see SimulationLod.h).
    const std::uint32_t now_tick = zone.TickIndex();
    std::uint64_t integrated = 0;

    for (auto entity : mobs) {
        float dt_eff = dt;
        SimulationTier tier = SimulationTier::Full;
        if (use_lod) {
            if (!entity.has<SimulationLod>()) {
                // Strays integrate fully (safe direction); the validator
                // flags lod-less mobs so the creation path gets fixed.
                assert(false && "Movement on mob without SimulationLod");
            } else {
                const auto lod = entity.get<SimulationLod>();
                if (!LodSystem::IsDue(lod, now_tick)) {
                    continue;
                }
                tier = lod.tier;
                dt_eff = dt * static_cast<float>(LodPeriodTicks(tier, *ctx.lod));
            }
        }
        const auto net = entity.get<NetId>();
        const auto speed = entity.get<MoveSpeed>();
        const auto wander = entity.get<WanderState>();
        auto position = entity.get<Position>();
        const Position before_move = position;
        auto heading = entity.get<Heading>();
        auto velocity = entity.get<Velocity>();
        auto intent = entity.get<MoveIntent>();
        const std::int64_t old_cell = SpatialCellKey(SpatialCellCoord(position.x),
                                                    SpatialCellCoord(position.y));

        const float move_speed = IntentSpeed(intent, speed);
        heading.angle = intent.dir_angle;
        velocity.x = std::sin(intent.dir_angle) * move_speed;
        velocity.y = std::cos(intent.dir_angle) * move_speed;
        velocity.z = 0.0f;

        position.x = std::clamp(position.x + velocity.x * dt_eff, 0.0f, max_extent);
        position.y = std::clamp(position.y + velocity.y * dt_eff, 0.0f, max_extent);

        const float from_center_x = position.x - wander.spawn_center.x;
        const float from_center_y = position.y - wander.spawn_center.y;
        const float radius_sq = wander.spawn_radius * wander.spawn_radius;
        const float distance_sq = from_center_x * from_center_x + from_center_y * from_center_y;
        if (wander.spawn_radius > 0.0f && distance_sq > radius_sq) {
            const float distance = std::sqrt(distance_sq);
            position.x = wander.spawn_center.x + (from_center_x / distance) * wander.spawn_radius;
            position.y = wander.spawn_center.y + (from_center_y / distance) * wander.spawn_radius;
        }

        position.z = ctx.terrain.SampleGroundHeight(position.x, position.y);
        note_moved(before_move, position);
        entity.set<Position>(position);
        entity.set<Heading>(heading);
        entity.set<Velocity>(velocity);
        entity.set<MoveIntent>(intent);
        if (use_lod && entity.has<SimulationLod>()) {
            // Advance the entity's own schedule; the tier cannot have
            // changed under us (transitions happen in eval/promotion only).
            auto lod = entity.get<SimulationLod>();
            lod.next_tick = now_tick + LodPeriodTicks(lod.tier, *ctx.lod);
            entity.set<SimulationLod>(lod);
        }
        ++integrated;
        zone.Grid().Move(net.value, old_cell, position);
        MigrationSystem::UpdateMarker(zone, ctx.zones, ctx.migration_queue, net.value, entity, position);
    }
    zone.Diagnostics().transform_dirty_since_diag.fetch_add(moved_entities, std::memory_order_relaxed);
    zone.Diagnostics().lod_move_updates_since_diag.fetch_add(integrated, std::memory_order_relaxed);
}

} // namespace gs::game
