#include "GhostSystem.h"

#include <unordered_set>

#include "../components/MobComponents.h"
#include "../components/NetworkComponents.h"
#include "../components/Tags.h"
#include "../components/TransformComponents.h"
#include "../zone/Zone.h"
#include "../zone/ZoneManager.h"
#include "../zone/ZoneOwnership.h"

namespace gs::game {

void GhostSystem::Rebuild(Zone& zone, ZoneManager& zones)
{
    AssertZoneOwner(zone, "zone ghost rebuild");

    Clear(zone);
    const auto& neighbors = zones.NeighborsOf(zones.FindIndexById(zone.Id()));
    if (neighbors.empty()) {
        return;
    }

    std::unordered_set<std::uint32_t> added_net_ids;
    for (const std::size_t neighbor_index : neighbors) {
        if (neighbor_index >= zones.ZoneCount()) {
            continue;
        }

        auto& neighbor = zones.GetZone(neighbor_index);
        // Copy under the neighbor's publish mutex: the neighbor may be
        // filling its current buffer on another worker at this moment.
        // Only plain snapshot data crosses the zone boundary, never handles.
        std::vector<BorderEntitySnapshot> snapshots;
        {
            std::lock_guard publish_lock(neighbor.PublishMutex());
            snapshots =
                neighbor.PublishBuffers()[(zone.TickIndex() + 1) % neighbor.PublishBuffers().size()];
        }
        for (const auto& snapshot : snapshots) {
            if (snapshot.net_id == 0 || zone.IsResident(snapshot.net_id) ||
                !added_net_ids.insert(snapshot.net_id).second) {
                continue;
            }

            auto entity = zone.World()
                              .entity()
                              .set<Position>(snapshot.position)
                              .set<Heading>(snapshot.heading)
                              .set<NetId>({snapshot.net_id})
                              .add<GhostTag>();
            if (snapshot.mob_type_id != 0) {
                entity.set<MobTypeRef>({snapshot.mob_type_id}).add<MobTag>();
            }
            zone.Ghosts().push_back(GhostRecord{entity, snapshot});
        }
    }
    zone.Diagnostics().ghost_count.store(static_cast<std::uint32_t>(zone.Ghosts().size()),
                                         std::memory_order_relaxed);
}

void GhostSystem::Clear(Zone& zone)
{
    AssertZoneOwner(zone, "zone ghost clear");

    for (auto& ghost : zone.Ghosts()) {
        if (ghost.entity.is_valid()) {
            ghost.entity.destruct();
        }
    }
    zone.Ghosts().clear();
    zone.Diagnostics().ghost_count.store(0, std::memory_order_relaxed);
}

void GhostSystem::RemoveByNetId(Zone& zone, std::uint32_t net_id)
{
    AssertZoneOwner(zone, "zone ghost dedup removal");

    auto& ghosts = zone.Ghosts();
    const auto it = std::find_if(ghosts.begin(), ghosts.end(), [net_id](const GhostRecord& ghost) {
        return ghost.snapshot.net_id == net_id;
    });
    if (it == ghosts.end()) {
        return;
    }

    if (it->entity.is_valid()) {
        it->entity.destruct();
    }
    ghosts.erase(it);
    zone.Diagnostics().ghost_count.store(static_cast<std::uint32_t>(ghosts.size()),
                                         std::memory_order_relaxed);
}

} // namespace gs::game
