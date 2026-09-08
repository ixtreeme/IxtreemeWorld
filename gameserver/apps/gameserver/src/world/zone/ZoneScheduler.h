#pragma once

#include <chrono>
#include <cstdint>

// Decides WHEN each zone ticks. Gameplay-agnostic: it only looks at resident
// counts, pending commands and tick deadlines, then hands due zones to the
// worker pool. Empty zones are skipped (diagnostic counter, as before) and
// their deadline is pushed forward so they stay idle.
namespace gs::game {

class ZoneManager;
class ZoneWorkerPool;
struct ZonePartition;

class ZoneScheduler {
public:
    // Split/merge gating. Scores are normalized load fractions (1.0 == at
    // budget); merge sits far below split (hysteresis) plus cooldowns, so
    // the topology cannot flap split/merge/split.
    struct Config {
        float split_load_threshold = 0.9f;
        float merge_load_threshold = 0.25f;
        std::chrono::seconds sustained_window{10};
        std::chrono::seconds split_cooldown{30};
        std::chrono::seconds merge_cooldown{60};
        std::uint8_t max_depth = 4;
    };

    void ScheduleOnce(ZoneManager& zones,
                      ZoneWorkerPool& pool,
                      std::chrono::steady_clock::time_point now);

    // Pure predicates over leaf state (const: timers are owned/updated by
    // ZoneLoadMonitor::Update). Execution re-validates in ZoneManager.
    bool ShouldSplit(const ZonePartition* leaf, std::chrono::steady_clock::time_point now) const;
    bool ShouldMerge(const ZonePartition* leaf, std::chrono::steady_clock::time_point now) const;

    // Simulation LOD master switch (mirrors LodConfig::enabled, set by
    // WorldRuntime::ConfigureSimulationLod). Off = legacy sleep rule.
    void SetLodEnabled(bool enabled) noexcept
    {
        lod_enabled_ = enabled;
    }

    Config config;

private:
    bool lod_enabled_ = true;
};

} // namespace gs::game
