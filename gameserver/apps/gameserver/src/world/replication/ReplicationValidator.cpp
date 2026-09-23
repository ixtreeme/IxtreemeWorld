#include "ReplicationValidator.h"

#include <algorithm>
#include <sstream>
#include <unordered_set>
#include <vector>

#include "../components/NetworkComponents.h"
#include "../components/ReplicationComponents.h"
#include "../components/Tags.h"
#include "../components/TransformComponents.h"
#include "../zone/Zone.h"
#include "../zone/ZoneManager.h"

namespace gs::game {
namespace {

struct ReferenceEntity {
    std::uint32_t net_id = 0;
    float x = 0.0f;
    float y = 0.0f;
};

struct ReferenceCandidate {
    float distance_sq = 0.0f;
    std::uint32_t net_id = 0;
};

std::uint32_t TransformVersionOf(Zone& zone, std::uint32_t net_id, bool* out_has_version)
{
    const auto entity = zone.FindEntity(net_id);
    if (entity.is_valid()) {
        if (out_has_version != nullptr) {
            *out_has_version = entity.has<TransformVersion>();
        }
        return entity.has<TransformVersion>() ? entity.get<TransformVersion>().tick : 0;
    }
    const GhostRecord* ghost = zone.FindGhost(net_id);
    if (ghost != nullptr && ghost->entity.is_valid()) {
        if (out_has_version != nullptr) {
            *out_has_version = ghost->entity.has<TransformVersion>();
        }
        return ghost->entity.has<TransformVersion>() ? ghost->entity.get<TransformVersion>().tick
                                                     : 0;
    }
    if (out_has_version != nullptr) {
        *out_has_version = false;
    }
    return 0;
}

} // namespace

bool ValidateReplicationShadow(ZoneManager& zones,
                               std::string& out_error,
                               std::size_t* out_viewers_checked,
                               std::size_t* out_relationships_checked)
{
    std::size_t viewers_checked = 0;
    std::size_t relationships_checked = 0;

    for (std::size_t i = 0; i < zones.ZoneCount(); ++i) {
        Zone& zone = zones.GetZone(i);
        if (!zone.SimulationEnabled() || zone.Partition() != PartitionState::Leaf) {
            continue;
        }
        if (zone.Players().empty()) {
            continue;
        }

        // Reference universe: exactly what the spatial grid indexes --
        // authoritative residents with a position plus the ghost set.
        std::vector<ReferenceEntity> universe;
        universe.reserve(zone.Entities().size() + zone.Ghosts().size());
        for (const auto& [net_id, entity] : zone.Entities()) {
            if (!entity.is_valid() || !entity.has<Position>()) {
                continue;
            }
            const auto position = entity.get<Position>();
            universe.push_back(ReferenceEntity{net_id, position.x, position.y});
        }
        for (const auto& ghost : zone.Ghosts()) {
            universe.push_back(ReferenceEntity{ghost.snapshot.net_id,
                                               ghost.snapshot.position.x,
                                               ghost.snapshot.position.y});
        }

        std::vector<ReferenceCandidate> candidates;
        candidates.reserve(universe.size());
        std::unordered_set<std::uint32_t> expected;

        for (const auto& [viewer_net_id, binding] : zone.Players()) {
            ++viewers_checked;
            const auto viewer_entity = zone.FindEntity(viewer_net_id);
            if (!viewer_entity.is_valid()) {
                continue;
            }
            const auto viewer_position = viewer_entity.get<Position>();

            // Exact brute-force AOI: same distance test, ordering and cap as
            // the production path -- but computed without the spatial grid.
            candidates.clear();
            for (const auto& entry : universe) {
                if (entry.net_id == 0 || entry.net_id == viewer_net_id) {
                    continue;
                }
                const float dx = entry.x - viewer_position.x;
                const float dy = entry.y - viewer_position.y;
                const float distance_sq = dx * dx + dy * dy;
                if (distance_sq <= kAoiRadiusSqMeters) {
                    candidates.push_back(ReferenceCandidate{distance_sq, entry.net_id});
                }
            }
            std::sort(candidates.begin(),
                      candidates.end(),
                      [](const ReferenceCandidate& lhs, const ReferenceCandidate& rhs) {
                          if (lhs.distance_sq != rhs.distance_sq) {
                              return lhs.distance_sq < rhs.distance_sq;
                          }
                          return lhs.net_id < rhs.net_id;
                      });
            if (candidates.size() > kAoiEntityCap) {
                candidates.resize(kAoiEntityCap);
            }
            expected.clear();
            expected.reserve(candidates.size());
            for (const auto& candidate : candidates) {
                expected.insert(candidate.net_id);
            }

            // (1) identity + coverage for everything the viewer knows.
            for (const auto& [net_id, last_sent] : binding.visible_net_versions) {
                ++relationships_checked;
                if (net_id == viewer_net_id) {
                    std::ostringstream message;
                    message << "zone " << zone.Id() << " viewer " << viewer_net_id
                            << ": interest set contains the viewer itself";
                    out_error = message.str();
                    return false;
                }
                const bool resident = zone.IsResident(net_id);
                const GhostRecord* ghost = zone.FindGhost(net_id);
                if (resident && ghost != nullptr) {
                    std::ostringstream message;
                    message << "zone " << zone.Id() << " viewer " << viewer_net_id
                            << ": net " << net_id
                            << " is both a resident and a ghost (duplicate identity)";
                    out_error = message.str();
                    return false;
                }
                if (!resident && ghost == nullptr) {
                    std::ostringstream message;
                    message << "zone " << zone.Id() << " viewer " << viewer_net_id
                            << ": interest net " << net_id
                            << " does not resolve to a resident or ghost";
                    out_error = message.str();
                    return false;
                }
                bool has_version = false;
                const std::uint32_t current = TransformVersionOf(zone, net_id, &has_version);
                if (!has_version) {
                    std::ostringstream message;
                    message << "zone " << zone.Id() << " viewer " << viewer_net_id
                            << ": visible net " << net_id
                            << " has no TransformVersion (creation path gap)";
                    out_error = message.str();
                    return false;
                }
                if (last_sent < current) {
                    std::ostringstream message;
                    message << "zone " << zone.Id() << " viewer " << viewer_net_id
                            << ": stale recipient state for net " << net_id << " (last_sent="
                            << last_sent << " current=" << current << ")";
                    out_error = message.str();
                    return false;
                }
                if (!expected.contains(net_id)) {
                    std::ostringstream message;
                    message << "zone " << zone.Id() << " viewer " << viewer_net_id
                            << ": extra interest net " << net_id << " (exact AOI excludes it)";
                    out_error = message.str();
                    return false;
                }
            }
            // (2) every exact AOI member must be in the interest set.
            for (const std::uint32_t net_id : expected) {
                if (binding.visible_net_versions.find(net_id) ==
                    binding.visible_net_versions.end()) {
                    std::ostringstream message;
                    message << "zone " << zone.Id() << " viewer " << viewer_net_id
                            << ": missing interest net " << net_id << " (exact AOI includes it)";
                    out_error = message.str();
                    return false;
                }
            }
        }
    }

    if (out_viewers_checked != nullptr) {
        *out_viewers_checked = viewers_checked;
    }
    if (out_relationships_checked != nullptr) {
        *out_relationships_checked = relationships_checked;
    }
    out_error.clear();
    return true;
}

} // namespace gs::game
