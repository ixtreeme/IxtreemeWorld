#include "ZoneLoadMonitor.h"

#include <algorithm>

#include "../activity/ContinuousLoadField.h"
#include "../zone/Zone.h"
#include "../zone/ZoneDiagnostics.h"
#include "../zone/ZoneManager.h"
#include "../zone/ZoneScheduler.h"

namespace gs::game {

namespace {

float ClampScore(float value, float upper) noexcept
{
    if (!std::isfinite(value) || value <= 0.0f) {
        return 0.0f;
    }
    return std::min(value, upper);
}

} // namespace

ZoneLoadMonitor::ZoneLoadMonitor(Config config)
    : config_(config)
{
    tick_scratch_.resize(ZoneDiagnostics::kTickSampleCapacity, 0u);
}

void ZoneLoadMonitor::Update(ZoneManager& zones,
                             const ZoneScheduler& scheduler,
                             std::chrono::steady_clock::time_point now,
                             const std::shared_ptr<const LoadGrid>& load_field)
{
    split_candidates_.clear();
    merge_candidates_.clear();
    overloaded_leaves_.clear();
    snapshots_.clear();

    const auto active_leaves = zones.GetActiveLeaves();
    snapshots_.reserve(active_leaves.size());

    for (ZonePartition* leaf : active_leaves) {
        const std::size_t zone_index = zones.FindIndexById(leaf->zone_id);
        if (zone_index >= zones.ZoneCount()) {
            continue;
        }
        const auto& zone = zones.GetZone(zone_index);
        const auto& diag = zone.Diagnostics();

        ZoneLoadSnapshot snap;
        snap.zone_id = leaf->zone_id;
        snap.players = diag.player_count.load(std::memory_order_relaxed);
        snap.mobs = diag.mob_count.load(std::memory_order_relaxed);
        const std::uint64_t ticks = diag.ticks_since_diag.load(std::memory_order_relaxed);
        snap.avg_tick_us =
            ticks > 0
                ? diag.tick_micros_since_diag.load(std::memory_order_relaxed) / ticks
                : 0;
        snap.timestamp = now;

        // p95/p99 from the zone's 256-sample tick ring. Only meaningful when
        // the zone actually ticked in the last diagnostic window: a sleeping
        // zone's stale samples must not resurrect an old overload.
        if (ticks > 0) {
            const std::size_t count =
                diag.CopyTickSamples(tick_scratch_.data(), tick_scratch_.size());
            if (count > 0) {
                std::sort(tick_scratch_.begin(), tick_scratch_.begin() + count);
                const auto quantile = [&](double p) {
                    const std::size_t index = std::min(
                        count - 1, static_cast<std::size_t>(std::floor(p * count)));
                    return static_cast<float>(tick_scratch_[index]);
                };
                snap.p95_tick_us = quantile(0.95);
                snap.p99_tick_us = quantile(0.99);
            }
        }

        // Legacy score: budget pressure from avg tick, p99 tail (a zone whose
        // average hides its tail is still overloaded -- the documented
        // hotspot evidence), and residents. p99 is the split-relevant tail.
        const float tick_frac =
            static_cast<float>(snap.avg_tick_us) / 1000.0f / config_.tick_budget_ms;
        const float tick_frac_p99 =
            ticks > 0 ? snap.p99_tick_us / 1000.0f / config_.tick_budget_ms : 0.0f;
        const float resident_frac =
            static_cast<float>(snap.players + snap.mobs) / config_.resident_budget;
        snap.legacy_load_score =
            ClampScore(std::max({tick_frac, tick_frac_p99, resident_frac}), 2.0f);

        // Field score: world-space load distribution under the zone bounds.
        // Null/disabled field -> 0, legacy behavior preserved.
        if (load_field != nullptr && load_field->enabled) {
            const auto& bounds = zone.Bounds();
            const WorldBounds zone_bounds{bounds.min_x, bounds.min_y, bounds.max_x, bounds.max_y};
            const LoadAggregate aggregate =
                AggregateLoad(*load_field, zone_bounds, config_.field_timescale);
            snap.field_peak_cell = aggregate.composite_peak;
            snap.field_active_cells = aggregate.active_cells;
            const float mean =
                aggregate.cells > 0
                    ? aggregate.composite_sum / static_cast<float>(aggregate.cells)
                    : 0.0f;
            snap.field_load_score = ClampScore(std::max(mean, aggregate.composite_peak), 1.0f);
        }

        snap.load_score = ClampScore(std::max(snap.legacy_load_score, snap.field_load_score), 2.0f);

        leaf->legacy_load_score = snap.legacy_load_score;
        leaf->field_load_score = snap.field_load_score;
        leaf->field_peak_score = snap.field_peak_cell;
        leaf->p95_tick_us = snap.p95_tick_us;
        leaf->p99_tick_us = snap.p99_tick_us;
        leaf->load_score = snap.load_score;
        snapshots_.push_back(snap);

        // Sustained-breach timer: start on first breach, clear when healthy.
        if (snap.load_score >= config_.split_load_threshold) {
            if (leaf->sustained_breach_since == std::chrono::steady_clock::time_point{}) {
                leaf->sustained_breach_since = now;
            }
            overloaded_leaves_.push_back(leaf->zone_id);
        } else {
            leaf->sustained_breach_since = {};
        }

        // Merge sustained-low seam (recorded, not yet gating): how long the
        // combined score has stayed below the merge threshold.
        if (snap.load_score < config_.merge_load_threshold) {
            if (leaf->field_low_since == std::chrono::steady_clock::time_point{}) {
                leaf->field_low_since = now;
            }
        } else {
            leaf->field_low_since = {};
        }

        if (scheduler.ShouldSplit(leaf, now)) {
            split_candidates_.push_back(leaf->zone_id);
        }
        if (scheduler.ShouldMerge(leaf, now) && leaf->parent != nullptr) {
            merge_candidates_.push_back(leaf->parent->zone_id);
        }
    }

    // One merge candidate per parent: siblings are evaluated per leaf, so
    // the same parent can appear up to 4x. Execution merges the whole
    // sibling set once; dedupe here.
    std::sort(merge_candidates_.begin(), merge_candidates_.end());
    merge_candidates_.erase(std::unique(merge_candidates_.begin(), merge_candidates_.end()),
                            merge_candidates_.end());
}

} // namespace gs::game
