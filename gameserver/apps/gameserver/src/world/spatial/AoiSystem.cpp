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

namespace gs::game {

void AoiSystem::RebuildIndex(Zone& zone)
{
    AssertZoneOwner(zone, "zone spatial grid rebuild");

    auto& grid = zone.Grid();
    grid.Clear();

    for (const auto& [net_id, binding] : zone.Players()) {
        const auto entity = zone.FindEntity(net_id);
        if (!entity.is_valid()) {
            continue;
        }
        grid.Insert(net_id, entity.get<Position>());
    }
    // Ghosts with MobTag also match the query below, but they are indexed
    // separately afterwards; skipping them here keeps every net_id in the
    // grid exactly once (as before).
    std::unordered_set<std::uint32_t> ghost_nets;
    ghost_nets.reserve(zone.Ghosts().size());
    for (const auto& ghost : zone.Ghosts()) {
        ghost_nets.insert(ghost.snapshot.net_id);
    }
    zone.World().query<const MobTag, const NetId, const Position>().each(
        [&](const MobTag&, const NetId& id, const Position& pos) {
            if (ghost_nets.contains(id.value)) {
                return;
            }
            grid.Insert(id.value, pos);
        });
    for (const auto& ghost : zone.Ghosts()) {
        grid.Insert(ghost.snapshot.net_id, ghost.snapshot.position);
    }
}

std::vector<std::uint32_t> AoiSystem::QueryCandidates(Zone& zone,
                                                      std::uint32_t viewer_net_id,
                                                      const Position& viewer_position)
{
    AssertZoneOwner(zone, "zone AOI query");

    struct Candidate {
        float distance_sq = std::numeric_limits<float>::max();
        std::uint32_t net_id = 0;
    };

    // Ghost positions by net_id for index-time resolution.
    std::unordered_map<std::uint32_t, Position> ghost_positions;
    ghost_positions.reserve(zone.Ghosts().size());
    for (const auto& ghost : zone.Ghosts()) {
        ghost_positions[ghost.snapshot.net_id] = ghost.snapshot.position;
    }

    std::vector<Candidate> candidates;
    zone.Grid().ForEachInRadius(viewer_position, kAoiRadiusMeters, [&](std::uint32_t net_id) {
        if (net_id == 0 || net_id == viewer_net_id) {
            return;
        }

        std::optional<Position> candidate_position;
        const auto entity = zone.FindEntity(net_id);
        if (entity.is_valid()) {
            candidate_position = entity.get<Position>();
        } else {
            const auto ghost_it = ghost_positions.find(net_id);
            if (ghost_it != ghost_positions.end()) {
                candidate_position = ghost_it->second;
            }
        }
        if (!candidate_position) {
            return;
        }

        const float dx = candidate_position->x - viewer_position.x;
        const float dy = candidate_position->y - viewer_position.y;
        const float distance_sq = dx * dx + dy * dy;
        if (distance_sq <= kAoiRadiusSqMeters) {
            candidates.push_back(Candidate{distance_sq, net_id});
        }
    });

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& lhs, const Candidate& rhs) {
        return lhs.distance_sq < rhs.distance_sq;
    });
    if (candidates.size() > kAoiEntityCap) {
        candidates.resize(kAoiEntityCap);
    }

    std::vector<std::uint32_t> refs;
    refs.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        refs.push_back(candidate.net_id);
    }
    return refs;
}

} // namespace gs::game
