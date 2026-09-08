#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

// Zone-local runtime counters and lightweight perf telemetry. Atomics because
// the supervisor thread reads them while a worker owns the zone. Gameplay
// state is NOT here; these are only observability counters.
//
// Perf baseline (§24): per-tick wall time goes to a small ring buffer (for
// p50/p95/p99 in benchmarks) plus sums; subsystem stages accumulate into
// their own sums. Cost per tick: a handful of clock reads + integer adds --
// negligible against a 50 ms tick budget.
namespace gs::game {

struct ZoneDiagnostics {
    static constexpr std::size_t kTickSampleCapacity = 256;

    std::atomic<std::uint32_t> player_count{0};
    std::atomic<std::uint32_t> mob_count{0};
    std::atomic<std::uint32_t> wandering_mob_count{0};
    std::atomic<std::uint32_t> idle_mob_count{0};
    std::atomic<std::uint32_t> ghost_count{0};
    std::atomic<std::uint64_t> ticks_since_diag{0};
    std::atomic<std::uint64_t> transform_records_since_diag{0};
    std::atomic<std::uint64_t> empty_skips_since_diag{0};
    std::atomic<std::uint64_t> migrations_since_diag{0};
    std::atomic<std::uint64_t> tick_micros_since_diag{0};
    std::atomic<std::uint64_t> aoi_queries_since_diag{0};
    // Dirty pipeline (§27): entities whose transform actually moved this
    // tick, vs. records sent. Sending is unchanged (full frames); the ratio
    // shows what a future delta protocol could skip.
    std::atomic<std::uint64_t> transform_dirty_since_diag{0};
    // Relevance tiers (§29): emitted AOI candidates by distance band. No
    // behavior change today; the seam for future frequency tiers
    // (near 20 Hz / mid 10 Hz / far 2-5 Hz / unchanged 0 Hz).
    std::atomic<std::uint64_t> tier_near_since_diag{0};
    std::atomic<std::uint64_t> tier_mid_since_diag{0};
    std::atomic<std::uint64_t> tier_far_since_diag{0};

    // Subsystem stage sums (microseconds, since last diag read).
    std::atomic<std::uint64_t> gameplay_micros_since_diag{0};
    std::atomic<std::uint64_t> ghost_micros_since_diag{0};
    std::atomic<std::uint64_t> replication_micros_since_diag{0};

    // Simulation LOD (§22). Tier gauges are recounted exactly by the 1 Hz
    // zone evaluation; insert paths bump them synchronously so sleep
    // decisions never observe a stale zero. Work counters are cumulative.
    std::atomic<std::uint32_t> lod_full{0};
    std::atomic<std::uint32_t> lod_reduced{0};
    std::atomic<std::uint32_t> lod_low{0};
    std::atomic<std::uint32_t> lod_dormant{0};
    std::atomic<std::uint64_t> lod_ai_updates_since_diag{0};
    std::atomic<std::uint64_t> lod_move_updates_since_diag{0};
    std::atomic<std::uint64_t> lod_promotions_since_diag{0};
    std::atomic<std::uint64_t> lod_demotions_since_diag{0};
    std::atomic<std::uint64_t> lod_wakes_since_diag{0};
    std::atomic<std::uint64_t> lod_eval_us_since_diag{0};
    // Cross-zone activity (§29): LOD tiers granted (at least jointly) by a
    // foreign-zone player. Counted by final tier, aligned with the gauges.
    std::atomic<std::uint64_t> cross_zone_full_since_diag{0};
    std::atomic<std::uint64_t> cross_zone_reduced_since_diag{0};
    std::atomic<std::uint64_t> cross_zone_low_since_diag{0};
    // External-activity sleep decisions (§29): passes where outside influence
    // alone blocked sleep / woke a sleeping zone.
    std::atomic<std::uint64_t> sleep_blocked_external_since_diag{0};
    std::atomic<std::uint64_t> wake_external_since_diag{0};

    // Recent per-tick wall times (microseconds), newest at head-1.
    std::array<std::atomic<std::uint64_t>, kTickSampleCapacity> tick_samples;
    std::atomic<std::size_t> tick_sample_head{0};

    void RecordTickSample(std::uint64_t micros) noexcept
    {
        const std::size_t slot =
            tick_sample_head.fetch_add(1, std::memory_order_relaxed) % kTickSampleCapacity;
        tick_samples[slot].store(micros, std::memory_order_relaxed);
    }

    // Copies up to capacity recent samples (order not guaranteed -- the owner
    // thread may be writing concurrently; values are individually atomic).
    std::size_t CopyTickSamples(std::uint64_t* out, std::size_t capacity) const noexcept
    {
        const std::size_t head = tick_sample_head.load(std::memory_order_relaxed);
        std::size_t count = head < kTickSampleCapacity ? head : kTickSampleCapacity;
        if (count > capacity) {
            count = capacity;
        }
        for (std::size_t i = 0; i < count; ++i) {
            out[i] = tick_samples[(head - 1 - i) % kTickSampleCapacity].load(std::memory_order_relaxed);
        }
        return count;
    }
};

} // namespace gs::game
