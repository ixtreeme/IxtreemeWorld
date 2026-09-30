#include "MovementSystem.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <vector>

#include "common/Logging.h"

#include "../components/AiComponents.h"
#include "../components/MobComponents.h"
#include "../components/MovementComponents.h"
#include "../components/WarpState.h"
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

void TryApplyWarp(Zone& zone, ZoneTickContext& ctx, Position& position,
                  WarpState& state, float dt)
{
    AssertZoneOwner(zone, "zone warp application");
    auto& metrics = zone.TerrainDemand().Counters();
    state.cooldown_seconds = std::max(0.0f, state.cooldown_seconds - dt);
    const mx::map::WarpRegion* warp = nullptr;
    for (const auto& candidate : ctx.world_logic.warps) {
        if (candidate.source.ContainsHalfOpen(position.x, position.y)) {
            warp = &candidate;
            break;
        }
    }
    if (state.pending_id != 0 && (!warp || warp->id != state.pending_id)) {
        if(state.terrain_request) state.terrain_request->Cancel();
        state.terrain_request.reset();
        state.pending_id = 0;
        state.pending_seconds = 0;
        ++state.cancelled;
        ++metrics.warps_cancelled;
    }
    if (!warp) {
        if (state.cooldown_seconds == 0.0f) state.armed = true;
        return;
    }
    if (state.pending_id == 0) {
        if (!state.armed) return;
        state.armed = false;
        state.pending_id = warp->id;
        state.pending_seconds = 0;
    }
    state.pending_seconds += dt;
    if (state.pending_seconds > 5.0f) {
        if(state.terrain_request) state.terrain_request->Cancel();
        state.terrain_request.reset();
        state.pending_id = 0;
        ++state.timed_out;
        ++metrics.warps_timed_out;
        return;
    }
    if(state.terrain_request) {
        const auto status=state.terrain_request->Status();
        if(status!=TerrainRequestStatus::Pending && status!=TerrainRequestStatus::Ready) {
            state.pending_id=0;
            if(status==TerrainRequestStatus::TimedOut) {++state.timed_out; ++metrics.warps_timed_out;}
            else {++state.refused; ++metrics.warps_refused;}
            state.terrain_request.reset();
            return;
        }
    }
    const auto cell = ctx.terrain.Cell(warp->target_x, warp->target_y);
    const auto height = ctx.terrain.Height(warp->target_x, warp->target_y);
    if (cell.status == mx::map::TerrainStatus::NotResident ||
        height.status == mx::map::TerrainStatus::NotResident) {
        if(!state.terrain_request && ctx.prepare_terrain)
            state.terrain_request=ctx.prepare_terrain(warp->target_x,warp->target_y);
        zone.TerrainDemand().Add(ctx.terrain.ChunkIndexOf(warp->target_x, warp->target_y));
        metrics.warp_wait_seconds += dt;
        return;
    }
    const auto target = ctx.zones.FindIndexForPosition(warp->target_x, warp->target_y);
    state.pending_id = 0;
    if(state.terrain_request) state.terrain_request->Consume();
    state.terrain_request.reset();
    if (!cell.Walkable() || !height.Ok() || target >= ctx.zones.ZoneCount()) {
        // One refusal per entry, no tick-rate log flood or partial teleport.
        ++state.refused;
        ++metrics.warps_refused;
        return;
    }
    position = {warp->target_x, warp->target_y, height.meters};
    state.cooldown_seconds = 1.0f;
    state.transfer_pending = ctx.zones.GetZone(target).Id() != zone.Id();
    ++state.completed;
    ++metrics.warps_completed;
}
} // namespace

void MovementSystem::Step(Zone& zone, float dt, ZoneTickContext& ctx)
{
    AssertZoneOwner(zone, "zone movement integration");

    // Two-phase iteration with only narrow const queries: wide query-each()
    // with an entity plus 6+ components crashes MSVC 14.51 (ICE), so handles
    // are collected first and components are read/updated per entity after.
    // Out-of-world rule (MAP-2, R10): a step whose target is outside the
    // half-open world, or has no terrain data, is refused on that axis --
    // never clamped onto the edge. Height follows the terrain query status:
    // an unknown height keeps the previous z instead of becoming 0 m.
    auto ground_z = [&ctx](const Position& p, float previous_z) {
        const auto height = ctx.terrain.Height(p.x, p.y);
        return height.Ok() ? height.meters : previous_z;
    };

    // MAP-3: terrain demand (streaming worlds) and query outcome counters.
    // A step's whole path is checked (collision across chunk borders, not
    // just the destination sample); a path over a chunk that is not loaded
    // is refused THIS tick and the chunk demanded -- the entity waits in
    // place (bounded by the load latency), nothing is guessed.
    const bool streaming = ctx.terrain.Streaming();
    const float lookahead = streaming ? ctx.terrain.LookaheadSeconds() : 0.0f;
    auto& demand = zone.TerrainDemand();
    auto& counters = demand.Counters();
    auto try_step = [&](float from_x, float from_y, float to_x, float to_y) {
        ++counters.steps_attempted;
        const StepCheck step = ctx.terrain.CheckStep(from_x, from_y, to_x, to_y);
        switch (step.result) {
        case StepResult::Clear:
            ++counters.ok;
            return true;
        case StepResult::NotResident:
            ++counters.not_resident;
            ++counters.steps_waiting;
            demand.Add(step.chunk);
            return false;
        case StepResult::InvalidData:
            // Permanently unusable data (chunk published invalid): refused,
            // not "waiting" -- no demand, nothing will arrive.
            ++counters.invalid;
            return false;
        case StepResult::OutsideWorld:
            ++counters.outside;
            return false;
        case StepResult::Blocked:
        case StepResult::TooSteep:
        case StepResult::DeepWater:
            ++counters.steps_blocked;
            return false;
        }
        return false;
    };

    std::vector<flecs::entity> players;
    players.reserve(static_cast<std::size_t>(zone.Diagnostics().player_count.load(std::memory_order_relaxed)));
    zone.World().query<const PlayerTag>().each([&](flecs::entity entity, const PlayerTag&) {
        players.push_back(entity);
    });

    std::uint64_t moved_entities = 0;
    auto note_moved = [&](flecs::entity entity, const Position& before, const Position& after) {
        // Dirty-transform signal (§27): sub-centimeter moves (warp/clamp
        // rounding) don't count; real displacement does. Feeds the
        // dirty-vs-sent ratio in diagnostics; sending itself is unchanged
        // (wire protocol still takes full frames -- TODO).
        const float dx = after.x - before.x;
        const float dy = after.y - before.y;
        if (dx * dx + dy * dy > 0.0001f) {
            ++moved_entities;
            // Phase 5A: only entities that actually changed need their border
            // snapshot re-evaluated (no full resident scan per tick).
            zone.MarkEntityDirty(entity);
            // Load field attribution: a dirty transform is replication
            // pressure in waiting; binned at the destination position.
            if (auto* load = zone.LoadBins().CellFor(after.x, after.y)) {
                ++load->repl_dirty;
            }
        }
    };

    for (auto entity : players) {
        const auto net = entity.get<NetId>();
        const auto speed = entity.get<MoveSpeed>();
        auto position = entity.get<Position>();
        auto warp_state = entity.has<WarpState>() ? entity.get<WarpState>() : WarpState{};
        if (warp_state.transfer_pending) {
            if (ctx.zones.FindIndexForPosition(position.x, position.y) != ctx.zones.FindIndexById(zone.Id())) {
                MigrationSystem::UpdateMarker(zone, ctx.zones, ctx.migration_queue, net.value, entity, position);
                continue; // freeze the validated landing point until ownership commits
            }
            warp_state.transfer_pending = false;
        }
        const Position before_move = position;
        auto heading = entity.get<Heading>();
        const float before_heading_angle = heading.angle;
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
        if (streaming) {
            // A player keeps its chunk resident even when standing still; a
            // moving one also pre-fetches where it will be one load latency
            // (x3, see TerrainStreamer::LookaheadSeconds) from now.
            demand.Add(ctx.terrain.ChunkIndexOf(position.x, position.y));
            if (move_speed > 0.0f) {
                demand.Add(ctx.terrain.ChunkIndexOf(position.x + velocity.x * lookahead,
                                                    position.y + velocity.y * lookahead),TerrainPriority::Prefetch);
            }
        }
        // Per axis: a step whose path is not clear (outside, not resident,
        // blocked, too steep, deep water) is refused on that axis -- never
        // clamped onto an edge (R10); the other axis may still move.
        const float next_x = position.x + dx;
        if (dx != 0.0f && try_step(position.x, position.y, next_x, position.y)) {
            position.x = next_x;
        }

        const float next_y = position.y + dy;
        if (dy != 0.0f && try_step(position.x, position.y, position.x, next_y)) {
            position.y = next_y;
        }

        TryApplyWarp(zone, ctx, position, warp_state, dt);
        entity.set<WarpState>(warp_state);
        position.z = ground_z(position, before_move.z);
        note_moved(entity, before_move, position);
        // Phase 5B: the replicated transform (position/heading/move_state)
        // changed -> stamp the version the dirty replication compares against.
        if (before_move.x != position.x || before_move.y != position.y ||
            before_move.z != position.z || before_heading_angle != heading.angle) {
            zone.NoteTransformChanged(entity);
        }
        // Load field attribution: one movement integration happened HERE.
        if (auto* load = zone.LoadBins().CellFor(position.x, position.y)) {
            ++load->sim_work;
        }
        zone.CommitPlayerPosition(entity, net.value, position);
        entity.set<Heading>(heading);
        entity.set<Velocity>(velocity);
        entity.set<MoveIntent>(intent);
        zone.Grid().Move(entity, net.value, old_cell, position);
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
    //
    // Low-tier substep audit (§27): the slowest cadence is Low at 1 Hz, so
    // dt_eff peaks at ~1.0s. At wander speed (1.5 m/s) that is a 1.5m step:
    // far below the 5m migration hysteresis, the 120m AOI radius and the
    // 500m activity cells, so nothing tunnels or skips a boundary band.
    // Border-crossing detection (UpdateMarker below) runs on every
    // integration, hence at worst one Low period late — bounded, not lost.
    // Splitting AI decisions from movement substeps (e.g. 1 Hz decisions
    // with 50-100ms integration) is deliberately NOT done now: the seam
    // exists structurally (intent persists across skipped ticks), but the
    // current speed scales need no finer integration.
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
        const float before_heading_angle = heading.angle;
        auto velocity = entity.get<Velocity>();
        auto intent = entity.get<MoveIntent>();
        const std::int64_t old_cell = SpatialCellKey(SpatialCellCoord(position.x),
                                                    SpatialCellCoord(position.y));

        const float move_speed = IntentSpeed(intent, speed);
        heading.angle = intent.dir_angle;
        velocity.x = std::sin(intent.dir_angle) * move_speed;
        velocity.y = std::cos(intent.dir_angle) * move_speed;
        velocity.z = 0.0f;

        // Candidate step, pulled back onto the wander leash (a gameplay
        // rule), then accepted only if its whole path is clear: inside the
        // world, with terrain data, no blocked cell (MAP-3: the blocking grid
        // applies to mobs too; a large low-LOD step cannot tunnel through a
        // one-cell wall), within the slope / water rules.
        if (streaming) {
            demand.Add(ctx.terrain.ChunkIndexOf(position.x, position.y));
        }
        Position next = position;
        next.x = position.x + velocity.x * dt_eff;
        next.y = position.y + velocity.y * dt_eff;
        const float from_center_x = next.x - wander.spawn_center.x;
        const float from_center_y = next.y - wander.spawn_center.y;
        const float radius_sq = wander.spawn_radius * wander.spawn_radius;
        const float distance_sq = from_center_x * from_center_x + from_center_y * from_center_y;
        if (wander.spawn_radius > 0.0f && distance_sq > radius_sq) {
            const float distance = std::sqrt(distance_sq);
            next.x = wander.spawn_center.x + (from_center_x / distance) * wander.spawn_radius;
            next.y = wander.spawn_center.y + (from_center_y / distance) * wander.spawn_radius;
        }
        if ((next.x != position.x || next.y != position.y) &&
            try_step(position.x, position.y, next.x, next.y)) {
            position.x = next.x;
            position.y = next.y;
        }

        position.z = ground_z(position, before_move.z);
        note_moved(entity, before_move, position);
        // Phase 5B: replicated transform changed -> stamp the version.
        if (before_move.x != position.x || before_move.y != position.y ||
            before_move.z != position.z || before_heading_angle != heading.angle) {
            zone.NoteTransformChanged(entity);
        }
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
        // Load field attribution: one movement integration happened HERE.
        if (auto* load = zone.LoadBins().CellFor(position.x, position.y)) {
            ++load->sim_work;
        }
        ++integrated;
        zone.Grid().Move(entity, net.value, old_cell, position);
        MigrationSystem::UpdateMarker(zone, ctx.zones, ctx.migration_queue, net.value, entity, position);
    }
    // This tick's terrain demand + query outcomes become visible to the
    // supervisor (streamer aggregation, metrics).
    demand.Publish();
    zone.Diagnostics().transform_dirty_since_diag.fetch_add(moved_entities, std::memory_order_relaxed);
    zone.Diagnostics().lod_move_updates_since_diag.fetch_add(integrated, std::memory_order_relaxed);
}

} // namespace gs::game
