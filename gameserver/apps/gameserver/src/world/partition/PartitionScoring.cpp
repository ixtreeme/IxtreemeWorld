#include "PartitionScoring.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace gs::game {

namespace {

constexpr float kScoreEpsilon = 1e-4f;

float Clamp01(float value) noexcept
{
    if (!std::isfinite(value) || value <= 0.0f) {
        return 0.0f;
    }
    return std::min(value, 1.0f);
}

// Candidate cuts are snapped to load-field cell boundaries so the child
// aggregation tiles cells exactly (no cell counted by two children). The snap
// is clamped into [lo, hi] (the minimum-zone-size safe range); when that range
// contains no cell boundary the plain clamped value is used.
float SnapCutToCell(float value, float grid_origin, float cell_size, float lo, float hi) noexcept
{
    if (!(cell_size > 0.0f) || !std::isfinite(value)) {
        return std::clamp(value, lo, hi);
    }
    const float k_lo = std::ceil((lo - grid_origin) / cell_size);
    const float k_hi = std::floor((hi - grid_origin) / cell_size);
    if (k_hi < k_lo) {
        return std::clamp(value, lo, hi);
    }
    const float k = std::round((value - grid_origin) / cell_size);
    return grid_origin + std::clamp(k, k_lo, k_hi) * cell_size;
}

// Load-weighted cut on one axis: the cell boundary that splits the zone's
// composite load most evenly. Deterministic: lowest position wins ties.
// Falls back to the midpoint when the zone carries no load.
float FindBalancedCut(const LoadGrid& grid,
                      const WorldBounds& rect,
                      LoadTimescale scale,
                      bool axis_x,
                      float lo,
                      float hi,
                      float midpoint) noexcept
{
    // Sum composite along the cut axis (columns for an X cut, rows for Y).
    const std::uint32_t x0 = grid.ClampedCellX(rect.min_x);
    const std::uint32_t x1 = grid.ClampedCellX(std::nextafter(rect.max_x, rect.min_x));
    const std::uint32_t y0 = grid.ClampedCellY(rect.min_y);
    const std::uint32_t y1 = grid.ClampedCellY(std::nextafter(rect.max_y, rect.min_y));
    if (x1 < x0 || y1 < y0) {
        return midpoint;
    }
    std::vector<float> sums(static_cast<std::size_t>(axis_x ? (x1 - x0 + 1) : (y1 - y0 + 1)), 0.0f);
    float total = 0.0f;
    for (std::uint32_t y = y0; y <= y1; ++y) {
        for (std::uint32_t x = x0; x <= x1; ++x) {
            const auto& cell = grid.cells[static_cast<std::size_t>(y) * grid.dim_x + x];
            const NormalizedLoad normalized =
                NormalizeLoad(LoadChannelsFor(cell, scale), grid.config, grid.window_seconds);
            const std::size_t index = axis_x ? (x - x0) : (y - y0);
            sums[index] += normalized.composite;
            total += normalized.composite;
        }
    }
    if (!(total > kScoreEpsilon)) {
        return midpoint;
    }
    // Scan boundaries between cell k-1 and k; pick the one whose left half is
    // closest to total/2.
    float best_position = midpoint;
    float best_diff = std::numeric_limits<float>::max();
    float left = 0.0f;
    const std::uint32_t first = axis_x ? x0 : y0;
    const std::uint32_t last = axis_x ? x1 : y1;
    for (std::uint32_t k = first + 1; k <= last; ++k) {
        left += sums[k - 1 - first];
        const float right = total - left;
        const float diff = std::abs(left - right);
        const float position =
            (axis_x ? grid.bounds.min_x : grid.bounds.min_y) +
            static_cast<float>(k) * grid.cell_size_m;
        if (diff < best_diff - kScoreEpsilon ||
            (std::abs(diff - best_diff) <= kScoreEpsilon && position < best_position)) {
            best_diff = diff;
            best_position = position;
        }
    }
    return std::clamp(best_position, lo, hi);
}

// Scores one candidate. Pure: all inputs are immutable snapshots/POD.
void ScoreCandidate(SplitCandidateScore& score,
                    const PartitionScoreInput& input,
                    const LoadGrid& field,
                    const ActivityGrid* activity,
                    const LoadAggregate& zone_aggregate,
                    const std::vector<LoadHotspot>& hotspots,
                    const PartitionScoringConfig& config,
                    std::chrono::steady_clock::time_point now)
{
    const mx::map::Rect& bounds = input.bounds;
    mx::map::Rect children[4];
    BuildQuadtreeChildBounds(bounds, score.center.x, score.center.y, children);

    float child_total[4] = {};
    float sum = 0.0f;
    for (int i = 0; i < 4; ++i) {
        const WorldBounds child_bounds{
            children[i].min_x, children[i].min_y, children[i].max_x, children[i].max_y};
        const LoadAggregate child = AggregateLoad(field, child_bounds, config.decision_timescale);
        child_total[i] = child.composite_sum;
        sum += child.composite_sum;
        score.child_total[i] = child.composite_sum;
        score.child_channels[i] = child.raw;
    }
    score.before_total = zone_aggregate.composite_sum;
    score.after_peak = *std::max_element(child_total, child_total + 4);
    const float mean = sum * 0.25f;
    score.peak_reduction =
        zone_aggregate.composite_sum > kScoreEpsilon
            ? Clamp01((zone_aggregate.composite_sum - score.after_peak) /
                      zone_aggregate.composite_sum)
            : 0.0f;
    score.balance_ratio = mean > kScoreEpsilon ? score.after_peak / mean : 1.0f;
    score.balance_benefit = score.peak_reduction;

    // Boundary bands: one thin rect around each internal cut line. The two
    // bands overlap in a band x band square at the center; summing both is a
    // documented conservative over-estimate (bands are thin).
    const float half_band = config.boundary_band_m * 0.5f;
    const WorldBounds x_band{
        score.center.x - half_band, bounds.min_y, score.center.x + half_band, bounds.max_y};
    const WorldBounds y_band{
        bounds.min_x, score.center.y - half_band, bounds.max_x, score.center.y + half_band};
    const LoadAggregate x_aggregate = AggregateLoad(field, x_band, config.decision_timescale);
    const LoadAggregate y_aggregate = AggregateLoad(field, y_band, config.decision_timescale);
    const std::size_t migration_index = LoadChannelIndex(LoadChannel::Migration);
    const std::size_t replication_index = LoadChannelIndex(LoadChannel::Replication);
    const std::size_t combat_index = LoadChannelIndex(LoadChannel::Combat);
    const float migration_units =
        x_aggregate.normalized[migration_index] + y_aggregate.normalized[migration_index];
    const float replication_units =
        x_aggregate.normalized[replication_index] + y_aggregate.normalized[replication_index];
    const float combat_units =
        x_aggregate.normalized[combat_index] + y_aggregate.normalized[combat_index];
    std::size_t activity_sources = 0;
    if (activity != nullptr && activity->Enabled()) {
        // Independent activity signal (§17): player influence near the cut,
        // never folded back into a load channel.
        activity_sources =
            activity->CountPlayerSourcesIn(x_band) + activity->CountPlayerSourcesIn(y_band);
    }
    score.activity_band =
        Clamp01(static_cast<float>(activity_sources) / config.activity_band_budget);
    score.migration_band = Clamp01(migration_units / config.migration_band_budget);
    score.replication_band = Clamp01(replication_units / config.replication_band_budget);
    score.combat_band = Clamp01(combat_units / config.combat_band_budget);

    // Hotspot-aware penalty (§23-24): a cut that passes through a hot
    // component is expected to create cross-boundary work, so it is penalized
    // in proportion to the load it would split.
    float crossed_load = 0.0f;
    std::uint32_t crossed_count = 0;
    for (const auto& hotspot : hotspots) {
        const bool crosses_x =
            hotspot.bounds.min_x < score.center.x && score.center.x < hotspot.bounds.max_x;
        const bool crosses_y =
            hotspot.bounds.min_y < score.center.y && score.center.y < hotspot.bounds.max_y;
        if (crosses_x || crosses_y) {
            crossed_load += hotspot.load;
            ++crossed_count;
        }
    }
    score.hotspot_crossed_load = crossed_load;
    score.hotspot_crossed_frac =
        zone_aggregate.composite_sum > kScoreEpsilon
            ? Clamp01(crossed_load / zone_aggregate.composite_sum)
            : 0.0f;
    score.hotspots_crossed = crossed_count;

    score.boundary_penalty =
        Clamp01(kBoundaryActivityWeight * score.activity_band +
                kBoundaryMigrationWeight * score.migration_band +
                kBoundaryCombatWeight * score.combat_band +
                kBoundaryHotspotWeight * score.hotspot_crossed_frac);
    score.replication_penalty = score.replication_band;

    // Migration cost: estimated transfer work from the CURRENT population
    // (zone-level, therefore candidate-independent; kept for objective
    // completeness and diagnostics, never a tie-breaker).
    const float transfer_work = static_cast<float>(input.players) * config.player_transfer_weight +
                                static_cast<float>(input.mobs) * config.mob_transfer_weight;
    score.migration_penalty = Clamp01(transfer_work / config.migration_work_budget);

    // Instability: soft decay after a recent topology mutation (hard gates
    // stay in the scheduler/executor).
    score.instability_penalty = 0.0f;
    if (input.last_mutation != std::chrono::steady_clock::time_point{} &&
        config.instability_window_s > 0.0f) {
        const float elapsed = std::chrono::duration<float>(now - input.last_mutation).count();
        if (elapsed >= 0.0f && elapsed < config.instability_window_s) {
            score.instability_penalty = Clamp01(1.0f - elapsed / config.instability_window_s);
        }
    }
    score.topology_penalty = Clamp01(config.topology_penalty);

    const float final_score = config.weight_balance * score.balance_benefit -
                              config.weight_boundary * score.boundary_penalty -
                              config.weight_migration * score.migration_penalty -
                              config.weight_replication * score.replication_penalty -
                              config.weight_instability * score.instability_penalty -
                              score.topology_penalty;
    score.final_score =
        std::isfinite(final_score) ? std::clamp(final_score, -1.0f, 1.0f) : -1.0f;
    score.valid = true;
}

} // namespace

const char* SplitCandidateKindName(SplitCandidateKind kind) noexcept
{
    switch (kind) {
    case SplitCandidateKind::Midpoint:
        return "midpoint";
    case SplitCandidateKind::Centroid:
        return "centroid";
    case SplitCandidateKind::CentroidX:
        return "centroid-x";
    case SplitCandidateKind::CentroidY:
        return "centroid-y";
    case SplitCandidateKind::BalancedX:
        return "balanced-x";
    case SplitCandidateKind::BalancedY:
        return "balanced-y";
    case SplitCandidateKind::HotspotX:
        return "hotspot-x";
    case SplitCandidateKind::HotspotY:
        return "hotspot-y";
    case SplitCandidateKind::HotspotCorner:
        return "hotspot-corner";
    case SplitCandidateKind::Count:
    default:
        return "?";
    }
}

const char* PartitionNoopReasonName(PartitionNoopReason reason) noexcept
{
    switch (reason) {
    case PartitionNoopReason::NoField:
        return "no-field";
    case PartitionNoopReason::NoValidCut:
        return "no-valid-cut";
    case PartitionNoopReason::BelowMinImprovement:
        return "below-min-improvement";
    case PartitionNoopReason::TransactionRejected:
        return "transaction-rejected";
    case PartitionNoopReason::None:
    default:
        return "none";
    }
}

SplitRecommendation PartitionScorer::ScoreSplit(const PartitionScoreInput& input,
                                                const LoadGrid* field,
                                                const ActivityGrid* activity,
                                                std::chrono::steady_clock::time_point now) const
{
    SplitRecommendation recommendation;
    recommendation.zone_id = input.zone_id;
    recommendation.timestamp = now;
    const bool bounds_valid = input.bounds.max_x > input.bounds.min_x &&
                              input.bounds.max_y > input.bounds.min_y;
    if (field == nullptr || !field->enabled || field->cells.empty() || !bounds_valid) {
        return recommendation; // invalid: caller maps to NoField/NoValidCut
    }
    recommendation.field_epoch = field->epoch;

    // Geometric validity of the cut range: both halves must clear the
    // minimum zone size (the executor re-validates; invalid candidates never
    // reach it).
    const float min_size = std::max(1.0f, config_.min_zone_size_m);
    const float cx_min = input.bounds.min_x + min_size;
    const float cx_max = input.bounds.max_x - min_size;
    const float cy_min = input.bounds.min_y + min_size;
    const float cy_max = input.bounds.max_y - min_size;
    if (cx_max < cx_min || cy_max < cy_min) {
        return recommendation;
    }
    const float mid_x = (input.bounds.min_x + input.bounds.max_x) * 0.5f;
    const float mid_y = (input.bounds.min_y + input.bounds.max_y) * 0.5f;

    const WorldBounds zone_bounds{
        input.bounds.min_x, input.bounds.min_y, input.bounds.max_x, input.bounds.max_y};
    const LoadAggregate zone_aggregate =
        AggregateLoad(*field, zone_bounds, config_.decision_timescale);
    recommendation.zone_total_load = zone_aggregate.composite_sum;
    recommendation.zone_peak_cell = zone_aggregate.composite_peak;

    std::vector<std::uint8_t> hotspot_scratch; // local: ScoreSplit stays stateless
    const std::vector<LoadHotspot> hotspots =
        DetectLoadHotspots(*field, zone_bounds, config_.decision_timescale,
                           config_.hotspot_threshold, config_.hotspot_max_count, hotspot_scratch);
    recommendation.hotspot_count = static_cast<std::uint32_t>(hotspots.size());

    float centroid_x = mid_x;
    float centroid_y = mid_y;
    if (zone_aggregate.weight_sum > 0.0f) {
        centroid_x = zone_aggregate.centroid_x;
        centroid_y = zone_aggregate.centroid_y;
    }
    float balanced_x = mid_x;
    float balanced_y = mid_y;
    if (zone_aggregate.composite_sum > kScoreEpsilon) {
        balanced_x = FindBalancedCut(*field, zone_bounds, config_.decision_timescale, true, cx_min,
                                     cx_max, mid_x);
        balanced_y = FindBalancedCut(*field, zone_bounds, config_.decision_timescale, false, cy_min,
                                     cy_max, mid_y);
    }

    // Hotspot-aware cuts: shift the cut to the largest hotspot's isolating
    // edge so the hot component stays whole inside one child. The edge choice
    // is deterministic: the hotspot is pushed toward its nearer zone half.
    float hotspot_x = mid_x;
    float hotspot_y = mid_y;
    if (!hotspots.empty()) {
        const LoadHotspot& primary = hotspots[0];
        hotspot_x = primary.center_x >= mid_x ? primary.bounds.max_x : primary.bounds.min_x;
        hotspot_y = primary.center_y >= mid_y ? primary.bounds.max_y : primary.bounds.min_y;
    }

    struct RawCandidate {
        SplitCandidateKind kind;
        float x;
        float y;
    };
    // Fixed, documented evaluation order. Dedupe keeps the first occurrence,
    // so the candidate list itself is deterministic.
    const RawCandidate raw_candidates[] = {
        {SplitCandidateKind::Midpoint, mid_x, mid_y},
        {SplitCandidateKind::Centroid, centroid_x, centroid_y},
        {SplitCandidateKind::CentroidX, centroid_x, mid_y},
        {SplitCandidateKind::CentroidY, mid_x, centroid_y},
        {SplitCandidateKind::BalancedX, balanced_x, mid_y},
        {SplitCandidateKind::BalancedY, mid_x, balanced_y},
        {SplitCandidateKind::HotspotX, hotspot_x, mid_y},
        {SplitCandidateKind::HotspotY, mid_x, hotspot_y},
        {SplitCandidateKind::HotspotCorner, hotspot_x, hotspot_y},
    };
    recommendation.candidates.reserve(9);
    for (const RawCandidate& raw : raw_candidates) {
        SplitCandidateScore score;
        score.kind = raw.kind;
        score.center.x =
            SnapCutToCell(raw.x, field->bounds.min_x, field->cell_size_m, cx_min, cx_max);
        score.center.y =
            SnapCutToCell(raw.y, field->bounds.min_y, field->cell_size_m, cy_min, cy_max);
        bool duplicate = false;
        for (const auto& existing : recommendation.candidates) {
            if (existing.center.x == score.center.x && existing.center.y == score.center.y) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        ScoreCandidate(score, input, *field, activity, zone_aggregate, hotspots, config_, now);
        recommendation.candidates.push_back(score);
    }
    if (recommendation.candidates.empty()) {
        return recommendation;
    }

    // Explicit, documented tie-break order:
    //   1. higher final_score
    //   2. lower boundary_penalty
    //   3. earlier position in the fixed candidate list (kind order)
    // No unordered iteration participates.
    std::size_t best = 0;
    for (std::size_t i = 1; i < recommendation.candidates.size(); ++i) {
        const auto& candidate = recommendation.candidates[i];
        const auto& current = recommendation.candidates[best];
        if (candidate.final_score > current.final_score + kScoreEpsilon) {
            best = i;
            continue;
        }
        if (candidate.final_score < current.final_score - kScoreEpsilon) {
            continue;
        }
        if (candidate.boundary_penalty < current.boundary_penalty - kScoreEpsilon) {
            best = i;
        }
    }
    recommendation.best = recommendation.candidates[best];
    recommendation.best_index = best;
    recommendation.baseline = recommendation.candidates[0]; // midpoint is always first
    for (const auto& candidate : recommendation.candidates) {
        if (candidate.kind == SplitCandidateKind::Midpoint) {
            recommendation.baseline = candidate;
            break;
        }
    }
    recommendation.expected_improvement = recommendation.best.final_score;
    recommendation.valid = true;
    return recommendation;
}

std::string FormatPartitionDecision(const PartitionDecisionRecord& record)
{
    char buffer[512];
    const SplitCandidateScore& candidate = record.candidate;
    if (record.executed) {
        std::snprintf(buffer,
                      sizeof(buffer),
                      "SPLIT zone=%u candidate=%s@(%.0f,%.0f) score=%.3f benefit=%.3f "
                      "boundary=%.3f migration=%.3f repl=%.3f instability=%.3f topology=%.3f "
                      "before=%.2f after=%.2f p99=%.1fms field_epoch=%llu",
                      record.zone_id,
                      SplitCandidateKindName(candidate.kind),
                      candidate.center.x,
                      candidate.center.y,
                      candidate.final_score,
                      candidate.balance_benefit,
                      candidate.boundary_penalty,
                      candidate.migration_penalty,
                      candidate.replication_penalty,
                      candidate.instability_penalty,
                      candidate.topology_penalty,
                      candidate.before_total,
                      candidate.after_peak,
                      record.p99_tick_ms,
                      static_cast<unsigned long long>(record.field_epoch));
    } else {
        std::snprintf(buffer,
                      sizeof(buffer),
                      "NOOP zone=%u reason=%s detail=%s load=%.3f field=%.3f p99=%.1fms "
                      "best=%s@(%.0f,%.0f) score=%.3f benefit=%.3f boundary=%.3f "
                      "migration=%.3f repl=%.3f instability=%.3f topology=%.3f",
                      record.zone_id,
                      PartitionNoopReasonName(record.noop_reason),
                      record.detail != nullptr ? record.detail : "",
                      record.load_score,
                      record.field_load_score,
                      record.p99_tick_ms,
                      SplitCandidateKindName(candidate.kind),
                      candidate.center.x,
                      candidate.center.y,
                      candidate.final_score,
                      candidate.balance_benefit,
                      candidate.boundary_penalty,
                      candidate.migration_penalty,
                      candidate.replication_penalty,
                      candidate.instability_penalty,
                      candidate.topology_penalty);
    }
    return std::string(buffer);
}

} // namespace gs::game
