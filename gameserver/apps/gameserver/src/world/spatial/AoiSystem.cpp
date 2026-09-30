#include "AoiSystem.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <optional>
#include <unordered_set>

#include "../WorldConstants.h"
#include "../components/MobComponents.h"
#include "../components/NetworkComponents.h"
#include "../components/Tags.h"
#include "../zone/Zone.h"
#include "../zone/ZoneOwnership.h"
#include "SpatialGrid.h"

namespace gs::game {
namespace {

// Scratch buffers reused across queries on the same worker thread: the pool
// threads are long-lived, so thread_locals eliminate per-viewer heap churn
// without any locking. Zone-local work never migrates threads mid-tick.
thread_local std::vector<AoiCandidate> t_candidates;
thread_local std::vector<AoiCandidate> t_results;

} // namespace

void AoiSystem::RebuildInto(Zone& zone, SpatialGrid& grid)
{
    AssertZoneOwner(zone, "zone spatial grid rebuild");

    grid.Clear();

    // Ship-ready: every authoritative resident with an identity and a
    // position is indexed, regardless of entity kind (player/mob/ship/...).
    // Ghosts are indexed separately below so each net appears exactly once.
    std::unordered_set<std::uint32_t> ghost_nets;
    ghost_nets.reserve(zone.Ghosts().size());
    for (const auto& ghost : zone.Ghosts()) {
        ghost_nets.insert(ghost.snapshot.net_id);
    }
    zone.World().query<const NetId, const Position>().each(
        [&](flecs::entity entity, const NetId& id, const Position& pos) {
            if (ghost_nets.contains(id.value)) {
                return;
            }
            grid.Insert(id.value, pos, entity);
        });
    for (const auto& ghost : zone.Ghosts()) {
        grid.Insert(ghost.snapshot.net_id, ghost.snapshot.position, ghost.entity);
    }
}

const std::vector<AoiCandidate>& AoiSystem::QueryCandidates(Zone& zone,
                                                            std::uint32_t viewer_net_id,
                                                            const Position& viewer_position,
                                                            bool partial_cap,
                                                            bool reference_positions,
                                                            bool nth_element,
                                                            bool count_metrics)
{
    // Read-only query: no ownership assert here (the caller guarantees the
    // context), so the quiescent-window shadow validator can run the same
    // exact query against the production index.
    const auto aoi_start = std::chrono::steady_clock::now();

    auto& candidates = t_candidates;
    candidates.clear();

    std::uint64_t cells_visited = 0;
    std::uint64_t entries_visited = 0;
    std::uint64_t exact_checks = 0;
    // PREFILTER: the grid cell scan is a strict superset of the AOI disc
    // (cell size == AOI radius, 3x3 neighborhood). EXACT FILTER: the squared
    // distance check below -- only a candidate that passes is ever visible,
    // so the prefilter can never cause a false negative.
    zone.Grid().ForEachInRadius(
        viewer_position,
        kAoiRadiusMeters,
        [&](const GridEntry& entry) {
            ++entries_visited;
            const std::uint32_t net_id = entry.net_id;
            if (net_id == 0 || net_id == viewer_net_id) {
                return;
            }
            // Optimized path (phase 5D): the grid entry carries the synced
            // position, so the radius scan is a sequential read with no
            // per-candidate component lookup. The reference path reads the
            // authoritative component (pre-5D behavior) for the A/B run.
            float px = entry.x;
            float py = entry.y;
            if (reference_positions) {
                if (!entry.entity.is_valid() || !entry.entity.has<Position>()) {
                    return;
                }
                const auto position = entry.entity.get<Position>();
                px = position.x;
                py = position.y;
            }
            ++exact_checks;
            const float dx = px - viewer_position.x;
            const float dy = py - viewer_position.y;
            const float distance_sq = dx * dx + dy * dy;
            if (distance_sq <= kAoiRadiusSqMeters) {
                candidates.push_back(AoiCandidate{distance_sq, net_id, entry.entity});
            }
        },
        &cells_visited);

    const std::uint64_t index_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - aoi_start)
            .count());
    const auto topk_start = std::chrono::steady_clock::now();
    const std::size_t pre_cap = candidates.size();

    // Load field attribution: the AOI cost of this viewer is one query plus
    // every candidate considered BEFORE the cap -- so a dense hotspot reads
    // as more expensive than the same entities spread out, and cap clipping
    // stays visible instead of hiding work (§10).
    if (auto* load = zone.LoadBins().CellFor(viewer_position.x, viewer_position.y)) {
        ++load->aoi_queries;
        load->aoi_candidates += static_cast<std::uint32_t>(
            pre_cap > 0xFFFFFFFFu ? 0xFFFFFFFFu : pre_cap);
    }
    auto& diag = zone.Diagnostics();
    if (count_metrics) {
        diag.aoi_candidates_pre_cap_since_diag.fetch_add(pre_cap, std::memory_order_relaxed);
        diag.aoi_cells_visited_since_diag.fetch_add(cells_visited, std::memory_order_relaxed);
        diag.aoi_entries_visited_since_diag.fetch_add(entries_visited, std::memory_order_relaxed);
        diag.aoi_exact_checks_since_diag.fetch_add(exact_checks, std::memory_order_relaxed);
        diag.aoi_index_us_since_diag.fetch_add(index_us, std::memory_order_relaxed);
    }

    // Deterministic order: nearest first, NetId tie-break. Both the top-k
    // reduction and the full sort select exactly the same capped set.
    const auto by_distance = [](const AoiCandidate& lhs, const AoiCandidate& rhs) {
        if (lhs.distance_sq != rhs.distance_sq) {
            return lhs.distance_sq < rhs.distance_sq;
        }
        return lhs.net_id < rhs.net_id;
    };
    if (candidates.size() > kAoiEntityCap && partial_cap) {
        const auto cap_end =
            candidates.begin() + static_cast<std::ptrdiff_t>(kAoiEntityCap);
        if (nth_element) {
            // O(n) selection + sort of the selected prefix: identical
            // ordered top-k to partial_sort (same comparator), less work.
            std::nth_element(candidates.begin(), cap_end, candidates.end(), by_distance);
            std::sort(candidates.begin(), cap_end, by_distance);
        } else {
            std::partial_sort(candidates.begin(), cap_end, candidates.end(), by_distance);
        }
        candidates.resize(kAoiEntityCap);
    } else {
        std::sort(candidates.begin(), candidates.end(), by_distance);
        if (candidates.size() > kAoiEntityCap) {
            candidates.resize(kAoiEntityCap);
        }
    }
    if (count_metrics) {
        diag.aoi_candidates_post_cap_since_diag.fetch_add(candidates.size(),
                                                          std::memory_order_relaxed);
    }

    std::uint64_t tier_near = 0;
    std::uint64_t tier_mid = 0;
    std::uint64_t tier_far = 0;
    auto& refs = t_results;
    refs.clear();
    refs.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        refs.push_back(candidate);
        switch (RelevanceTierForDistanceSq(candidate.distance_sq)) {
        case RelevanceTier::Near:
            ++tier_near;
            break;
        case RelevanceTier::Mid:
            ++tier_mid;
            break;
        case RelevanceTier::Far:
            ++tier_far;
            break;
        }
    }
    diag.tier_near_since_diag.fetch_add(tier_near, std::memory_order_relaxed);
    diag.tier_mid_since_diag.fetch_add(tier_mid, std::memory_order_relaxed);
    diag.tier_far_since_diag.fetch_add(tier_far, std::memory_order_relaxed);
    diag.aoi_topk_us_since_diag.fetch_add(
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - topk_start)
                .count()),
        std::memory_order_relaxed);
    diag.aoi_micros_since_diag.fetch_add(
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - aoi_start)
                .count()),
        std::memory_order_relaxed);
    return refs;
}

} // namespace gs::game
