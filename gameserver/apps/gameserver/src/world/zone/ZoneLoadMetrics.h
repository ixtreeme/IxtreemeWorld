#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

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
    ZoneActivity activity = ZoneActivity::Active;
};

void CollectZoneLoadMetrics(ZoneManager& zones, std::vector<ZoneLoadMetrics>& out);

} // namespace gs::game
