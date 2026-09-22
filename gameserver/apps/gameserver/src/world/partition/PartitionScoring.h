#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "map/MapData.h"

#include "../activity/ContinuousLoadField.h"
#include "../activity/SpatialActivityField.h"
#include "PartitionTypes.h"
#include "ZonePartition.h"

// Adaptive Partition Scoring (Adaptive Simulation Fabric, phase 2).
//
// OBSERVE -> SCORE -> DECIDE. This module is the SCORE step: it is strictly
// READ-ONLY. It never creates/retires a zone, never transfers an entity,
// never touches the owner map or the partition tree. Its output is a
// recommendation object; the EXISTING transactional split/merge layer is the
// only executor (Plan -> Stage -> Transfer -> Validate -> Commit/Rollback).
//
// A "split candidate" here is the CENTER POINT of the existing quadtree
// transaction: the four children are tiled from that point with the same
// half-open geometry the executor uses (BuildQuadtreeChildBounds). Candidates
// are deterministic, few, and cell-aligned to the load field grid.
//
// The score is an expected-improvement estimate over the NOOP baseline:
//
//   final_score = w_balance     * balance_benefit      (peak load reduction)
//               - w_boundary    * boundary_penalty     (cross-boundary work)
//               - w_migration   * migration_penalty    (transfer work estimate)
//               - w_replication * replication_penalty  (estimated future repl)
//               - w_instability * instability_penalty  (recent topology churn)
//               - topology_penalty                     (fixed +4 zones cost)
//
// Every term is normalized to [0,1] and clamped; the score is clamped to
// [-1,1] and guarded against NaN/Inf. All weights/budgets are configuration.
// The per-channel breakdown (Simulation/Replication/AOI/Combat/Migration) is
// preserved in every candidate score for diagnostics (§15).
namespace gs::game {

class Zone;

// --- candidate model --------------------------------------------------------

enum class SplitCandidateKind : std::uint8_t {
    Midpoint = 0, // geometric midpoint (legacy behavior, always evaluated)
    Centroid,     // cut through the load centroid on both axes
    CentroidX,    // load centroid on X, geometric midpoint on Y
    CentroidY,    // geometric midpoint on X, load centroid on Y
    BalancedX,    // X cut that splits the zone's load most evenly
    BalancedY,    // Y cut that splits the zone's load most evenly
    HotspotX,     // X cut at the largest hotspot's isolating edge
    HotspotY,     // Y cut at the largest hotspot's isolating edge
    HotspotCorner,// both cuts at the largest hotspot's isolating edges
    Count,
};

const char* SplitCandidateKindName(SplitCandidateKind kind) noexcept;

// --- scoring configuration --------------------------------------------------

// Term sub-blend inside the boundary penalty. Named constants, documented:
// a boundary is as expensive as the crossing signals it radiates. Activity
// (player influence near the cut) and migration (measured crossing churn) are
// the strongest signals; combat and hotspot splitting follow.
inline constexpr float kBoundaryActivityWeight = 0.35f;
inline constexpr float kBoundaryMigrationWeight = 0.35f;
inline constexpr float kBoundaryCombatWeight = 0.15f;
inline constexpr float kBoundaryHotspotWeight = 0.15f;

struct PartitionScoringConfig {
    // Geometric floor for candidate cuts (mirrors the effective partition
    // min_zone_size_m; each half must clear it). Set by ConfigurePartition.
    float min_zone_size_m = 500.0f;

    // Decision gate: a split is only recommended when the best candidate's
    // expected improvement reaches this value (NOOP baseline = 0).
    float min_expected_improvement = 0.10f;

    // Boundary band width around a candidate cut (meters). The band carries
    // the cross-boundary cost signals; 1.5x AOI radius by default (players
    // just inside a boundary interact across it).
    float boundary_band_m = 180.0f;

    // Hotspot detection: cells at/above this normalized composite are hot.
    float hotspot_threshold = 0.5f;
    std::uint32_t hotspot_max_count = 4;

    // Score term weights (all configurable, no hidden magic).
    float weight_balance = 1.0f;
    float weight_boundary = 0.35f;
    float weight_migration = 0.15f;
    float weight_replication = 0.20f;
    float weight_instability = 0.10f;
    float topology_penalty = 0.05f;

    // Band reference budgets: the raw signal value that saturates the
    // normalized [0,1] penalty. Calibrated by the synthetic bench.
    float activity_band_budget = 50.0f;      // player sources near the cut
    float migration_band_budget = 2.0f;      // normalized migration units
    float replication_band_budget = 10.0f;   // normalized replication units
    float combat_band_budget = 1.0f;         // normalized combat units

    // Migration cost estimate: weighted residents / budget.
    float migration_work_budget = 2000.0f;
    float player_transfer_weight = 4.0f; // sessions + OwnerMap cost more
    float mob_transfer_weight = 1.0f;

    // Instability: a mutation inside this window adds a soft penalty that
    // decays linearly to zero (hard gates like cooldowns stay in place).
    float instability_window_s = 120.0f;

    // Which smoothed timescale the scorer reads. Fast (rise ~1.5s, fall ~8s)
    // tracks the spatial distribution within seconds while the sustained
    // overload timer suppresses single-window spikes; see docs §7.
    LoadTimescale decision_timescale = LoadTimescale::Fast;

    // Log executed/why-not decisions (never per tick; control cycle only).
    bool decision_log_enabled = true;
    // Minimum seconds between why-not log lines for the same zone.
    float why_not_log_seconds = 10.0f;
};

struct ValidatedPartitionScoringConfig {
    PartitionScoringConfig effective;
    std::vector<std::string> warnings;
};

// Pure: repairs invalid fields with warnings, never UB. NaN/Inf invalid.
inline ValidatedPartitionScoringConfig ValidatePartitionScoringConfig(
    const PartitionScoringConfig& in)
{
    ValidatedPartitionScoringConfig out;
    out.effective = in;

    auto warn = [&out](const char* message) {
        out.warnings.emplace_back(std::string("partition-scoring: ") + message);
    };
    auto finite = [](float value) { return std::isfinite(value); };
    auto positive = [&finite](float value) { return finite(value) && value > 0.0f; };
    auto non_negative = [&finite](float value) { return finite(value) && value >= 0.0f; };

    if (!positive(out.effective.min_zone_size_m)) {
        warn("min_zone_size_m must be finite and > 0, using 500");
        out.effective.min_zone_size_m = 500.0f;
    }
    if (!finite(out.effective.min_expected_improvement) ||
        out.effective.min_expected_improvement < 0.0f ||
        out.effective.min_expected_improvement > 1.0f) {
        warn("min_expected_improvement outside [0,1], using 0.10");
        out.effective.min_expected_improvement = 0.10f;
    }
    if (!positive(out.effective.boundary_band_m)) {
        warn("boundary_band_m must be finite and > 0, using 180");
        out.effective.boundary_band_m = 180.0f;
    }
    if (!finite(out.effective.hotspot_threshold) || out.effective.hotspot_threshold <= 0.0f ||
        out.effective.hotspot_threshold > 1.0f) {
        warn("hotspot_threshold outside (0,1], using 0.5");
        out.effective.hotspot_threshold = 0.5f;
    }
    if (out.effective.hotspot_max_count < 1 || out.effective.hotspot_max_count > 16) {
        warn("hotspot_max_count outside [1,16], using 4");
        out.effective.hotspot_max_count = 4;
    }
    auto check_weight = [&](float& weight, const char* name) {
        if (!non_negative(weight)) {
            warn((std::string(name) + " must be finite and >= 0, using 1.0").c_str());
            weight = 1.0f;
        }
    };
    check_weight(out.effective.weight_balance, "weight_balance");
    check_weight(out.effective.weight_boundary, "weight_boundary");
    check_weight(out.effective.weight_migration, "weight_migration");
    check_weight(out.effective.weight_replication, "weight_replication");
    check_weight(out.effective.weight_instability, "weight_instability");
    check_weight(out.effective.topology_penalty, "topology_penalty");
    auto check_budget = [&](float& budget, const char* name, float fallback) {
        if (!positive(budget)) {
            warn((std::string(name) + " must be finite and > 0, using " +
                  std::to_string(static_cast<int>(fallback)))
                     .c_str());
            budget = fallback;
        }
    };
    check_budget(out.effective.activity_band_budget, "activity_band_budget", 50.0f);
    check_budget(out.effective.migration_band_budget, "migration_band_budget", 2.0f);
    check_budget(out.effective.replication_band_budget, "replication_band_budget", 10.0f);
    check_budget(out.effective.combat_band_budget, "combat_band_budget", 1.0f);
    check_budget(out.effective.migration_work_budget, "migration_work_budget", 2000.0f);
    check_budget(out.effective.player_transfer_weight, "player_transfer_weight", 4.0f);
    check_budget(out.effective.mob_transfer_weight, "mob_transfer_weight", 1.0f);
    if (!non_negative(out.effective.instability_window_s)) {
        warn("instability_window_s must be finite and >= 0, using 120");
        out.effective.instability_window_s = 120.0f;
    }
    if (!non_negative(out.effective.why_not_log_seconds)) {
        warn("why_not_log_seconds must be finite and >= 0, using 10");
        out.effective.why_not_log_seconds = 10.0f;
    }
    return out;
}

// --- scorer input / output --------------------------------------------------

// Pure read-only snapshot of everything the scorer needs. Filled by the
// caller (WorldRuntime) so the scorer itself never touches live zone state.
struct PartitionScoreInput {
    ZoneId zone_id = 0;
    mx::map::Rect bounds{};
    std::uint8_t depth = 0;
    std::uint64_t players = 0;
    std::uint64_t mobs = 0;
    // Most recent topology mutation touching this leaf (split of the leaf or
    // merge of its parent); {} = none known.
    std::chrono::steady_clock::time_point last_mutation{};
};

// One candidate's full breakdown. Everything the tuning/report path needs:
// the scalar alone is never sufficient (§23).
struct SplitCandidateScore {
    SplitCandidateKind kind = SplitCandidateKind::Midpoint;
    SplitCenter center{};
    bool valid = false;

    // Terms (all [0,1] except final_score).
    float balance_benefit = 0.0f;
    float boundary_penalty = 0.0f;
    float migration_penalty = 0.0f;
    float replication_penalty = 0.0f;
    float instability_penalty = 0.0f;
    float topology_penalty = 0.0f;
    float final_score = 0.0f; // expected improvement over NOOP, [-1,1]

    // Load distribution evidence.
    float before_total = 0.0f; // zone aggregate (decision timescale)
    float after_peak = 0.0f;   // max child aggregate
    float peak_reduction = 0.0f;
    float balance_ratio = 0.0f; // after_peak / mean child (1 = uniform)
    float child_total[4] = {};
    // Raw channel breakdown per child (NW/NE/SW/SE), for diagnostics.
    LoadChannels child_channels[4];

    // Boundary band evidence (the two internal cut lines).
    float activity_band = 0.0f;
    float migration_band = 0.0f;
    float replication_band = 0.0f;
    float combat_band = 0.0f;
    float hotspot_crossed_load = 0.0f;
    float hotspot_crossed_frac = 0.0f;
    std::uint32_t hotspots_crossed = 0;
};

struct SplitRecommendation {
    bool valid = false; // false = no geometrically valid candidate / no field
    ZoneId zone_id = 0;
    std::uint64_t field_epoch = 0;
    std::uint64_t activity_epoch = 0; // boundary-signal generation used
    std::chrono::steady_clock::time_point timestamp{};
    float zone_total_load = 0.0f;
    float zone_peak_cell = 0.0f;
    std::uint32_t hotspot_count = 0;
    std::vector<SplitCandidateScore> candidates; // deterministic order
    SplitCandidateScore baseline{};              // midpoint reference
    SplitCandidateScore best{};
    std::size_t best_index = 0;
    float expected_improvement = 0.0f; // == best.final_score
};

// Why a zone that is overloaded did NOT split (structured, for tuning §31).
enum class PartitionNoopReason : std::uint8_t {
    None = 0,
    NoField,            // load field unavailable/disabled
    NoValidCut,         // geometry cannot host any candidate
    BelowMinImprovement,// scored, but the gate rejected every candidate
    TransactionRejected,// executor refused after scoring (staging/plan)
};

const char* PartitionNoopReasonName(PartitionNoopReason reason) noexcept;

// Structured decision record (§30-31). Stored in a bounded in-memory log;
// logged as one line only at topology decisions / rate-limited why-not.
struct PartitionDecisionRecord {
    std::chrono::steady_clock::time_point timestamp{};
    ZoneId zone_id = 0;
    bool executed = false;
    bool scored = false;
    PartitionNoopReason noop_reason = PartitionNoopReason::None;
    // Stable literal: scheduler gate name or plan reject reason (why-not).
    const char* detail = "";
    float p99_tick_ms = 0.0f;
    float load_score = 0.0f;
    float field_load_score = 0.0f;
    std::uint64_t field_epoch = 0;
    float expected_improvement = 0.0f;
    SplitCandidateScore candidate{};
};

std::string FormatPartitionDecision(const PartitionDecisionRecord& record);

// --- the scorer -------------------------------------------------------------

// Supervisor-only, single-threaded (like the rest of the control plane).
// ScoreSplit is deterministic for identical inputs: same field generation,
// bounds, config and `now` produce bit-identical candidate lists/scores.
class PartitionScorer {
public:
    using Config = PartitionScoringConfig;

    explicit PartitionScorer(Config config = Config{})
        : config_(config)
    {
    }

    void SetConfig(Config config) noexcept
    {
        config_ = config;
    }
    const Config& GetConfig() const noexcept
    {
        return config_;
    }

    // Pure read-only scoring. `field` may be null/disabled -> invalid result.
    // `activity` is an independent boundary signal (§17) and may be null.
    //
    // STATELESS: all scratch is local, so concurrent readers (diagnostics,
    // benchmarks, admin) can score while the supervisor scores -- as long as
    // they pass immutable generations. No shared mutable state.
    SplitRecommendation ScoreSplit(const PartitionScoreInput& input,
                                   const LoadGrid* field,
                                   const ActivityGrid* activity,
                                   std::chrono::steady_clock::time_point now) const;

private:
    Config config_;
};

} // namespace gs::game
