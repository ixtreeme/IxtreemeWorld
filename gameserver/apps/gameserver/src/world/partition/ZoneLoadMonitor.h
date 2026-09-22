#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

#include "ZonePartition.h"
#include "../activity/LoadFieldTypes.h"
#include "../zone/ZoneScheduler.h"

// Load-driven topology control plane (runs at ~Hz, never on the hot path).
// OBSERVE step of the adaptive fabric: reads ZoneDiagnostics + the immutable
// load field generation through the ZoneManager, writes normalized scores +
// sustained timers onto the partition nodes, and exposes split candidates,
// merge groups and the overloaded-leaf list (why-not diagnostics). Decision
// cadence and thresholds are Config, not hardcode. Predicate evaluation lives
// on ZoneScheduler (pure, const); this class owns the mutation (scores,
// timers) so const-correctness stays honest. Scoring itself is a separate
// read-only step (PartitionScorer).
namespace gs::game {

class ZoneManager;
struct LoadGrid;

struct ZoneLoadSnapshot {
    ZoneId zone_id = 0;
    float load_score = 0.0f;        // combined gate input (max of the two below)
    float legacy_load_score = 0.0f; // avg/p99 tick + resident pressure
    float field_load_score = 0.0f;  // load field mean/peak composite (fast)
    float field_load_score_slow = 0.0f; // slow EMA view (conservative merge)
    float field_peak_cell = 0.0f;   // max per-cell composite inside the zone
    std::uint64_t field_active_cells = 0;
    float p95_tick_us = 0.0f;
    float p99_tick_us = 0.0f;
    std::uint64_t players = 0;
    std::uint64_t mobs = 0;
    std::uint64_t avg_tick_us = 0;
    std::chrono::steady_clock::time_point timestamp{};
};

// One observed quadtree sibling group (a real parent with four active leaf
// children). Discovery order is tree/child-vector order: deterministic.
struct MergeGroupSnapshot {
    ZoneId parent_id = 0;
    std::array<ZoneId, 4> child_ids{};
    float max_child_load_score = 0.0f;
    float parent_field_fast = 0.0f;
    float parent_field_slow = 0.0f;
    // Conservative group score: max(every child, parent aggregate fast/slow).
    // One hot child keeps the whole group ineligible (phase-3 §8).
    float group_load_score = 0.0f;
    float sustained_low_seconds = 0.0f; // 0 = timer not running
    ZoneScheduler::MergeGate gate = ZoneScheduler::MergeGate::NoParent;
    bool candidate = false; // gate == Pass
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
        // Load field timescale used for the field overload score. Must match
        // the scorer's decision timescale so observe and score agree.
        LoadTimescale field_timescale = LoadTimescale::Fast;
    };

    explicit ZoneLoadMonitor(Config config = Config{});

    // Supervisor only. Updates scores/timers, then fills candidates.
    // `load_field` may be null/disabled: the field score is then 0 and the
    // legacy (avg/p99 tick + resident) behavior is preserved exactly.
    void Update(ZoneManager& zones,
                const ZoneScheduler& scheduler,
                std::chrono::steady_clock::time_point now,
                const std::shared_ptr<const LoadGrid>& load_field);

    const std::vector<ZoneId>& SplitCandidates() const noexcept
    {
        return split_candidates_;
    }
    // Gate-passing merge groups, deterministic order.
    const std::vector<ZoneId>& MergeCandidates() const noexcept
    {
        return merge_candidates_;
    }
    // Every observed group with its gate state (why-not diagnostics).
    const std::vector<MergeGroupSnapshot>& MergeGroups() const noexcept
    {
        return merge_groups_;
    }
    // Leaves whose combined score is at/above the split threshold, regardless
    // of whether the hard gates (sustained/cooldown/depth/size) passed. The
    // why-not diagnostics path uses this to explain rejections.
    const std::vector<ZoneId>& OverloadedLeaves() const noexcept
    {
        return overloaded_leaves_;
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
    std::vector<MergeGroupSnapshot> merge_groups_;
    std::vector<ZoneId> overloaded_leaves_;
    std::vector<std::uint64_t> tick_scratch_; // supervisor-only, reused
};

} // namespace gs::game
