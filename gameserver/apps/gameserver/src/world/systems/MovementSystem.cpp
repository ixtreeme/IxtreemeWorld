#include "MovementSystem.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <vector>

#include "common/Logging.h"

#include "../components/AiComponents.h"
#include "../components/LayerComponents.h"
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

// ---- 3D-5A layered movement ------------------------------------------------
// A layered entity's authoritative pose is (volume, x, y); z is derived from
// the volume's cooked support plane. Targets are float positions passed to
// the shared contract exactly (float -> double is exact), so the committed
// point is bit-identical to what was checked for clearance.

const mx::map::LayerVolume* FindLayerVolume(const mx::map::LayeredWorld& world, std::uint32_t id)
{
    for (const auto& volume : world.volumes) {
        if (volume.id == id) {
            return &volume;
        }
    }
    return nullptr;
}

bool InsideFootprint(const mx::map::LayerVolume& volume, double x, double y)
{
    return x >= volume.bounds.min_x && x < volume.bounds.max_x && y >= volume.bounds.min_y &&
           y < volume.bounds.max_y;
}

// One move from `from` to (to_x, to_y). Target volume: the current one when
// its footprint holds the point, else the far end of a proven portal of the
// current volume whose footprint holds it (the shared contract re-checks the
// proof, the crossing and both parts of the segment). Leaving the volume
// system is the caller's separate terrain-edge check (3D-5B2).
mx::map::LayerGroundResult TryLayeredMove(const mx::map::LayeredWorld& world,
                                          const mx::map::LayerActorProfile& actor,
                                          const LayerPresence& presence,
                                          const Position& from,
                                          float to_x,
                                          float to_y)
{
    mx::map::LayerGroundResult result;
    const auto* volume = FindLayerVolume(world, presence.volume_id);
    if (volume == nullptr || !volume->HasValidGroundSupport()) {
        result.status = mx::map::GroundSupportStatus::InvalidState;
        return result;
    }
    mx::map::LayerGroundState current{presence.volume_id, presence.layer_id,
                                      static_cast<double>(from.x), static_cast<double>(from.y), 0.0};
    current.z = volume->ground_support->Height(current.x, current.y);
    const double x = static_cast<double>(to_x);
    const double y = static_cast<double>(to_y);
    if (InsideFootprint(*volume, x, y)) {
        return mx::map::ResolveLayerActorMove(world, actor, current, volume->id, x, y);
    }
    result.status = mx::map::GroundSupportStatus::TransitionRequired;
    result.state = current;
    for (const auto& portal : world.portals) {
        if (!portal.proof) {
            continue;
        }
        std::uint32_t other = 0;
        if (portal.source_volume == volume->id) {
            other = portal.target_volume;
        } else if (portal.target_volume == volume->id) {
            other = portal.source_volume;
        }
        const auto* neighbour = other != 0 ? FindLayerVolume(world, other) : nullptr;
        if (neighbour == nullptr || !InsideFootprint(*neighbour, x, y)) {
            continue;
        }
        auto candidate = mx::map::ResolveLayerActorMove(world, actor, current, other, x, y);
        if (candidate.Ok()) {
            return candidate;
        }
        result = candidate;
    }
    return result;
}

// Longest straight piece checked as one move: shorter than any standable
// stair tread (radius + cell), so a single piece crosses at most one portal.
constexpr float kLayeredSubStepMeters = 0.5f;

// 3D-5B2: the terrain side of a proven terrain-edge crossing, checked against
// the authoritative (resident) terrain: the terrain path between the edge
// point and the terrain end of the move is clear, and the terrain at the
// crossing is within the actor's step of the support plane. A chunk that is
// not resident is demanded and the crossing waits (never guessed).
bool TerrainSideAccepts(Zone& zone, ZoneTickContext& ctx, const mx::map::LayerTerrainCrossing& crossing,
                        float terrain_x, float terrain_y)
{
    const auto cx = static_cast<float>(crossing.x);
    const auto cy = static_cast<float>(crossing.y);
    const StepCheck path = ctx.terrain.CheckStep(cx, cy, terrain_x, terrain_y);
    if (path.result == StepResult::NotResident) {
        zone.TerrainDemand().Add(path.chunk);
    }
    if (path.result != StepResult::Clear) {
        return false;
    }
    const auto height = ctx.terrain.Height(cx, cy);
    return height.Ok() && std::abs(static_cast<double>(height.meters) - crossing.plane_z) <= crossing.step_m;
}

// Moves along the straight line to (to_x, to_y) in sub-steps; stops at the
// last accepted point. Returns true when the pose changed. Outcomes are
// counted per refused/accepted piece. A piece that leaves the volume across
// a proven terrain edge (and whose terrain side the terrain accepts) ends
// the layered presence: `presence.volume_id` becomes 0 and z is the terrain.
bool StepLayered(Zone& zone,
                 ZoneTickContext& ctx,
                 LayerPresence& presence,
                 Position& position,
                 float to_x,
                 float to_y)
{
    auto& diag = zone.Diagnostics();
    if (ctx.layered == nullptr) {
        diag.layered_moves_invalid_total.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const mx::map::LayeredWorld& world = *ctx.layered;
    const mx::map::LayerActorProfile& actor = ctx.layer_actor;
    const Position start = position;
    const float dx = to_x - start.x;
    const float dy = to_y - start.y;
    const float length = std::sqrt(dx * dx + dy * dy);
    const int pieces = std::max(1, static_cast<int>(std::ceil(length / kLayeredSubStepMeters)));
    bool moved = false;
    for (int i = 1; i <= pieces; ++i) {
        const float fraction = static_cast<float>(i) / static_cast<float>(pieces);
        const float px = i == pieces ? to_x : start.x + dx * fraction;
        const float py = i == pieces ? to_y : start.y + dy * fraction;
        const auto step = TryLayeredMove(world, actor, presence, position, px, py);
        if (!step.Ok() && step.status == mx::map::GroundSupportStatus::TransitionRequired) {
            // Leaving the volume: only across a proven terrain edge.
            const auto* volume = FindLayerVolume(world, presence.volume_id);
            if (volume != nullptr && volume->HasValidGroundSupport()) {
                mx::map::LayerGroundState current{presence.volume_id, presence.layer_id,
                                                  static_cast<double>(position.x), static_cast<double>(position.y), 0.0};
                current.z = volume->ground_support->Height(current.x, current.y);
                const auto exit = mx::map::ResolveLayerActorExitToTerrain(world, actor, current, px, py);
                if (exit.Ok()) {
                    const auto ground = ctx.terrain.Height(px, py);
                    if (ground.Ok() && TerrainSideAccepts(zone, ctx, exit, px, py)) {
                        position = {px, py, ground.meters};
                        presence = {};
                        diag.layered_terrain_exits_total.fetch_add(1, std::memory_order_relaxed);
                        return true;
                    }
                    diag.layered_moves_blocked_total.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                if (exit.status == mx::map::GroundSupportStatus::Blocked) {
                    diag.layered_moves_blocked_total.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            }
        }
        if (!step.Ok()) {
            switch (step.status) {
            case mx::map::GroundSupportStatus::Blocked:
            case mx::map::GroundSupportStatus::OutsideVolume:
                diag.layered_moves_blocked_total.fetch_add(1, std::memory_order_relaxed);
                break;
            case mx::map::GroundSupportStatus::TransitionRequired:
                diag.layered_moves_transition_total.fetch_add(1, std::memory_order_relaxed);
                break;
            default:
                diag.layered_moves_invalid_total.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            break;
        }
        position = {px, py, static_cast<float>(step.state.z)};
        presence = {step.state.volume_id, step.state.layer_id};
        diag.layered_moves_ok_total.fetch_add(1, std::memory_order_relaxed);
        if (step.portal_id != 0) {
            diag.layered_portal_crossings_total.fetch_add(1, std::memory_order_relaxed);
        }
        moved = true;
    }
    return moved;
}

// 3D-5B2: a terrain player whose step enters a volume across a proven terrain
// edge becomes layered on it (z = the support plane at the target). False =
// no entry; the caller continues with the terrain step.
bool TryEnterFromTerrain(Zone& zone, ZoneTickContext& ctx, LayerPresence& presence, Position& position,
                         float to_x, float to_y)
{
    if (ctx.layered == nullptr || ctx.layered->terrain_edges.empty()) {
        return false;
    }
    const auto entry = mx::map::ResolveLayerActorEnterFromTerrain(*ctx.layered, ctx.layer_actor, position.x,
                                                                  position.y, to_x, to_y);
    if (!entry.Ok() || !TerrainSideAccepts(zone, ctx, entry, position.x, position.y)) {
        return false;
    }
    position = {to_x, to_y, static_cast<float>(entry.inside.z)};
    presence = {entry.volume_id, entry.layer_id};
    zone.Diagnostics().layered_terrain_entries_total.fetch_add(1, std::memory_order_relaxed);
    return true;
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
        // Per axis: a step whose path is not clear is refused on that axis --
        // never clamped onto an edge (R10); the other axis may still move.
        // A layered entity (3D-5A) moves through the cooked clearance /
        // proven-portal contract with z on the support plane and may leave
        // to the terrain across a proven terrain edge; a terrain player may
        // enter a volume across one (3D-5B2). Terrain warps apply only to an
        // entity that is on the terrain for the whole tick.
        const auto* presence_ptr = entity.try_get<LayerPresence>();
        const LayerPresence original = presence_ptr != nullptr ? *presence_ptr : LayerPresence{};
        LayerPresence presence = original;
        auto axis_step = [&](float to_x, float to_y) {
            if (presence.volume_id != 0) {
                StepLayered(zone, ctx, presence, position, to_x, to_y);
                return;
            }
            if (TryEnterFromTerrain(zone, ctx, presence, position, to_x, to_y)) {
                return;
            }
            if (try_step(position.x, position.y, to_x, to_y)) {
                position.x = to_x;
                position.y = to_y;
            }
        };
        if (dx != 0.0f) {
            axis_step(position.x + dx, position.y);
        }
        if (dy != 0.0f) {
            axis_step(position.x, position.y + dy);
        }
        if (original.volume_id == 0 && presence.volume_id == 0) {
            TryApplyWarp(zone, ctx, position, warp_state, dt);
            entity.set<WarpState>(warp_state);
        }
        if (presence.volume_id == 0) {
            position.z = ground_z(position, before_move.z);
        }
        if (presence.volume_id != original.volume_id || presence.layer_id != original.layer_id) {
            if (presence.volume_id != 0) {
                entity.set<LayerPresence>(presence);
            } else {
                entity.remove<LayerPresence>();
            }
        }
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
        zone.Grid().Move(entity, net.value, old_cell, position, SpatialVolumeOf(entity));
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
        if (const auto* presence_ptr = entity.try_get<LayerPresence>()) {
            // 3D-5A layered mob: the whole (possibly low-LOD, long) step is
            // split into pieces of at most one portal each.
            // It may leave to the terrain across a proven terrain edge; terrain
            // mobs never enter volumes (3D-5B2 admits players only).
            const LayerPresence original = *presence_ptr;
            LayerPresence presence = original;
            if (next.x != position.x || next.y != position.y) {
                StepLayered(zone, ctx, presence, position, next.x, next.y);
            }
            if (presence.volume_id == 0) {
                entity.remove<LayerPresence>();
            } else if (presence.volume_id != original.volume_id || presence.layer_id != original.layer_id) {
                entity.set<LayerPresence>(presence);
            }
        } else {
            if ((next.x != position.x || next.y != position.y) &&
                try_step(position.x, position.y, next.x, next.y)) {
                position.x = next.x;
                position.y = next.y;
            }

            position.z = ground_z(position, before_move.z);
        }
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
        zone.Grid().Move(entity, net.value, old_cell, position, SpatialVolumeOf(entity));
        MigrationSystem::UpdateMarker(zone, ctx.zones, ctx.migration_queue, net.value, entity, position);
    }
    // This tick's terrain demand + query outcomes become visible to the
    // supervisor (streamer aggregation, metrics).
    demand.Publish();
    zone.Diagnostics().transform_dirty_since_diag.fetch_add(moved_entities, std::memory_order_relaxed);
    zone.Diagnostics().lod_move_updates_since_diag.fetch_add(integrated, std::memory_order_relaxed);
}

} // namespace gs::game
