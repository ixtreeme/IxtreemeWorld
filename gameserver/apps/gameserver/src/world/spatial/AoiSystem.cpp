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
                                                            bool partial_cap)
{
    AssertZoneOwner(zone, "zone AOI query");
    const auto aoi_start = std::chrono::steady_clock::now();

    auto& candidates = t_candidates;
    candidates.clear();

    // PREFILTER: the grid cell scan is a strict superset of the AOI disc
    // (cell size == AOI radius, 3x3 neighborhood). EXACT FILTER: the squared
    // distance check below -- only a candidate that passes is ever visible,
    // so the prefilter can never cause a false negative.
    zone.Grid().ForEachInRadius(viewer_position, kAoiRadiusMeters, [&](const GridEntry& entry) {
        const std::uint32_t net_id = entry.net_id;
        if (net_id == 0 || net_id == viewer_net_id) {
            return;
        }

        // The grid entry carries the zone-local entity handle, so the radius
        // scan never pays a random hash lookup per candidate: only the
        // position component read remains. Ghost entities carry their
        // reconciled Position the same way.
        if (!entry.entity.is_valid() || !entry.entity.has<Position>()) {
            return;
        }
        const auto position = entry.entity.get<Position>();
        const float dx = position.x - viewer_position.x;
        const float dy = position.y - viewer_position.y;
        const float distance_sq = dx * dx + dy * dy;
        if (distance_sq <= kAoiRadiusSqMeters) {
            candidates.push_back(AoiCandidate{distance_sq, net_id, entry.entity});
        }
    });

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
    diag.aoi_candidates_pre_cap_since_diag.fetch_add(pre_cap, std::memory_order_relaxed);

    // Deterministic order: nearest first, NetId tie-break. Both the top-k
    // reduction and the full sort select exactly the same capped set.
    const auto by_distance = [](const AoiCandidate& lhs, const AoiCandidate& rhs) {
        if (lhs.distance_sq != rhs.distance_sq) {
            return lhs.distance_sq < rhs.distance_sq;
        }
        return lhs.net_id < rhs.net_id;
    };
    if (candidates.size() > kAoiEntityCap && partial_cap) {
        std::partial_sort(candidates.begin(),
                          candidates.begin() + static_cast<std::ptrdiff_t>(kAoiEntityCap),
                          candidates.end(),
                          by_distance);
        candidates.resize(kAoiEntityCap);
    } else {
        std::sort(candidates.begin(), candidates.end(), by_distance);
        if (candidates.size() > kAoiEntityCap) {
            candidates.resize(kAoiEntityCap);
        }
    }
    diag.aoi_candidates_post_cap_since_diag.fetch_add(candidates.size(),
                                                      std::memory_order_relaxed);

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
    diag.aoi_micros_since_diag.fetch_add(
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - aoi_start)
                .count()),
        std::memory_order_relaxed);
    return refs;
}

} // namespace gs::game
