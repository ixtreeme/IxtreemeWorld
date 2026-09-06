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

    // Subsystem stage sums (microseconds, since last diag read).
    std::atomic<std::uint64_t> gameplay_micros_since_diag{0};
    std::atomic<std::uint64_t> ghost_micros_since_diag{0};
    std::atomic<std::uint64_t> replication_micros_since_diag{0};

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
