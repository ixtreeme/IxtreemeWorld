#include "ReplicationValidator.h"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <unordered_set>
#include <vector>

#include "../components/MovementComponents.h"
#include "../components/NetworkComponents.h"
#include "../components/ReplicationComponents.h"
#include "../components/Tags.h"
#include "../components/TransformComponents.h"
#include "../spatial/AoiSystem.h"
#include "../zone/Zone.h"
#include "../zone/ZoneManager.h"
#include "ProtocolEncoder.h"
#include "ReplicationConfig.h"

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
                               const ReplicationConfig& config,
                               std::string& out_error,
                               std::size_t* out_viewers_checked,
                               std::size_t* out_relationships_checked,
                               std::size_t* out_records_checked)
{
    std::size_t viewers_checked = 0;
    std::size_t relationships_checked = 0;
    std::size_t records_checked = 0;

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

            // (5) Ordered AOI equivalence: run the production index query for
            // this exact viewer and compare the ordered top-k with the
            // brute-force reference element by element. Metrics are disabled
            // so the audit cannot pollute what it validates.
            {
                const auto& actual = AoiSystem::QueryCandidates(zone,
                                                                viewer_net_id,
                                                                viewer_position,
                                                                config.aoi_partial_cap,
                                                                false,
                                                                config.aoi_nth_element,
                                                                false);
                if (actual.size() != candidates.size()) {
                    std::ostringstream message;
                    message << "zone " << zone.Id() << " viewer " << viewer_net_id
                            << ": AOI candidate count " << actual.size()
                            << " != exact top-k " << candidates.size();
                    out_error = message.str();
                    return false;
                }
                for (std::size_t ci = 0; ci < actual.size(); ++ci) {
                    if (actual[ci].net_id != candidates[ci].net_id ||
                        actual[ci].distance_sq != candidates[ci].distance_sq) {
                        std::ostringstream message;
                        message << "zone " << zone.Id() << " viewer " << viewer_net_id
                                << ": AOI candidate " << ci << " mismatch (index net "
                                << actual[ci].net_id << " d2=" << actual[ci].distance_sq
                                << ", exact net " << candidates[ci].net_id
                                << " d2=" << candidates[ci].distance_sq << ")";
                        out_error = message.str();
                        return false;
                    }
                }
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
                    const auto missing_entity = zone.FindEntity(net_id);
                    float stored_x = 0.0f;
                    float stored_y = 0.0f;
                    float stored_z = 0.0f;
                    const bool stored = zone.Grid().DebugStoredPosition(net_id, stored_x,
                                                                        stored_y, stored_z);
                    message << "zone " << zone.Id() << " viewer " << viewer_net_id
                            << ": missing interest net " << net_id << " (exact AOI includes it)"
                            << " viewer=(" << viewer_position.x << ", " << viewer_position.y
                            << ") grid_stored=";
                    if (stored) {
                        message << "(" << stored_x << ", " << stored_y << ", " << stored_z << ")";
                    } else {
                        message << "absent";
                    }
                    if (missing_entity.is_valid() && missing_entity.has<Position>()) {
                        const auto pos = missing_entity.get<Position>();
                        message << " authority=(" << pos.x << ", " << pos.y << ", " << pos.z << ")";
                    } else {
                        message << " authority=missing";
                    }
                    out_error = message.str();
                    return false;
                }
            }
        }

        // (3) Canonical shared-record audit (phase 5C; populated only when
        // the audit retention flag is on). The retained 19-byte records were
        // serialized once and shared by many recipients; each must still
        // match the entity's current transform state. A stale cached payload
        // (wrong version reused) shows up here as a field mismatch.
        const auto& audit_records = zone.LastReplicationAuditRecords();
        for (const TransformRecord& record : audit_records) {
            ++records_checked;
            std::uint32_t net_id = 0;
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
            std::memcpy(&net_id, record.data(), sizeof(net_id));
            std::memcpy(&x, record.data() + 4, sizeof(x));
            std::memcpy(&y, record.data() + 8, sizeof(y));
            std::memcpy(&z, record.data() + 12, sizeof(z));
            const std::uint16_t heading_q =
                static_cast<std::uint16_t>(record[16] | (record[17] << 8));
            const auto move_state = static_cast<MoveState>(record[18]);

            const auto entity = zone.FindEntity(net_id);
            if (entity.is_valid()) {
                const auto position = entity.get<Position>();
                const auto heading = entity.get<Heading>();
                const auto intent = entity.get<MoveIntent>();
                if (position.x != x || position.y != y || position.z != z ||
                    QuantizeHeading(heading.angle) != heading_q || intent.state != move_state) {
                    std::ostringstream message;
                    message << "zone " << zone.Id() << ": shared transform record for net " << net_id
                            << " does not match authority (record=(" << x << ", " << y << ", " << z
                            << ") hq=" << heading_q << " state=" << static_cast<int>(move_state)
                            << ", authority=(" << position.x << ", " << position.y << ", "
                            << position.z << ") hq=" << QuantizeHeading(heading.angle)
                            << " state=" << static_cast<int>(intent.state) << ")";
                    out_error = message.str();
                    return false;
                }
            } else if (const GhostRecord* ghost = zone.FindGhost(net_id)) {
                if (ghost->snapshot.position.x != x || ghost->snapshot.position.y != y ||
                    ghost->snapshot.position.z != z ||
                    QuantizeHeading(ghost->snapshot.heading.angle) != heading_q ||
                    ghost->snapshot.move_state != move_state) {
                    std::ostringstream message;
                    message << "zone " << zone.Id() << ": shared transform record for ghost net "
                            << net_id << " does not match the ghost snapshot";
                    out_error = message.str();
                    return false;
                }
            }
            // A net that no longer resolves was valid at its tick; migrations
            // and despawns after it are covered by the interest/coverage
            // checks above.
        }
    }

    if (out_viewers_checked != nullptr) {
        *out_viewers_checked = viewers_checked;
    }
    if (out_relationships_checked != nullptr) {
        *out_relationships_checked = relationships_checked;
    }
    if (out_records_checked != nullptr) {
        *out_records_checked = records_checked;
    }
    out_error.clear();
    return true;
}

} // namespace gs::game
