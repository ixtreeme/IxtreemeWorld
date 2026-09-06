#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

// Zone-local runtime counters. Atomics because the supervisor thread reads
// them for diagnostics while a worker owns the zone. Gameplay state is NOT
// here; these are only observability counters.
namespace gs::game {

struct ZoneDiagnostics {
    std::atomic<std::uint32_t> player_count{0};
    std::atomic<std::uint32_t> mob_count{0};
    std::atomic<std::uint32_t> wandering_mob_count{0};
    std::atomic<std::uint32_t> idle_mob_count{0};
    std::atomic<std::uint32_t> ghost_count{0};
    std::atomic<std::uint64_t> ticks_since_diag{0};
    std::atomic<std::uint64_t> transform_records_since_diag{0};
    std::atomic<std::uint64_t> empty_skips_since_diag{0};
    std::atomic<std::uint64_t> migrations_since_diag{0};
    // Lightweight perf baseline (§24): accumulated zone-tick wall time and
    // AOI queries; the supervisor reports per-second averages. Cheap enough
    // to keep on in production (one clock read per tick + integer adds).
    std::atomic<std::uint64_t> tick_micros_since_diag{0};
    std::atomic<std::uint64_t> aoi_queries_since_diag{0};
};

} // namespace gs::game
