#pragma once

#include <cstdint>
#include <string>

#include <boost/asio/io_context.hpp>

// Phase-4 integrated readiness benchmark (100 km world / 500 players /
// 200,000 mobs). Uses a synthetic flat world (no 100 km^2 heightfield asset)
// but the PRODUCTION systems end to end: zones, scheduler, partition
// split/merge, migration, ghost, LOD, zone sleep/wake, activity field,
// load field, monitor, scorer, stability controller, AOI, replication.
//
// Deterministic setup; every scenario runs SETUP -> WARMUP -> MEASURE ->
// FINAL VALIDATION. Setup cost is never mixed into steady-state tick cost.
namespace gs::bench {

struct ReadinessConfig {
    std::string scenario = "spread"; // spread|quiet|hotspot|multi|moving|border|combat|replication|churn|dense
    float world_km = 100.0f;
    int zones_x = 8;
    int zones_y = 8;
    int players = 500;
    int mobs = 200000;
    int warmup_seconds = 15;
    int measure_seconds = 30;
    bool asf_off = false;        // adaptive control off (observe-only baseline)
    bool load_field_off = false; // load field telemetry off
    bool lod_off = false;        // LOD off (every entity Full-equivalent)
    // Correctness run: exact ghost equivalence validation every ~2 s during
    // the measure phase (auto-repair disabled so failures are visible). The
    // performance numbers of such a run are NOT comparable (shadow scans all
    // authority) -- use a separate performance run with this off.
    bool ghost_shadow = false;
    // Phase 5B correctness run: exact interest-set + recipient-coverage
    // shadow validation every ~2 s during the measure phase. Like
    // ghost_shadow, this is a CORRECTNESS run; its performance numbers are
    // not comparable to a clean performance run.
    bool replication_shadow = false;
    // Phase 5B A/B switches (benchmark-only): legacy full replication (every
    // visible entity every tick) and legacy full AOI candidate sort.
    bool repl_full = false;
    bool aoi_full_sort = false;
    // Phase 5D A/B: AOI candidates read positions from the authoritative
    // component (the pre-5D reference path) instead of the grid entry.
    bool aoi_reference_positions = false;
    // Phase 5D top-k A/B: force the 5B partial_sort instead of nth_element.
    bool aoi_partial_sort = false;
    std::uint32_t seed = 20260922;
};

int RunReadinessBenchmark(boost::asio::io_context& io, const ReadinessConfig& config);

} // namespace gs::bench
