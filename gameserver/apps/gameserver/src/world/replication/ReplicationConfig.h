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
