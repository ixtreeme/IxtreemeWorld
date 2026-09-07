#include "ZoneLoadMonitor.h"

#include <algorithm>

#include "../zone/Zone.h"
#include "../zone/ZoneDiagnostics.h"
#include "../zone/ZoneManager.h"
#include "../zone/ZoneScheduler.h"

namespace gs::game {

ZoneLoadMonitor::ZoneLoadMonitor(Config config)
    : config_(config)
{
}

void ZoneLoadMonitor::Update(ZoneManager& zones,
                             const ZoneScheduler& scheduler,
                             std::chrono::steady_clock::time_point now)
{
    split_candidates_.clear();
    merge_candidates_.clear();
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
        snap.avg_tick_us = ticks > 0 ? diag.tick_micros_since_diag.load(std::memory_order_relaxed) / ticks
                                     : 0;
        snap.timestamp = now;

        // Normalized score: simulation budget pressure dominates, resident
        // pressure contributes. p99 histograms are a documented Phase-3
        // upgrade; avg tick is what ZoneDiagnostics publishes today.
        const float tick_frac =
            static_cast<float>(snap.avg_tick_us) / 1000.0f / config_.tick_budget_ms;
        const float resident_frac =
            static_cast<float>(snap.players + snap.mobs) / config_.resident_budget;
        snap.load_score = std::clamp(std::max(tick_frac, resident_frac), 0.0f, 2.0f);

        leaf->load_score = snap.load_score;
        snapshots_.push_back(snap);

        // Sustained-breach timer: start on first breach, clear when healthy.
        if (snap.load_score >= config_.split_load_threshold) {
            if (leaf->sustained_breach_since == std::chrono::steady_clock::time_point{}) {
                leaf->sustained_breach_since = now;
            }
        } else {
            leaf->sustained_breach_since = {};
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
