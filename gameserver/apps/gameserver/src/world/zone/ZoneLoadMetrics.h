#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "../distributed/RuntimeIds.h"
#include "Zone.h"

// Per-zone load snapshot for load-aware scheduling evolution, benchmarks
// and future dynamic partitioning. Plain data, point-in-time, cheap to
// collect (atomics + one locked queue-depth read).
namespace gs::game {

class ZoneManager;

struct ZoneLoadMetrics {
    std::uint32_t zone_id = 0;
    std::uint32_t players = 0;
    std::uint32_t mobs = 0;
    std::uint32_t ghosts = 0;
    std::uint64_t avg_tick_us = 0;
    std::uint64_t aoi_queries = 0;
    std::uint64_t migrations = 0;
    std::uint64_t repl_records = 0;
    std::size_t queue_depth = 0;
    std::size_t max_queue_depth = 0;
    ZoneActivity activity = ZoneActivity::Active;
    // Per-stage tick sums of the current diagnostic window (microseconds).
    // Non-destructive reads; the periodic diag owns the exchange(). These
    // let the readiness benchmark attribute the tick cost to a stage without
    // a profiler.
    std::uint64_t gameplay_us = 0;
    std::uint64_t ai_us = 0;
    std::uint64_t movement_us = 0;
    std::uint64_t aoi_us = 0;
    std::uint64_t ghost_us = 0;
    std::uint64_t activity_publish_us = 0;
    std::uint64_t load_publish_us = 0;
    std::uint64_t replication_us = 0;
    std::uint64_t lod_eval_us = 0;
    std::uint64_t ghost_entities = 0;
};

void CollectZoneLoadMetrics(const ZoneManager& zones, std::vector<ZoneLoadMetrics>& out);

// Process-wide aggregate: the future cluster scheduler's input unit (§32).
// Plain snapshot, point-in-time, cheap to collect. Reuse: benchmarks print
// it; a load balancer would ship it.
struct ProcessLoadSnapshot {
    RuntimeIdentity identity;
    std::uint32_t world_tick = 0;
    std::size_t zone_count = 0;
    std::size_t active_zones = 0;
    std::size_t sleeping_zones = 0;
    std::uint64_t players = 0;
    std::uint64_t mobs = 0;
    std::uint64_t ghosts = 0;
    double avg_zone_tick_ms = 0.0;
    std::uint64_t repl_records = 0;
    std::uint64_t migrations = 0;
    std::uint64_t worker_tasks = 0;
    std::uint64_t worker_busy_us = 0;
    double supervisor_avg_ms = 0.0;
    std::uint64_t routes_local = 0;
    std::uint64_t routes_remote_emulated = 0;
    std::uint64_t routes_unavailable = 0;
    std::uint64_t routes_draining = 0;
    std::vector<ZoneLoadMetrics> zones;
};

} // namespace gs::game
