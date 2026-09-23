#include "Zone.h"

#include <cassert>
#include <exception>

#include "common/Logging.h"

#include "ZoneOwnership.h"
#include "../activity/SpatialActivityField.h"
#include "../systems/AiSystem.h"
#include "../systems/CooldownSystem.h"
#include "../systems/LodSystem.h"
#include "../systems/MovementSystem.h"
#include "../visibility/BorderPublisher.h"
#include "../visibility/GhostSystem.h"
#include "../replication/ReplicationSystem.h"
#include "../spawn/SpawnRandom.h"

namespace gs::game {

Zone::Zone(ZoneId id, std::string name, mx::map::Rect bounds)
    : id_(id)
    , name_(std::move(name))
    , bounds_(bounds)
{
    RegisterWorldComponents(world_);
    next_tick_ = std::chrono::steady_clock::now();
}

bool Zone::HasEntity(std::uint32_t net_id) const
{
    return entities_.find(net_id) != entities_.end();
}

flecs::entity Zone::FindEntity(std::uint32_t net_id) const
{
    const auto it = entities_.find(net_id);
    if (it == entities_.end()) {
        return flecs::entity();
    }
    return it->second;
}

void Zone::IndexEntity(std::uint32_t net_id, flecs::entity entity)
{
    // A live NetId must never be indexed twice: that would orphan the
    // previous handle and fork authority. Debug-checked; zero Release cost.
    assert(!HasEntity(net_id));
    entities_[net_id] = entity;
    // The resident set changed: the border publisher must do a full refill
    // and the ghost reconcile must not trust cached neighbor generations
    // (phase 5A incremental maintenance).
    ++entity_set_generation_;
}

void Zone::UnindexEntity(std::uint32_t net_id)
{
    entities_.erase(net_id);
    ++entity_set_generation_;
}

bool Zone::IsResident(std::uint32_t net_id) const
{
    return HasEntity(net_id);
}

Zone::PlayerBinding* Zone::FindPlayer(std::uint32_t net_id)
{
    const auto it = players_.find(net_id);
    return it != players_.end() ? &it->second : nullptr;
}

const Zone::PlayerBinding* Zone::FindPlayer(std::uint32_t net_id) const
{
    const auto it = players_.find(net_id);
    return it != players_.end() ? &it->second : nullptr;
}

Zone::PlayerBinding* Zone::FindPlayerBySession(gs::common::SessionId session_id)
{
    const auto net_it = net_by_session_.find(session_id);
    if (net_it == net_by_session_.end()) {
        return nullptr;
    }
    return FindPlayer(net_it->second);
}

void Zone::InsertPlayerBinding(std::uint32_t net_id, PlayerBinding binding)
{
    assert(players_.find(net_id) == players_.end());
    const auto session_id = binding.session ? binding.session->Id() : 0;
    if (session_id != 0) {
        net_by_session_[session_id] = net_id;
    }
    players_[net_id] = std::move(binding);
}

Zone::PlayerBinding Zone::ExtractPlayerBinding(std::uint32_t net_id)
{
    PlayerBinding binding;
    const auto it = players_.find(net_id);
    if (it != players_.end()) {
        binding = std::move(it->second);
        players_.erase(it);
    }
    for (auto session_it = net_by_session_.begin(); session_it != net_by_session_.end();) {
        if (session_it->second == net_id) {
            session_it = net_by_session_.erase(session_it);
        } else {
            ++session_it;
        }
    }
    return binding;
}

void Zone::ErasePlayerBinding(std::uint32_t net_id)
{
    ExtractPlayerBinding(net_id);
}

std::mt19937& Zone::MobRng(std::uint32_t net_id, std::uint32_t mob_type_id)
{
    const auto it = mob_rng_.find(net_id);
    if (it != mob_rng_.end()) {
        return it->second;
    }
    auto inserted = mob_rng_.emplace(net_id, std::mt19937(MakeMobSeed(net_id, mob_type_id)));
    return inserted.first->second;
}

void Zone::EraseMobRng(std::uint32_t net_id)
{
    mob_rng_.erase(net_id);
}

std::optional<std::mt19937> Zone::ExtractMobRng(std::uint32_t net_id)
{
    const auto it = mob_rng_.find(net_id);
    if (it == mob_rng_.end()) {
        return std::nullopt;
    }
    auto rng = std::move(it->second);
    mob_rng_.erase(it);
    return rng;
}

void Zone::InsertMobRng(std::uint32_t net_id, std::mt19937 rng)
{
    mob_rng_.insert_or_assign(net_id, std::move(rng));
}

void Zone::RefreshResidentCounts()
{
    diagnostics_.player_count.store(static_cast<std::uint32_t>(players_.size()),
                                    std::memory_order_relaxed);
    std::uint32_t mobs = 0;
    std::uint32_t moving = 0;
    world_.query<const MobTag, const WanderState>().each([&](const MobTag&, const WanderState& wander) {
        ++mobs;
        if (wander.mode == WanderState::Mode::Moving) {
            ++moving;
        }
    });
    diagnostics_.mob_count.store(mobs, std::memory_order_relaxed);
    diagnostics_.wandering_mob_count.store(moving, std::memory_order_relaxed);
    diagnostics_.idle_mob_count.store(mobs - moving, std::memory_order_relaxed);
}

void Zone::DrainCommands()
{
    AssertZoneOwner(*this, "zone command drain");
    auto commands = commands_.TakeAll();
    while (!commands.empty()) {
        auto command = std::move(commands.front());
        commands.pop();
        // A failing command must not kill the tick or wedge the zone: log
        // and continue with the rest. (Previously an exception here escaped
        // Tick and left tick_in_progress stuck.)
        try {
            command(*this);
        } catch (const std::exception& error) {
            LOG_ERROR("Zone {} ('{}') command failed: {}", id_, name_, error.what());
        } catch (...) {
            LOG_ERROR("Zone {} ('{}') command failed with unknown exception", id_, name_);
        }
    }
}

void Zone::Tick(float dt, ZoneTickContext& ctx)
{
    using Clock = std::chrono::steady_clock;
    const auto tick_start = Clock::now();
    // The flag is cleared on EVERY exit path below: a tick must never get
    // stuck "in progress", or the supervisor's migration/respawn/shutdown
    // waits would hang forever.
    try {
        ZoneWriteGuard guard(*this, "Zone::Tick");
        // Phase 5B: the world-global tick is the version domain for
        // transform replication (shared across zones, so migration never
        // invalidates a recipient's last-sent comparison).
        world_tick_ = ctx.world_tick;
        // Phase 5A: the dirty-publish list is per-tick scratch; commands may
        // have marked entities before the tick started, so drain only after
        // the previous tick consumed it (here, at the top).
        dirty_publish_entities_.clear();
        DrainCommands();

        // Simulation LOD evaluation (1 Hz, staggered by zone id so zones
        // decluster across passes). Recomputes tiers + tier gauges before
        // the gameplay steps of this tick consume them.
        if (ctx.lod != nullptr && ctx.lod->enabled &&
            ((zone_tick_ + id_) % LodSystem::kEvalPeriodTicks) == 0) {
            LodSystem::Evaluate(*this, ctx);
        }

        const auto gameplay_start = Clock::now();
        CooldownSystem::Step(*this, dt);
        const auto ai_start = Clock::now();
        AiSystem::StepWander(*this, dt, ctx.mob_types, ctx.lod);
        diagnostics_.ai_micros_since_diag.fetch_add(
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - ai_start)
                    .count()),
            std::memory_order_relaxed);
        const auto movement_start = Clock::now();
        MovementSystem::Step(*this, dt, ctx);
        diagnostics_.movement_micros_since_diag.fetch_add(
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - movement_start)
                    .count()),
            std::memory_order_relaxed);
        AssertZoneOwner(*this, "flecs world progress");
        world_.progress(dt);
        diagnostics_.gameplay_micros_since_diag.fetch_add(
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - gameplay_start)
                    .count()),
            std::memory_order_relaxed);

        // The supervisor sync gap before the next worker tick: this worker
        // only marks MigrateTo (and enqueues migration events); the
        // supervisor performs ownership transfer. Publish this zone's border
        // residents, then rebuild read-only ghosts from the previous tick
        // buffers of neighboring zones. The spatial index is maintained
        // incrementally by the systems above, so no rebuild happens here.
        const auto ghost_start = Clock::now();
        const auto border_publish_start = Clock::now();
        BorderPublisher::Publish(*this);
        diagnostics_.ghost_publish_micros_since_diag.fetch_add(
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() -
                                                                      border_publish_start)
                    .count()),
            std::memory_order_relaxed);
        // World-space activity publication (post-movement positions): this
        // zone's authoritative players for the cross-zone activity field.
        const auto activity_start = Clock::now();
        ActivityPublisher::Publish(*this);
        diagnostics_.activity_publish_micros_since_diag.fetch_add(
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() -
                                                                      activity_start)
                    .count()),
            std::memory_order_relaxed);
        if (players_.empty()) {
            GhostSystem::Clear(*this);
        } else {
            // Phase 5A: incremental reconcile (KEEP/ADD/REMOVE), never a full
            // rebuild on the production path.
            const auto reconcile_start = Clock::now();
            GhostSystem::Reconcile(*this, ctx.zones);
            diagnostics_.ghost_reconcile_micros_since_diag.fetch_add(
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() -
                                                                          reconcile_start)
                        .count()),
                std::memory_order_relaxed);
        }
        diagnostics_.ghost_micros_since_diag.fetch_add(
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - ghost_start)
                    .count()),
            std::memory_order_relaxed);

        if (!players_.empty()) {
            const auto repl_start = Clock::now();
            const auto records =
                ReplicationSystem::BroadcastTransforms(*this, ctx.send, ctx.replication);
            diagnostics_.transform_records_since_diag.fetch_add(records, std::memory_order_relaxed);
            diagnostics_.replication_micros_since_diag.fetch_add(
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - repl_start)
                        .count()),
                std::memory_order_relaxed);
        }
        // Continuous load field publication (post-everything positions): moves
        // this tick's touched load deltas into the zone's published buffer.
        const auto load_publish_start = Clock::now();
        LoadFieldPublisher::Publish(*this);
        diagnostics_.load_publish_micros_since_diag.fetch_add(
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() -
                                                                      load_publish_start)
                    .count()),
            std::memory_order_relaxed);
        diagnostics_.ticks_since_diag.fetch_add(1, std::memory_order_relaxed);
        ++zone_tick_;
        RefreshResidentCounts();
        // Phase 5D: publish this tick's spatial-index maintenance deltas.
        {
            const auto& maintenance = grid_.Maintenance();
            diagnostics_.grid_inserts_since_diag.fetch_add(
                maintenance.inserts - grid_maintenance_snapshot_.inserts,
                std::memory_order_relaxed);
            diagnostics_.grid_removes_since_diag.fetch_add(
                maintenance.removes - grid_maintenance_snapshot_.removes,
                std::memory_order_relaxed);
            diagnostics_.grid_moves_in_cell_since_diag.fetch_add(
                maintenance.moves_in_cell - grid_maintenance_snapshot_.moves_in_cell,
                std::memory_order_relaxed);
            diagnostics_.grid_moves_cell_since_diag.fetch_add(
                maintenance.moves_cell - grid_maintenance_snapshot_.moves_cell,
                std::memory_order_relaxed);
            grid_maintenance_snapshot_ = maintenance;
        }
    } catch (const std::exception& error) {
        LOG_ERROR("Zone {} ('{}') tick failed: {}", id_, name_, error.what());
    } catch (...) {
        LOG_ERROR("Zone {} ('{}') tick failed with unknown exception", id_, name_);
    }
    const auto tick_micros = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - tick_start).count());
    diagnostics_.tick_micros_since_diag.fetch_add(tick_micros, std::memory_order_relaxed);
    diagnostics_.RecordTickSample(tick_micros);
    tick_in_progress_.store(false, std::memory_order_release);
}

} // namespace gs::game
