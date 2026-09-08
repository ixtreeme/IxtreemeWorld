#include "LodSystem.h"

#include <cassert>
#include <cfloat>
#include <cstdint>
#include <vector>

#include "../WorldConstants.h"
#include "../components/CombatComponents.h"
#include "../components/MigrationComponents.h"
#include "../components/NetworkComponents.h"
#include "../components/Tags.h"
#include "../components/TransformComponents.h"
#include "../zone/Zone.h"
#include "../zone/ZoneOwnership.h"

namespace gs::game {

static_assert(kTickDt.count() == 50, "LOD tick math assumes a 20 Hz simulation clock");
static_assert(std::is_standard_layout_v<SimulationLod>);

namespace {

std::uint32_t TicksSince(std::uint32_t now_tick, std::uint32_t past_tick) noexcept
{
    // Unsigned wrap is well-defined; the half-range guard keeps a
    // once-per-136-years wrap from collapsing a grace window.
    return now_tick >= past_tick ? now_tick - past_tick : 0u;
}

std::uint32_t GraceTicks(float grace_sec) noexcept
{
    if (!(grace_sec > 0.0f)) {
        return 0u;
    }
    const float ticks = grace_sec * static_cast<float>(kLodTicksPerSecond);
    return ticks > 0.0f ? static_cast<std::uint32_t>(ticks) : 0u;
}

} // namespace

void LodSystem::Wake(Zone& zone, flecs::entity entity, std::uint32_t now_tick)
{
    if (!entity.is_valid() || entity.has<PlayerTag>() || entity.has<GhostTag>()) {
        return;
    }
    if (!entity.has<SimulationLod>()) {
        // Never fabricate here: the debug validator flags lod-less mobs
        // loudly so missing creation paths get fixed instead of masked.
        assert(false && "LodSystem::Wake on mob without SimulationLod");
        return;
    }
    auto lod = entity.get<SimulationLod>();
    if (lod.tier != SimulationTier::Full) {
        lod.tier = SimulationTier::Full;
        zone.Diagnostics().lod_wakes_since_diag.fetch_add(1, std::memory_order_relaxed);
    }
    lod.next_tick = now_tick;
    lod.awake_tick = now_tick;
    entity.set<SimulationLod>(lod);
}

void LodSystem::Evaluate(Zone& zone, ZoneTickContext& ctx)
{
    AssertZoneOwner(zone, "zone lod evaluation");
    const LodConfig& config = *ctx.lod; // caller guarantees non-null + enabled
    // LOD timebase: the zone's own tick counter (see SimulationLod.h).
    const std::uint32_t now = zone.TickIndex();
    const auto eval_start = std::chrono::steady_clock::now();

    // Player bubble centers: authoritative residents only. Zones are small
    // and player counts per zone are low, so this snapshot is trivial.
    struct Bubble {
        float x, y;
    };
    std::vector<Bubble> players;
    players.reserve(static_cast<std::size_t>(zone.Diagnostics().player_count.load(std::memory_order_relaxed)));
    for (const auto& [net_id, binding] : zone.Players()) {
        (void)binding;
        const auto player = zone.FindEntity(net_id);
        if (player.is_valid() && player.has<Position>()) {
            const auto pos = player.get<Position>();
            players.push_back(Bubble{pos.x, pos.y});
        }
    }

    const float full_sq = config.full_radius_m * config.full_radius_m;
    const float reduced_sq = config.reduced_radius_m * config.reduced_radius_m;
    const float low_sq = config.low_radius_m * config.low_radius_m;
    const std::uint32_t grace_full = GraceTicks(config.demote_full_sec);
    const std::uint32_t grace_reduced = GraceTicks(config.demote_reduced_sec);
    const std::uint32_t grace_low = GraceTicks(config.demote_low_sec);

    std::uint32_t n_full = 0;
    std::uint32_t n_reduced = 0;
    std::uint32_t n_low = 0;
    std::uint32_t n_dormant = 0;

    // Direct nearest-player scan at 1 Hz per zone (deliberately NOT a
    // per-tick global scan: 1000x cheaper than the 20 Hz strawman, and exact
    // with no derived-state invalidation. A cell-aggregated variant can
    // replace this loop body later without touching callers.)
    zone.World().query<const MobTag>().each([&](flecs::entity entity, const MobTag&) {
        if (entity.has<GhostTag>()) {
            return;
        }
        if (!entity.has<SimulationLod>() || !entity.has<Position>() || !entity.has<NetId>()) {
            assert(false && "LOD evaluation on mob without lod/position/identity");
            return;
        }
        auto lod = entity.get<SimulationLod>();
        const auto pos = entity.get<Position>();
        const std::uint32_t net = entity.get<NetId>().value;

        SimulationTier desired = SimulationTier::Dormant;
        if (!players.empty()) {
            float best_sq = FLT_MAX;
            for (const auto& bubble : players) {
                const float dx = pos.x - bubble.x;
                const float dy = pos.y - bubble.y;
                const float d_sq = dx * dx + dy * dy;
                if (d_sq < best_sq) {
                    best_sq = d_sq;
                    if (best_sq < full_sq) {
                        break;
                    }
                }
            }
            desired = best_sq < full_sq
                          ? SimulationTier::Full
                          : best_sq < reduced_sq ? SimulationTier::Reduced
                                                : best_sq < low_sq ? SimulationTier::Low
                                                                   : SimulationTier::Dormant;
        }
        // Overrides: combat recency and pending migration pin Full
        // regardless of distance. (Mobs never initiate attacks in this
        // phase; incoming-attack promotion arrives via Wake + awake_tick.)
        if (entity.has<MigrateTo>() && entity.get<MigrateTo>().target_zone != 0) {
            desired = SimulationTier::Full;
        }
        if (entity.has<AttackCooldown>() && entity.get<AttackCooldown>().remaining > 0.0f) {
            desired = SimulationTier::Full;
        }

        // NOTE on ordering: lower enum value = HIGHER relevance
        // (Full=0 is most relevant). Promotion moves toward Full (<),
        // demotion cascades toward Dormant (>), one level per eligible
        // grace window.
        auto& diag = zone.Diagnostics();
        if (desired < lod.tier) {
            // Promotion is immediate and may skip levels.
            lod.tier = desired;
            lod.awake_tick = now;
            lod.next_tick = ScheduleNext(lod.tier, now, net, config);
            diag.lod_promotions_since_diag.fetch_add(1, std::memory_order_relaxed);
        } else if (desired == lod.tier) {
            lod.awake_tick = now; // sustained relevance restarts the grace clock
        } else if (lod.tier != SimulationTier::Dormant) {
            // One-level demotion cascade per eligible grace window.
            const std::uint32_t grace = lod.tier == SimulationTier::Full
                                            ? grace_full
                                            : lod.tier == SimulationTier::Reduced ? grace_reduced
                                                                                  : grace_low;
            if (TicksSince(now, lod.awake_tick) >= grace) {
                lod.tier = static_cast<SimulationTier>(static_cast<std::uint8_t>(lod.tier) + 1);
                lod.next_tick = ScheduleNext(lod.tier, now, net, config);
                diag.lod_demotions_since_diag.fetch_add(1, std::memory_order_relaxed);
            }
        }
        entity.set<SimulationLod>(lod);

        switch (lod.tier) {
        case SimulationTier::Full:
            ++n_full;
            break;
        case SimulationTier::Reduced:
            ++n_reduced;
            break;
        case SimulationTier::Low:
            ++n_low;
            break;
        case SimulationTier::Dormant:
            ++n_dormant;
            break;
        }
    });

    // Exact recount: any drift (spawn/migration inserts bump counters
    // synchronously for sleep decisions; deaths/despawns go stale-high)
    // self-heals here within one evaluation period.
    auto& diag = zone.Diagnostics();
    diag.lod_full.store(n_full, std::memory_order_relaxed);
    diag.lod_reduced.store(n_reduced, std::memory_order_relaxed);
    diag.lod_low.store(n_low, std::memory_order_relaxed);
    diag.lod_dormant.store(n_dormant, std::memory_order_relaxed);
    diag.lod_eval_us_since_diag.fetch_add(
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                                  eval_start)
                .count()),
        std::memory_order_relaxed);
}

} // namespace gs::game
