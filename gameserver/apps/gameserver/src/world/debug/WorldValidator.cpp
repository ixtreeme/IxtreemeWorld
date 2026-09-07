#include "WorldValidator.h"

#include <cmath>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../components/Tags.h"
#include "../distributed/WorldDirectory.h"
#include "../migration/MigrationQueue.h"
#include "../partition/ZonePartition.h"
#include "../spatial/SpatialValidator.h"
#include "../zone/Zone.h"
#include "../zone/ZoneManager.h"

namespace gs::game {
namespace {

bool Fail(std::string& out_error, const std::string& message)
{
    out_error = message;
    return false;
}

} // namespace

namespace {

// Partition topology audit (§47-48): active leaves tile their region roots
// exactly (no gap, no overlap), only leaves are authoritative, retired
// zones are empty.
bool ValidatePartitionTopology(const ZoneManager& zones, std::string& out_error)
{
    constexpr float kEps = 0.01f;
    std::vector<ZonePartition*> leaves;
    for (const auto& root : zones.PartitionRoots()) {
        CollectActiveLeaves(root.get(), leaves);
        // Every active leaf must sit inside its region root (map coverage is
        // whatever the map defines; split subtrees additionally tile).
        for (ZonePartition* leaf : leaves) {
            if (leaf->zone_id != 0 && leaf->region_id == root->region_id) {
                if (leaf->bounds.min_x < root->bounds.min_x - kEps ||
                    leaf->bounds.min_y < root->bounds.min_y - kEps ||
                    leaf->bounds.max_x > root->bounds.max_x + kEps ||
                    leaf->bounds.max_y > root->bounds.max_y + kEps) {
                    std::ostringstream message;
                    message << "partition: leaf zone " << leaf->zone_id
                            << " escapes its region root";
                    return Fail(out_error, message.str());
                }
            }
        }
        // Sibling tiling: enforced for split-created internal nodes only.
        // Region roots are exempt -- map zones never tile the full quadrant,
        // and promising otherwise would change spawn/lookup behavior.
        std::vector<const ZonePartition*> stack{root.get()};
        while (!stack.empty()) {
            const ZonePartition* node = stack.back();
            stack.pop_back();
            if (node->IsLeaf() || node->parent == nullptr) {
                for (const auto& child : node->children) {
                    stack.push_back(child.get());
                }
                continue;
            }
            float min_x = node->bounds.max_x;
            float min_y = node->bounds.max_y;
            float max_x = node->bounds.min_x;
            float max_y = node->bounds.min_y;
            for (const auto& child : node->children) {
                min_x = std::min(min_x, child->bounds.min_x);
                min_y = std::min(min_y, child->bounds.min_y);
                max_x = std::max(max_x, child->bounds.max_x);
                max_y = std::max(max_y, child->bounds.max_y);
                stack.push_back(child.get());
            }
            if (std::abs(min_x - node->bounds.min_x) > kEps ||
                std::abs(min_y - node->bounds.min_y) > kEps ||
                std::abs(max_x - node->bounds.max_x) > kEps ||
                std::abs(max_y - node->bounds.max_y) > kEps) {
                std::ostringstream message;
                message << "partition: children of zone " << node->zone_id
                        << " do not tile the parent bounds";
                return Fail(out_error, message.str());
            }
        }
    }
    // Pairwise interior-disjoint active leaves.
    for (std::size_t i = 0; i < leaves.size(); ++i) {
        for (std::size_t j = i + 1; j < leaves.size(); ++j) {
            const auto& a = leaves[i]->bounds;
            const auto& b = leaves[j]->bounds;
            const bool overlap = a.min_x < b.max_x - kEps && a.max_x > b.min_x + kEps &&
                                 a.min_y < b.max_y - kEps && a.max_y > b.min_y + kEps;
            if (overlap) {
                std::ostringstream message;
                message << "partition: active leaves " << leaves[i]->zone_id << " and "
                        << leaves[j]->zone_id << " overlap";
                return Fail(out_error, message.str());
            }
        }
    }
    return true;
}

} // namespace

bool ValidateWorldConsistency(ZoneManager& zones,
                              const OwnerMap& owners,
                              const MigrationQueue& migrations,
                              const WorldDirectory& directory,
                              std::string& out_error)
{
    if (!ValidatePartitionTopology(zones, out_error)) {
        return false;
    }

    // Directory <-> tree <-> runtime cross-check (§21): every active leaf
    // has exactly one assignment pointing at itself; every assignment
    // resolves to a known zone; staged (uncommitted) zones are never routed
    // (assignments publish only at commit); retired assignments are tolerated
    // as in-flight retention.
    {
        const auto assignments = directory.AssignmentSnapshot();
        std::unordered_map<ZoneId, ZoneLocation> by_zone;
        for (const auto& [id, loc] : assignments) {
            if (loc.zone != id) {
                std::ostringstream message;
                message << "directory: assignment " << id << " points at zone " << loc.zone;
                return Fail(out_error, message.str());
            }
            if (zones.FindIndexById(id) >= zones.ZoneCount()) {
                std::ostringstream message;
                message << "directory: assignment points at unknown zone " << id;
                return Fail(out_error, message.str());
            }
            by_zone.emplace(id, loc);
        }
        for (ZonePartition* leaf : zones.GetActiveLeaves()) {
            const auto it = by_zone.find(leaf->zone_id);
            if (it == by_zone.end()) {
                std::ostringstream message;
                message << "directory: active leaf zone " << leaf->zone_id << " has no assignment";
                return Fail(out_error, message.str());
            }
            const std::size_t index = zones.FindIndexById(leaf->zone_id);
            const auto& zone = zones.GetZone(index);
            if (!zone.SimulationEnabled() || zone.Partition() != PartitionState::Leaf) {
                std::ostringstream message;
                message << "topology: active leaf zone " << leaf->zone_id
                        << " is not a simulating leaf runtime";
                return Fail(out_error, message.str());
            }
        }
        for (std::size_t i = 0; i < zones.ZoneCount(); ++i) {
            const auto& zone = zones.GetZone(i);
            if (zone.Partition() == PartitionState::Staging &&
                by_zone.find(zone.Id()) != by_zone.end()) {
                std::ostringstream message;
                message << "directory: staged zone " << zone.Id() << " routed before commit";
                return Fail(out_error, message.str());
            }
        }
    }

    std::unordered_set<std::uint32_t> authoritative_nets;

    for (std::size_t i = 0; i < zones.ZoneCount(); ++i) {
        auto& zone = zones.GetZone(i);

        // Only simulating leaves hold authority. Anything else must be
        // empty (drained before retirement).
        if (!zone.SimulationEnabled() || zone.Partition() != PartitionState::Leaf) {
            if (zone.Partition() == PartitionState::Retired) {
                // Retired: zero authority state of every kind (§19). No
                // entities, no bindings, no session mappings, no queued
                // commands, no spatial entries, no ghosts, no simulation.
                if (!zone.Entities().empty()) {
                    std::ostringstream message;
                    message << "zone " << zone.Id() << ": retired zone holds indexed entities";
                    return Fail(out_error, message.str());
                }
                if (!zone.Players().empty() || !zone.NetBySession().empty()) {
                    std::ostringstream message;
                    message << "zone " << zone.Id() << ": retired zone holds player state";
                    return Fail(out_error, message.str());
                }
                if (!zone.Commands().Empty()) {
                    std::ostringstream message;
                    message << "zone " << zone.Id() << ": retired zone holds queued commands";
                    return Fail(out_error, message.str());
                }
                if (zone.Grid().Size() != 0) {
                    std::ostringstream message;
                    message << "zone " << zone.Id() << ": retired zone holds spatial entries";
                    return Fail(out_error, message.str());
                }
                if (!zone.Ghosts().empty()) {
                    std::ostringstream message;
                    message << "zone " << zone.Id() << ": retired zone holds ghosts";
                    return Fail(out_error, message.str());
                }
                if (zone.SimulationEnabled()) {
                    std::ostringstream message;
                    message << "zone " << zone.Id() << ": retired zone still simulating";
                    return Fail(out_error, message.str());
                }
            } else if (!zone.Entities().empty() || !zone.Players().empty()) {
                std::ostringstream message;
                message << "zone " << zone.Id() << ": non-authoritative zone still holds residents";
                return Fail(out_error, message.str());
            }
            continue;
        }

        // Every indexed NetId resolves to a live entity, exactly once world-wide.
        for (const auto& [net_id, entity] : zone.Entities()) {
            (void)entity;
            if (!zone.FindEntity(net_id).is_valid()) {
                std::ostringstream message;
                message << "zone " << zone.Id() << ": indexed net " << net_id << " is stale";
                return Fail(out_error, message.str());
            }
            if (!authoritative_nets.insert(net_id).second) {
                std::ostringstream message;
                message << "net " << net_id << " authoritative in two zones (dual authority!)";
                return Fail(out_error, message.str());
            }
        }

        // Session bindings point at live indexed entities, both maps agree.
        for (const auto& [net_id, binding] : zone.Players()) {
            if (!binding.session) {
                std::ostringstream message;
                message << "zone " << zone.Id() << ": player net " << net_id << " has null session";
                return Fail(out_error, message.str());
            }
            if (!zone.FindEntity(net_id).is_valid()) {
                std::ostringstream message;
                message << "zone " << zone.Id() << ": player net " << net_id << " has no entity";
                return Fail(out_error, message.str());
            }
            const auto session_net = zone.NetIdForSession(binding.session->Id());
            if (!session_net || *session_net != net_id) {
                std::ostringstream message;
                message << "zone " << zone.Id() << ": net_by_session disagrees for session "
                        << binding.session->Id();
                return Fail(out_error, message.str());
            }
            // Reverse direction (§22): every live binding has a matching
            // OwnerMap entry pointing back here.
            const auto owner_it = owners.find(binding.session->Id());
            if (owner_it == owners.end() || owner_it->second.net_id != net_id ||
                owner_it->second.zone_index != i || owner_it->second.location.zone != zone.Id()) {
                std::ostringstream message;
                message << "zone " << zone.Id() << ": player net " << net_id
                        << " has no matching OwnerMap entry";
                return Fail(out_error, message.str());
            }
        }
        for (const auto& [session_id, net_id] : zone.NetBySession()) {
            const auto* binding = zone.FindPlayer(net_id);
            if (binding == nullptr || !binding->session || binding->session->Id() != session_id) {
                std::ostringstream message;
                message << "zone " << zone.Id() << ": orphan net_by_session entry for session "
                        << session_id;
                return Fail(out_error, message.str());
            }
        }

        // RNG drivers must not outlive their mob (leak check).
        for (const auto net_id : zone.MobRngKeys()) {
            const auto entity = zone.FindEntity(net_id);
            if (!entity.is_valid() || !entity.has<MobTag>()) {
                std::ostringstream message;
                message << "zone " << zone.Id() << ": orphan mob RNG for net " << net_id;
                return Fail(out_error, message.str());
            }
        }

        // Ghosts are never residents and always tagged.
        for (const auto& ghost : zone.Ghosts()) {
            if (zone.Entities().find(ghost.snapshot.net_id) != zone.Entities().end()) {
                std::ostringstream message;
                message << "zone " << zone.Id() << ": ghost net " << ghost.snapshot.net_id
                        << " is also indexed as resident";
                return Fail(out_error, message.str());
            }
            if (!ghost.entity.is_valid() || !ghost.entity.has<GhostTag>()) {
                std::ostringstream message;
                message << "zone " << zone.Id() << ": ghost net " << ghost.snapshot.net_id
                        << " missing entity/GhostTag";
                return Fail(out_error, message.str());
            }
        }

        if (!ValidateSpatialIndex(zone, zone.Grid(), out_error)) {
            return false;
        }
    }

    // OwnerMap entries point at a live binding + entity in the recorded zone,
    // and the fast-path caches agree with the global identity + directory.
    for (const auto& [session_id, owner] : owners) {
        if (owner.zone_index >= zones.ZoneCount()) {
            std::ostringstream message;
            message << "owner map: session " << session_id << " points at missing zone index "
                    << owner.zone_index;
            return Fail(out_error, message.str());
        }
        auto& zone = zones.GetZone(owner.zone_index);
        if (owner.location.zone != zone.Id()) {
            std::ostringstream message;
            message << "owner map: session " << session_id << " location zone "
                    << owner.location.zone << " disagrees with indexed zone " << zone.Id();
            return Fail(out_error, message.str());
        }
        // Owners must point at live authority, never at retired/frozen zones.
        if (!zone.SimulationEnabled() || zone.Partition() != PartitionState::Leaf) {
            std::ostringstream message;
            message << "owner map: session " << session_id << " routed to non-authoritative zone "
                    << zone.Id();
            return Fail(out_error, message.str());
        }
        if (owner.entity != ToGlobalEntityId(owner.net_id, NamespaceOf(owner.entity))) {
            std::ostringstream message;
            message << "owner map: session " << session_id << " net/global id mismatch";
            return Fail(out_error, message.str());
        }
        const auto directory_location = directory.ResolveZone(zone.Id());
        if (!directory_location || !(*directory_location == owner.location)) {
            std::ostringstream message;
            message << "owner map: session " << session_id << " location disagrees with directory";
            return Fail(out_error, message.str());
        }
        const auto* binding = zone.FindPlayer(owner.net_id);
        if (binding == nullptr || !binding->session || binding->session->Id() != session_id) {
            std::ostringstream message;
            message << "owner map: session " << session_id << " has no live binding in zone "
                    << zone.Id();
            return Fail(out_error, message.str());
        }
        if (!zone.FindEntity(owner.net_id).is_valid()) {
            std::ostringstream message;
            message << "owner map: session " << session_id << " has no live entity in zone "
                    << zone.Id();
            return Fail(out_error, message.str());
        }
    }

    // Pending migration requests reference valid zones (liveness itself is
    // revalidated at execution; stale ones are dropped there).
    for (const auto& request : migrations.RequestSnapshot()) {
        if (zones.FindIndexById(request.source_zone_id) >= zones.ZoneCount() ||
            zones.FindIndexById(request.target_zone_id) >= zones.ZoneCount()) {
            std::ostringstream message;
            message << "migration queue: net " << request.net_id << " references invalid zone "
                    << request.source_zone_id << "->" << request.target_zone_id;
            return Fail(out_error, message.str());
        }
    }

    return true;
}

} // namespace gs::game
