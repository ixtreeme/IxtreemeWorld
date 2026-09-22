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
        // Null/disabled field -> 0, legacy behavior preserved. The slow EMA
        // view is recorded separately: merge eligibility is conservative and
        // requires BOTH timescales to be calm (phase-3 §11).
        if (load_field != nullptr && load_field->enabled) {
            const auto& bounds = zone.Bounds();
            const WorldBounds zone_bounds{bounds.min_x, bounds.min_y, bounds.max_x, bounds.max_y};
            const LoadAggregate aggregate =
                AggregateLoad(*load_field, zone_bounds, config_.field_timescale);
            const LoadAggregate slow_aggregate =
                AggregateLoad(*load_field, zone_bounds, LoadTimescale::Slow);
            snap.field_peak_cell = aggregate.composite_peak;
            snap.field_active_cells = aggregate.active_cells;
            const float mean =
                aggregate.cells > 0
                    ? aggregate.composite_sum / static_cast<float>(aggregate.cells)
                    : 0.0f;
            const float slow_mean =
                slow_aggregate.cells > 0
                    ? slow_aggregate.composite_sum / static_cast<float>(slow_aggregate.cells)
                    : 0.0f;
            snap.field_load_score = ClampScore(std::max(mean, aggregate.composite_peak), 1.0f);
            snap.field_load_score_slow =
                ClampScore(std::max(slow_mean, slow_aggregate.composite_peak), 1.0f);
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
    }

    // --- merge group observation (phase 3) ---------------------------------
    // Deterministic DFS over the partition forests: a merge candidate is a
    // REAL quadtree parent with exactly four authoritative active leaves.
    // No arbitrary-neighbor or partial-group merges.
    merge_groups_.clear();
    merge_candidates_.clear();
    std::vector<ZonePartition*> internal_nodes;
    for (const auto& root : zones.PartitionRoots()) {
        CollectInternalNodes(root.get(), internal_nodes);
    }
    merge_groups_.reserve(internal_nodes.size());
    for (ZonePartition* parent : internal_nodes) {
        if (parent->children.size() != 4) {
            continue; // quadtree groups only; the executor re-validates
        }
        MergeGroupSnapshot group;
        group.parent_id = parent->zone_id;
        float max_child = 0.0f;
        bool children_ok = true;
        for (std::size_t i = 0; i < parent->children.size(); ++i) {
            const ZonePartition* child = parent->children[i].get();
            group.child_ids[i] = child->zone_id;
            max_child = std::max(max_child, child->load_score);
            if (!child->IsActiveLeaf()) {
                children_ok = false;
            }
        }
        group.max_child_load_score = max_child;

        // Whole-area aggregate on both field timescales. A group is only
        // eligible when EVERY child AND the aggregate are below the merge
        // threshold; one hot child keeps the whole group out.
        if (children_ok && load_field != nullptr && load_field->enabled) {
            const WorldBounds bounds{parent->bounds.min_x, parent->bounds.min_y,
                                     parent->bounds.max_x, parent->bounds.max_y};
            const LoadAggregate fast = AggregateLoad(*load_field, bounds, config_.field_timescale);
            const LoadAggregate slow = AggregateLoad(*load_field, bounds, LoadTimescale::Slow);
            const auto score_of = [](const LoadAggregate& aggregate) {
                const float mean =
                    aggregate.cells > 0
                        ? aggregate.composite_sum / static_cast<float>(aggregate.cells)
                        : 0.0f;
                return std::max(mean, aggregate.composite_peak);
            };
            group.parent_field_fast = ClampScore(score_of(fast), 1.0f);
            group.parent_field_slow = ClampScore(score_of(slow), 1.0f);
        }
        group.group_load_score =
            ClampScore(std::max({max_child, group.parent_field_fast, group.parent_field_slow}),
                       2.0f);

        // Sustained-low timer: explicit start/keep/reset. Any child breach or
        // aggregate breach resets it; no implicit decay, no hidden semantics.
        if (children_ok && group.group_load_score < config_.merge_load_threshold) {
            if (parent->group_low_since == std::chrono::steady_clock::time_point{}) {
                parent->group_low_since = now;
            }
        } else {
            parent->group_low_since = {};
        }
        if (parent->group_low_since != std::chrono::steady_clock::time_point{}) {
            group.sustained_low_seconds =
                std::chrono::duration<float>(now - parent->group_low_since).count();
        }

        group.gate = scheduler.EvaluateMergeGate(parent, now);
        group.candidate = group.gate == ZoneScheduler::MergeGate::Pass;
        if (group.candidate) {
            merge_candidates_.push_back(parent->zone_id);
        }
        merge_groups_.push_back(group);
    }
}

} // namespace gs::game
