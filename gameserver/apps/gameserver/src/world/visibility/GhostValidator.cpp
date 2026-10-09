#include "GhostValidator.h"

#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../components/MobComponents.h"
#include "../components/NetworkComponents.h"
#include "../components/Tags.h"
#include "../components/TransformComponents.h"
#include "../replication/SnapshotBuilder.h"
#include "../spatial/SpatialTypes.h"
#include "../zone/Zone.h"
#include "../zone/ZoneManager.h"
#include "BorderSnapshot.h"

namespace gs::game {

namespace {

struct ExpectedGhost {
    BorderEntitySnapshot snapshot;
    std::uint32_t source_zone_id = 0;
};

bool ContainsNet(const std::vector<std::uint32_t>& nets, std::uint32_t net_id)
{
    for (const std::uint32_t candidate : nets) {
        if (candidate == net_id) {
            return true;
        }
    }
    return false;
}

// Failure diagnostics: per-neighbor publish/cursor state for one net.
void DescribeNetSources(Zone& zone,
                        ZoneManager& zones,
                        std::uint32_t net_id,
                        std::uint32_t ghost_source,
                        std::ostringstream& out)
{
    out << " | ghost_source=" << ghost_source;
    for (const auto& cursor : zone.GhostMaintenance().neighbors) {
        if (cursor.zone_index >= zones.ZoneCount()) {
            continue;
        }
        Zone& neighbor = zones.GetZone(cursor.zone_index);
        std::lock_guard lock(neighbor.PublishMutex());
        const auto it = neighbor.PublishIndex().find(net_id);
        out << " | n" << neighbor.Id() << " gen=" << neighbor.PublishGeneration()
            << " cursor=" << cursor.publish_generation
            << " delta_valid=" << (neighbor.PublishDeltaValid() ? 1 : 0)
            << " in_buf=" << (it != neighbor.PublishIndex().end() ? 1 : 0)
            << " in_delta=" << (ContainsNet(neighbor.PublishDeltaNets(), net_id) ? 1 : 0)
            << " in_rem=" << (ContainsNet(neighbor.PublishDeltaRemoved(), net_id) ? 1 : 0);
    }
}

bool BuildExpectedPublishBuffer(Zone& zone, std::vector<BorderEntitySnapshot>& out)
{
    out.clear();
    for (const auto& [net_id, binding] : zone.Players()) {
        const auto entity = zone.FindEntity(net_id);
        if (!entity.is_valid()) {
            continue;
        }
        const auto position = entity.get<Position>();
        if (IsInBorderBand(zone.Bounds(), position, kAoiRadiusMeters)) {
            out.push_back(BuildPlayerSnapshot(zone, entity));
        }
    }
    zone.World().query<const MobTag>().each([&](flecs::entity entity, const MobTag&) {
        if (entity.has<GhostTag>()) {
            return;
        }
        const auto position = entity.get<Position>();
        if (IsInBorderBand(zone.Bounds(), position, kAoiRadiusMeters)) {
            out.push_back(BuildMobSnapshot(entity));
        }
    });
    return true;
}

// Exact expected ghost universe from the CURRENT neighbor publish buffers:
// the union of every neighbor's border set (a net transiently published by
// two neighbors during a migration is ONE ghost, and either source is a valid
// attribution), local residents excluded, ghosts never ghosted.
void BuildExpectedGhosts(Zone& zone,
                         ZoneManager& zones,
                         std::unordered_map<std::uint32_t, std::vector<ExpectedGhost>>& out)
{
    out.clear();
    const auto& neighbors = zones.NeighborsOf(zones.FindIndexById(zone.Id()));
    for (const std::size_t neighbor_index : neighbors) {
        if (neighbor_index >= zones.ZoneCount()) {
            continue;
        }
        Zone& neighbor = zones.GetZone(neighbor_index);
        const ZoneId neighbor_id = neighbor.Id();
        std::vector<BorderEntitySnapshot> snapshots;
        {
            std::lock_guard lock(neighbor.PublishMutex());
            snapshots = neighbor.PublishBuffer();
        }
        for (const auto& snapshot : snapshots) {
            if (snapshot.net_id == 0 || zone.IsResident(snapshot.net_id)) {
                continue;
            }
            auto& candidates = out[snapshot.net_id];
            bool duplicate = false;
            for (const auto& candidate : candidates) {
                if (candidate.source_zone_id == neighbor_id) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                candidates.push_back(ExpectedGhost{snapshot, neighbor_id});
            }
        }
    }
}

} // namespace

bool ValidateGhostPublisherFidelity(Zone& zone, std::string& out_error, bool* out_publish_pending)
{
    if (out_publish_pending != nullptr) {
        *out_publish_pending = false;
    }
    // The resident set can change between the last publish and this audit
    // (spawn/despawn/transfer): the next tick's publish performs a full
    // refill. The buffer is legitimately up to one tick behind in that window.
    if (zone.EntitySetGeneration() != zone.PublishedEntitySetGeneration()) {
        if (out_publish_pending != nullptr) {
            *out_publish_pending = true;
        }
        out_error.clear();
        return true;
    }

    std::vector<BorderEntitySnapshot> expected;
    BuildExpectedPublishBuffer(zone, expected);

    std::vector<BorderEntitySnapshot> actual;
    {
        std::lock_guard lock(zone.PublishMutex());
        actual = zone.PublishBuffer();
    }

    if (expected.size() != actual.size()) {
        std::ostringstream message;
        message << "zone " << zone.Id() << ": publish buffer holds " << actual.size()
                << " snapshots, expected " << expected.size();
        out_error = message.str();
        return false;
    }
    std::unordered_map<std::uint32_t, std::size_t> actual_index;
    actual_index.reserve(actual.size());
    for (std::size_t i = 0; i < actual.size(); ++i) {
        if (!actual_index.emplace(actual[i].net_id, i).second) {
            std::ostringstream message;
            message << "zone " << zone.Id() << ": duplicate net " << actual[i].net_id
                    << " in publish buffer";
            out_error = message.str();
            return false;
        }
    }
    for (const auto& snapshot : expected) {
        const auto it = actual_index.find(snapshot.net_id);
        if (it == actual_index.end()) {
            std::ostringstream message;
            message << "zone " << zone.Id() << ": publish buffer missing net " << snapshot.net_id;
            out_error = message.str();
            return false;
        }
        if (!SameBorderSnapshot(snapshot, actual[it->second])) {
            std::ostringstream message;
            message << "zone " << zone.Id() << ": publish buffer stale for net "
                    << snapshot.net_id << " (expected (" << snapshot.position.x << ", "
                    << snapshot.position.y << ") hp " << snapshot.hp_current << ", got ("
                    << actual[it->second].position.x << ", " << actual[it->second].position.y
                    << ") hp " << actual[it->second].hp_current << ")";
            out_error = message.str();
            return false;
        }
    }
    // The derived publish index must mirror the buffer exactly.
    {
        std::lock_guard lock(zone.PublishMutex());
        if (zone.PublishIndex().size() != zone.PublishBuffer().size()) {
            std::ostringstream message;
            message << "zone " << zone.Id() << ": publish index size mismatch";
            out_error = message.str();
            return false;
        }
        for (std::size_t i = 0; i < zone.PublishBuffer().size(); ++i) {
            const auto it = zone.PublishIndex().find(zone.PublishBuffer()[i].net_id);
            if (it == zone.PublishIndex().end() || it->second != i) {
                std::ostringstream message;
                message << "zone " << zone.Id() << ": publish index wrong for net "
                        << zone.PublishBuffer()[i].net_id;
                out_error = message.str();
                return false;
            }
        }
    }
    out_error.clear();
    return true;
}

bool ValidateGhostEquivalence(Zone& zone,
                              ZoneManager& zones,
                              std::string& out_error,
                              bool* out_topology_stale)
{
    if (out_topology_stale != nullptr) {
        *out_topology_stale = false;
    }
    // Ghosts exist for VISIBILITY: only zones with at least one viewer
    // maintain them (a playerless zone clears its ghost set by design). The
    // equivalence contract therefore applies to player-bearing zones only.
    if (zone.Players().empty()) {
        if (!zone.Ghosts().empty()) {
            std::ostringstream message;
            message << "zone " << zone.Id() << ": playerless zone holds "
                    << zone.Ghosts().size() << " ghosts";
            out_error = message.str();
            return false;
        }
        out_error.clear();
        return true;
    }

    // A topology change (split/merge/retire) invalidates the neighbor cursor
    // set; the consumer rebuilds it (force-full reconcile) on its next tick.
    // Until then its ghost set is legitimately stale for at most one tick, so
    // exact equivalence cannot be asserted yet. The caller counts these zones
    // separately: a persistent mismatch would show up as every poll skipping.
    const auto& cursors = zone.GhostMaintenance().neighbors;
    const auto& graph_neighbors = zones.NeighborsOf(zones.FindIndexById(zone.Id()));
    bool topology_caught_up = graph_neighbors.size() == cursors.size();
    if (topology_caught_up) {
        for (std::size_t i = 0; i < graph_neighbors.size(); ++i) {
            if (graph_neighbors[i] != cursors[i].zone_index) {
                topology_caught_up = false;
                break;
            }
        }
    }
    if (!topology_caught_up) {
        if (out_topology_stale != nullptr) {
            *out_topology_stale = true;
        }
        out_error.clear();
        return true;
    }

    // The reconcile contract is "at most ~1 tick stale": a neighbor that
    // published after this zone's last reconcile is legitimately ahead by up
    // to a bounded number of publish generations. Exact membership/field
    // comparison applies to caught-up sources; an unbounded lag is a failure
    // (a permanently stale ghost would otherwise never be detected).
    constexpr std::uint64_t kMaxReconcileLagGenerations = 2;
    std::unordered_map<std::uint32_t, bool> source_caught_up;
    bool all_caught_up = true;
    for (const auto& cursor : cursors) {
        if (cursor.zone_index >= zones.ZoneCount()) {
            continue;
        }
        Zone& neighbor = zones.GetZone(cursor.zone_index);
        const std::uint64_t current = neighbor.PublishGeneration();
        const bool caught_up = current == cursor.publish_generation;
        source_caught_up[neighbor.Id()] = caught_up;
        if (!caught_up) {
            all_caught_up = false;
            if (current > cursor.publish_generation + kMaxReconcileLagGenerations) {
                std::ostringstream message;
                message << "zone " << zone.Id() << ": ghost source zone " << neighbor.Id()
                        << " publish lag " << (current - cursor.publish_generation)
                        << " generations (reconcile is not keeping up)";
                out_error = message.str();
                return false;
            }
        }
    }

    std::unordered_map<std::uint32_t, std::vector<ExpectedGhost>> expected;
    BuildExpectedGhosts(zone, zones, expected);

    const auto& ghosts = zone.Ghosts();
    if (zone.GhostIndex().size() != ghosts.size()) {
        std::ostringstream message;
        message << "zone " << zone.Id() << ": ghost index size " << zone.GhostIndex().size()
                << " != ghost count " << ghosts.size();
        out_error = message.str();
        return false;
    }
    if (all_caught_up && ghosts.size() != expected.size()) {
        std::ostringstream message;
        message << "zone " << zone.Id() << ": ghost count " << ghosts.size() << ", expected "
                << expected.size();
        out_error = message.str();
        return false;
    }
    std::unordered_set<std::uint32_t> seen;
    seen.reserve(ghosts.size());
    for (const auto& ghost : ghosts) {
        const std::uint32_t net_id = ghost.snapshot.net_id;
        if (!seen.insert(net_id).second) {
            std::ostringstream message;
            message << "zone " << zone.Id() << ": duplicate ghost net " << net_id;
            out_error = message.str();
            return false;
        }
        if (zone.IsResident(net_id)) {
            std::ostringstream message;
            message << "zone " << zone.Id() << ": ghost net " << net_id
                    << " is a local resident (authoritative ghost)";
            out_error = message.str();
            return false;
        }
        if (!ghost.entity.is_valid() || !ghost.entity.has<GhostTag>()) {
            std::ostringstream message;
            message << "zone " << zone.Id() << ": ghost net " << net_id
                    << " missing entity/GhostTag";
            out_error = message.str();
            return false;
        }
        if (!zone.Grid().Contains(net_id, ghost.snapshot.position)) {
            std::ostringstream message;
            message << "zone " << zone.Id() << ": ghost net " << net_id
                    << " missing/wrong cell in spatial index";
            out_error = message.str();
            return false;
        }
        if (zone.FindGhost(net_id) != &ghost) {
            std::ostringstream message;
            message << "zone " << zone.Id() << ": ghost index does not resolve net " << net_id;
            out_error = message.str();
            return false;
        }
        // A ghost is comparable when its source is caught up; a ghost from a
        // lagging source may also be verifiable through a caught-up duplicate
        // publisher (migration transient), which is checked below.
        const auto caught = source_caught_up.find(ghost.source_zone_id);
        if (caught != source_caught_up.end() && !caught->second) {
            continue; // bounded reconcile lag: membership/fields not comparable yet
        }
        const auto it = expected.find(net_id);
        if (it == expected.end()) {
            std::ostringstream message;
            message << "zone " << zone.Id() << ": extra ghost net " << net_id
                    << " (not published by any neighbor)";
            DescribeNetSources(zone, zones, net_id, ghost.source_zone_id, message);
            out_error = message.str();
            return false;
        }
        // Union semantics: the ghost must be attributable to a neighbor that
        // actually publishes it, with a snapshot matching one of them.
        bool source_valid = false;
        bool snapshot_valid = false;
        for (const auto& candidate : it->second) {
            if (candidate.source_zone_id == ghost.source_zone_id) {
                source_valid = true;
            }
            if (SameBorderSnapshot(ghost.snapshot, candidate.snapshot)) {
                snapshot_valid = true;
            }
        }
        if (!source_valid) {
            std::ostringstream message;
            message << "zone " << zone.Id() << ": ghost net " << net_id << " source zone "
                    << ghost.source_zone_id << " does not publish it";
            out_error = message.str();
            return false;
        }
        if (!snapshot_valid) {
            const auto& reference = it->second.front().snapshot;
            std::ostringstream message;
            message << "zone " << zone.Id() << ": ghost net " << net_id
                    << " snapshot stale (ghost (" << ghost.snapshot.position.x << ", "
                    << ghost.snapshot.position.y << ") hp " << ghost.snapshot.hp_current
                    << ", expected (" << reference.position.x << ", " << reference.position.y
                    << ") hp " << reference.hp_current << ")";
            DescribeNetSources(zone, zones, net_id, ghost.source_zone_id, message);
            out_error = message.str();
            return false;
        }
    }
    // Expected-but-missing can only be asserted for caught-up sources.
    if (!all_caught_up) {
        std::unordered_map<std::uint32_t, bool> present;
        present.reserve(ghosts.size());
        for (const auto& ghost : ghosts) {
            present[ghost.snapshot.net_id] = true;
        }
        for (const auto& [net_id, candidates] : expected) {
            bool any_caught_up = false;
            for (const auto& candidate : candidates) {
                const auto caught = source_caught_up.find(candidate.source_zone_id);
                if (caught == source_caught_up.end() || caught->second) {
                    any_caught_up = true;
                    break;
                }
            }
            if (!any_caught_up) {
                continue;
            }
            if (!present.contains(net_id)) {
                std::ostringstream message;
                message << "zone " << zone.Id() << ": missing ghost net " << net_id
                        << " from zone " << candidates.front().source_zone_id;
                DescribeNetSources(zone, zones, net_id, 0, message);
                out_error = message.str();
                return false;
            }
        }
    }
    out_error.clear();
    return true;
}

bool ValidateAllGhostEquivalence(ZoneManager& zones,
                                 std::string& out_error,
                                 std::size_t* out_zones_checked,
                                 std::size_t* out_ghosts_checked,
                                 std::size_t* out_zones_skipped)
{
    std::size_t zones_checked = 0;
    std::size_t ghosts_checked = 0;
    std::size_t zones_skipped = 0;
    for (std::size_t i = 0; i < zones.ZoneCount(); ++i) {
        Zone& zone = zones.GetZone(i);
        if (!zone.SimulationEnabled() || zone.Partition() != PartitionState::Leaf) {
            // Retired/staged zones are drained and hold no ghosts.
            if (!zone.Ghosts().empty()) {
                std::ostringstream message;
                message << "zone " << zone.Id() << ": non-simulating zone holds "
                        << zone.Ghosts().size() << " ghosts";
                out_error = message.str();
                return false;
            }
            continue;
        }
        bool publish_pending = false;
        if (!ValidateGhostPublisherFidelity(zone, out_error, &publish_pending)) {
            return false;
        }
        if (publish_pending) {
            ++zones_skipped;
            continue;
        }
        bool topology_stale = false;
        if (!ValidateGhostEquivalence(zone, zones, out_error, &topology_stale)) {
            return false;
        }
        if (topology_stale) {
            ++zones_skipped;
            continue;
        }
        ++zones_checked;
        ghosts_checked += zone.Ghosts().size();
    }
    if (out_zones_checked != nullptr) {
        *out_zones_checked = zones_checked;
    }
    if (out_ghosts_checked != nullptr) {
        *out_ghosts_checked = ghosts_checked;
    }
    if (out_zones_skipped != nullptr) {
        *out_zones_skipped = zones_skipped;
    }
    out_error.clear();
    return true;
}

} // namespace gs::game
