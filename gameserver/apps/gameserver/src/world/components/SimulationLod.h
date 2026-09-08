#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Server-side simulation LOD (compute optimization, NOT graphics LOD).
// An entity's tier decides HOW OFTEN its AI/movement integrate; gameplay
// stays transparent because integration always scales with the elapsed
// interval (no teleports) and promotion is immediate.
//
// Timebase: the owning zone's tick counter (uint32, 20 Hz per simulating
// zone). All LOD timestamps live in this clock, never wall time and never
// the global world tick (whose phase races the per-zone 50 ms deadline and
// would randomly skip scheduled integrations). Deterministic under load;
// a starved zone merely experiences grace windows in slow motion (safe).
//
// Lifetime: emplaced at mob spawn (Full, no grace history), carried across
// migration/split/merge by EntityTransfer, never present on players
// (implicit Full) or ghosts (read-only representations).
namespace gs::game {

// Ticks per second of the simulation clock. Kept next to the component so
// grace/period math never depends on wall time. Must match kTickDt (50ms);
// enforced by static_assert in LodSystem.cpp.
inline constexpr int kLodTicksPerSecond = 20;
// Dormant entities never schedule: sentinel deadline, checked by name.
inline constexpr std::uint32_t kLodNeverTick = 0xFFFFFFFFu;

// ORDERING INVARIANT: lower value = HIGHER relevance (Full is most
// relevant). Promotion compares desired < tier, demotion desired > tier.
// Never reorder without updating LodSystem::Evaluate.
enum class SimulationTier : std::uint8_t {
    Full = 0,    // every tick (20 Hz): players, combat, nearby
    Reduced = 1, // every Nth tick (default 10 Hz): mid relevance
    Low = 2,     // every Mth tick (default 1 Hz): distant but alive
    Dormant = 3, // event-driven only: no per-tick simulation at all
};

struct SimulationLod {
    SimulationTier tier = SimulationTier::Full;
    // Next world tick this entity integrates. Advanced by the integrator,
    // re-armed on every tier transition. kLodNeverTick while Dormant.
    std::uint32_t next_tick = 0;
    // Last world tick the entity was deemed Full-relevant (promotion,
    // sustained desire, spawn proximity). Demotion graces count from here,
    // which yields per-level cascade hysteresis without extra fields.
    std::uint32_t awake_tick = 0;
};

// Generic future seam (§29): policies key off entity KIND, never hardcode
// Player/Mob call sites. Ships/bosses later add a kind + radii here.
enum class SimulationKind : std::uint8_t {
    Mob = 0,
};

struct LodConfig {
    bool enabled = true;
    // Activation bubbles: entity takes the HIGHEST tier any player grants.
    float full_radius_m = 150.0f;
    float reduced_radius_m = 500.0f;
    float low_radius_m = 1500.0f;
    // Effective frequencies (period = round(20 / hz) ticks).
    float reduced_hz = 10.0f;
    float low_hz = 1.0f;
    // Demotion graces in seconds (per-level cascade, conservative sum).
    float demote_full_sec = 5.0f;
    float demote_reduced_sec = 30.0f;
    float demote_low_sec = 60.0f;
};

struct ValidatedLodConfig {
    LodConfig effective;
    std::vector<std::string> warnings;
};

// Pure: enforces 0 < full < reduced < low radii, hz > 0, grace >= 0.
// Warn + fall back per field (same philosophy as partition config).
inline ValidatedLodConfig ValidateLodConfig(const LodConfig& in)
{
    ValidatedLodConfig out;
    out.effective = in;

    if (!(out.effective.full_radius_m > 0.0f)) {
        out.warnings.emplace_back("simulation: full_radius_m must be > 0, using 150");
        out.effective.full_radius_m = 150.0f;
    }
    if (!(out.effective.reduced_radius_m > out.effective.full_radius_m)) {
        out.warnings.emplace_back("simulation: reduced_radius_m must exceed full_radius_m, using 500");
        out.effective.reduced_radius_m = 500.0f;
    }
    if (!(out.effective.low_radius_m > out.effective.reduced_radius_m)) {
        out.warnings.emplace_back("simulation: low_radius_m must exceed reduced_radius_m, using 1500");
        out.effective.low_radius_m = 1500.0f;
    }
    if (!(out.effective.reduced_hz > 0.0f) || out.effective.reduced_hz > 20.0f) {
        out.warnings.emplace_back("simulation: reduced_hz must be in (0,20], using 10");
        out.effective.reduced_hz = 10.0f;
    }
    if (!(out.effective.low_hz > 0.0f) || out.effective.low_hz > 20.0f) {
        out.warnings.emplace_back("simulation: low_hz must be in (0,20], using 1");
        out.effective.low_hz = 1.0f;
    }
    if (out.effective.demote_full_sec < 0.0f) {
        out.warnings.emplace_back("simulation: demote_full_sec < 0, using 0");
        out.effective.demote_full_sec = 0.0f;
    }
    if (out.effective.demote_reduced_sec < 0.0f) {
        out.warnings.emplace_back("simulation: demote_reduced_sec < 0, using 0");
        out.effective.demote_reduced_sec = 0.0f;
    }
    if (out.effective.demote_low_sec < 0.0f) {
        out.warnings.emplace_back("simulation: demote_low_sec < 0, using 0");
        out.effective.demote_low_sec = 0.0f;
    }
    return out;
}

// Ticks between integrations for a tier (Full=1, Reduced/Low from hz).
// Pure helper shared by the scheduler loops so cadence can never diverge.
inline std::uint32_t LodPeriodTicks(SimulationTier tier, const LodConfig& config)
{
    auto hz_to_period = [](float hz) -> std::uint32_t {
        if (!(hz > 0.0f)) {
            return 20u;
        }
        const float period = 20.0f / hz;
        const std::uint32_t rounded =
            static_cast<std::uint32_t>(period + 0.5f);
        if (rounded < 1u) {
            return 1u;
        }
        return rounded > 400u ? 400u : rounded;
    };
    switch (tier) {
    case SimulationTier::Full:
        return 1u;
    case SimulationTier::Reduced:
        return hz_to_period(config.reduced_hz);
    case SimulationTier::Low:
        return hz_to_period(config.low_hz);
    case SimulationTier::Dormant:
        return 0u; // never scheduled; callers check the tier first
    }
    return 1u;
}

} // namespace gs::game
