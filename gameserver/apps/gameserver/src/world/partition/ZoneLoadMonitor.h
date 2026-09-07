#pragma once

#include <chrono>
#include <cstdint>
#include <vector>

#include "ZonePartition.h"

// Load-driven topology control plane (runs at ~Hz, never on the hot path).
// Reads ZoneDiagnostics through the ZoneManager, writes normalized scores +
// sustained-breach timers onto the partition leaves, and exposes split/merge
// candidates. Decision cadence and thresholds are Config, not hardcode.
// Predicate evaluation itself lives on ZoneScheduler (pure, const); this
// class owns the mutation (scores, timers) so const-correctness stays honest.
namespace gs::game {

class ZoneManager;
class ZoneScheduler;

struct ZoneLoadSnapshot {
    ZoneId zone_id = 0;
    float load_score = 0.0f;
    std::uint64_t players = 0;
    std::uint64_t mobs = 0;
    std::uint64_t avg_tick_us = 0;
    std::chrono::steady_clock::time_point timestamp{};
};

class ZoneLoadMonitor {
public:
    struct Config {
        float split_load_threshold = 0.9f; // sustained score above -> split
        float merge_load_threshold = 0.25f; // sustained score below -> merge
        std::chrono::seconds sustained_window{10};
        std::chrono::seconds split_cooldown{30};
        std::chrono::seconds merge_cooldown{60};
        float tick_budget_ms = 16.0f; // score 1.0 == avg tick at budget
        float resident_budget = 2000.0f; // score 1.0 == this many residents
    };

    explicit ZoneLoadMonitor(Config config = Config{});

    // Supervisor only. Updates scores/timers, then fills candidates.
    void Update(ZoneManager& zones,
                const ZoneScheduler& scheduler,
                std::chrono::steady_clock::time_point now);

    const std::vector<ZoneId>& SplitCandidates() const noexcept
    {
        return split_candidates_;
    }
    const std::vector<ZoneId>& MergeCandidates() const noexcept
    {
        return merge_candidates_;
    }
    const std::vector<ZoneLoadSnapshot>& RecentSnapshots() const noexcept
    {
        return snapshots_;
    }

    const Config& GetConfig() const noexcept
    {
        return config_;
    }

private:
    Config config_;
    std::vector<ZoneLoadSnapshot> snapshots_;
    std::vector<ZoneId> split_candidates_;
    std::vector<ZoneId> merge_candidates_;
};

} // namespace gs::game
