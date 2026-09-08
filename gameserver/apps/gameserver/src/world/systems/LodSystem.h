#pragma once

#include <cstdint>

#include <flecs.h>

#include "../components/SimulationLod.h"

// Simulation LOD evaluation + promotion. Decides per-entity tiers from
// player-proximity bubbles with hysteresis; the AI/movement loops only read
// the resulting schedule (no per-system tier branching beyond a due check).
//
// Threading: everything here runs under the zone's write guard (inside
// Zone::Tick for Evaluate, supervisor-side for Wake call sites that already
// hold ownership). No internal locking.
namespace gs::game {

class Zone;
struct ZoneTickContext;

class LodSystem {
public:
    // World ticks between full-zone relevance evaluations (1 Hz at 20 Hz).
    // Zones stagger by zone id so evaluations decluster across a pass.
    static constexpr int kEvalPeriodTicks = 20;

    // Recounts tiers for every mob in the zone (1 Hz, staggered). Pure
    // function of player positions + combat/migration state + hysteresis.
    // Also refreshes the lod_* tier gauges exactly (drift self-heals here).
    static void Evaluate(Zone& zone, ZoneTickContext& ctx);

    // Immediate promotion to Full (attack received, damage, script, ...).
    // Never waits for the next evaluation. Idempotent: re-stamps relevance
    // every call, counts a wake only on actual tier change.
    static void Wake(Zone& zone, flecs::entity entity, std::uint32_t now_tick);

    // Due for integration this tick? Dormant never is.
    static bool IsDue(const SimulationLod& lod, std::uint32_t now_tick) noexcept
    {
        return lod.tier != SimulationTier::Dormant && now_tick >= lod.next_tick;
    }

    // (Re-)arms the schedule after a tier assignment, phase-spread by net id
    // so Reduced/Low entities decluster across ticks instead of marching in
    // lockstep every period.
    static std::uint32_t ScheduleNext(SimulationTier tier,
                                      std::uint32_t now_tick,
                                      std::uint32_t net_id,
                                      const LodConfig& config) noexcept
    {
        if (tier == SimulationTier::Dormant) {
            return kLodNeverTick;
        }
        const std::uint32_t period = LodPeriodTicks(tier, config);
        return now_tick + (period > 1 ? net_id % period : 0u);
    }
};

} // namespace gs::game
