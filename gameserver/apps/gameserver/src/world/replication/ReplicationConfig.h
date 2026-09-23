#pragma once

#include <algorithm>
#include <cstdint>

// Phase 5B replication/AOI knobs. World-global (owned by WorldRuntime),
// read-only for zone ticks through ZoneTickContext. Defaults are the
// optimized path; every switch exists for A/B measurement, rollback or
// debugging (never for gameplay semantics).
namespace gs::game {

struct ReplicationConfig {
    // Change-driven transform records: only entities whose transform changed
    // since the recipient last received them (plus a staggered periodic
    // refresh) are included in a viewer's frame. OFF = legacy behavior: every
    // visible entity is sent every tick.
    bool dirty_enabled = true;
    // AOI candidate reduction: keep only the nearest kAoiEntityCap candidates
    // instead of sorting the whole candidate set. The selected set is
    // identical to the full-sort result (same distance,net tie-break);
    // OFF = legacy full sort.
    bool aoi_partial_cap = true;
    // Phase 5D top-k selection strategy: true = nth_element + sort of the
    // selected prefix (O(n) selection), false = partial_sort (O(n log k)).
    // Both produce the identical ordered top-k. A/B only.
    bool aoi_nth_element = true;
    // Full transform refresh period in ticks (staggered per viewer). Bounds
    // any missed update to this window and keeps the client state exact.
    std::uint32_t refresh_ticks = 20; // 1 s at 20 Hz
    // Phase 5D A/B: read AOI candidate positions from the spatial grid entry
    // (optimized, default) instead of from the authoritative flecs component
    // (the pre-5D reference path). Both produce the identical visible set.
    bool aoi_reference_positions = false;

    // ---- Phase 6: replication protocol v2 --------------------------------
    // v2 sends field-level deltas against the recipient's known state,
    // scheduled by a recipient-relative Network LOD and constrained by a
    // per-session budget. false = the v1 full-state behavior (reference).
    bool v2_enabled = true;
    // Network LOD (recipient-relative update frequency; distinct from the
    // Simulation LOD). Distances in meters, periods in ticks (20 Hz base).
    struct NetworkLodTierConfig {
        float near_distance_m = 40.0f;
        float normal_distance_m = 80.0f;
        std::uint32_t near_period_ticks = 1;    // 20 Hz
        std::uint32_t normal_period_ticks = 2;  // 10 Hz
        std::uint32_t reduced_period_ticks = 4; // 5 Hz (beyond normal_distance)
        // Combat relevance (attacker/target/recent hit) promotes to Critical.
        std::uint32_t critical_period_ticks = 1;
    };
    NetworkLodTierConfig network_lod;
    bool network_lod_enabled = true;
    // Per-session per-frame budget. Critical records (lifecycle, combat
    // promotion, self) always pass; state beyond the budget stays pending and
    // is retried next frame. 0 = unlimited.
    std::uint32_t budget_max_records = 0;   // e.g. 64
    std::uint32_t budget_max_bytes = 0;     // e.g. 2048
    // Starvation protection: a pending state older than this many ticks is
    // sent first (bypassing the budget order). Bounds the client state age.
    std::uint32_t max_defer_ticks = 40;     // 2 s at 20 Hz
    // Staggered periodic full-state resync (per viewer, phase by NetId).
    // 0 disables it (only valid with v2 off or as an A/B switch).
    std::uint32_t resync_ticks = 0;         // 0 = reuse refresh_ticks
};

// Clamps a candidate config into the supported range. Returns true when any
// correction was applied (the caller logs it).
inline bool ValidateReplicationConfig(ReplicationConfig& config)
{
    bool corrected = false;
    const std::uint32_t clamped = std::clamp<std::uint32_t>(config.refresh_ticks, 1, 400);
    if (clamped != config.refresh_ticks) {
        config.refresh_ticks = clamped;
        corrected = true;
    }
    return corrected;
}

} // namespace gs::game
