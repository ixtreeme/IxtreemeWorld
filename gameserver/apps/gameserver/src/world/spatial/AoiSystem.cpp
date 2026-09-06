#include "AoiSystem.h"

#include <algorithm>
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
thread_local std::vector<std::pair<float, std::uint32_t>> t_candidates;
thread_local std::vector<std::uint32_t> t_results;

const Position* FindGhostPosition(const GhostPositionCache& ghosts, std::uint32_t net_id)
{
    // Ghost sets are small (border-band residents of neighbors); a sorted
    // vector with binary search beats a per-query hash map build.
    std::size_t lo = 0;
    std::size_t hi = ghosts.size();
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (ghosts[mid].first < net_id) {
            lo = mid + 1;
        } else if (ghosts[mid].first > net_id) {
            hi = mid;
        } else {
            return &ghosts[mid].second;
        }
    }
    return nullptr;
}

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
        [&](const NetId& id, const Position& pos) {
            if (ghost_nets.contains(id.value)) {
                return;
            }
            grid.Insert(id.value, pos);
        });
    for (const auto& ghost : zone.Ghosts()) {
        grid.Insert(ghost.snapshot.net_id, ghost.snapshot.position);
    }
}

GhostPositionCache AoiSystem::BuildGhostCache(const Zone& zone)
{
    GhostPositionCache cache;
    cache.reserve(zone.Ghosts().size());
    for (const auto& ghost : zone.Ghosts()) {
        cache.emplace_back(ghost.snapshot.net_id, ghost.snapshot.position);
    }
    std::sort(cache.begin(), cache.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.first < rhs.first;
    });
    return cache;
}

std::vector<std::uint32_t> AoiSystem::QueryCandidates(Zone& zone,
                                                      std::uint32_t viewer_net_id,
                                                      const Position& viewer_position,
                                                      const GhostPositionCache& ghosts)
{
    AssertZoneOwner(zone, "zone AOI query");

    auto& candidates = t_candidates;
    candidates.clear();

    zone.Grid().ForEachInRadius(viewer_position, kAoiRadiusMeters, [&](std::uint32_t net_id) {
        if (net_id == 0 || net_id == viewer_net_id) {
            return;
        }

        const Position* candidate_position = nullptr;
        Position resident_position;
        const auto entity = zone.FindEntity(net_id);
        if (entity.is_valid()) {
            resident_position = entity.get<Position>();
            candidate_position = &resident_position;
        } else {
            candidate_position = FindGhostPosition(ghosts, net_id);
        }
        if (candidate_position == nullptr) {
            return;
        }

        const float dx = candidate_position->x - viewer_position.x;
        const float dy = candidate_position->y - viewer_position.y;
        const float distance_sq = dx * dx + dy * dy;
        if (distance_sq <= kAoiRadiusSqMeters) {
            candidates.emplace_back(distance_sq, net_id);
        }
    });

    std::sort(candidates.begin(), candidates.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.first < rhs.first;
    });
    if (candidates.size() > kAoiEntityCap) {
        candidates.resize(kAoiEntityCap);
    }

    std::uint64_t tier_near = 0;
    std::uint64_t tier_mid = 0;
    std::uint64_t tier_far = 0;
    auto& refs = t_results;
    refs.clear();
    refs.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        refs.push_back(candidate.second);
        switch (RelevanceTierForDistanceSq(candidate.first)) {
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
    zone.Diagnostics().tier_near_since_diag.fetch_add(tier_near, std::memory_order_relaxed);
    zone.Diagnostics().tier_mid_since_diag.fetch_add(tier_mid, std::memory_order_relaxed);
    zone.Diagnostics().tier_far_since_diag.fetch_add(tier_far, std::memory_order_relaxed);
    return refs;
}

} // namespace gs::game
