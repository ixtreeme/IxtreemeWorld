#include "MigrationSystem.h"

#include <algorithm>

#include "../WorldConstants.h"
#include "../components/MigrationComponents.h"
#include "../spatial/SpatialTypes.h"
#include "../zone/Zone.h"
#include "../zone/ZoneManager.h"
#include "../zone/ZoneOwnership.h"
#include "MigrationQueue.h"

namespace gs::game {

void MigrationSystem::UpdateMarker(Zone& zone,
                                   ZoneManager& zones,
                                   MigrationQueue* queue,
                                   std::uint32_t net_id,
                                   flecs::entity entity,
                                   const Position& position)
{
    AssertZoneOwner(zone, "zone migration marker update");

    const std::size_t current_zone_index = zones.FindIndexById(zone.Id());
    const std::size_t target_zone_index = zones.FindIndexForPosition(position.x, position.y);
    std::uint32_t target_zone_id = 0;

    if (target_zone_index < zones.ZoneCount() && target_zone_index != current_zone_index &&
        DistanceOutsideRect(zone.Bounds(), position) > kMigrationHysteresisMeters) {
        const auto& neighbors = zones.NeighborsOf(current_zone_index);
        if (std::find(neighbors.begin(), neighbors.end(), target_zone_index) != neighbors.end()) {
            target_zone_id = zones.GetZone(target_zone_index).Id();
        }
    }

    entity.set<MigrateTo>({target_zone_id});

    // Event-driven migration: only border crossings touch the queue, and the
    // dedup set absorbs repeats while a request is pending. The common case
    // (inside bounds, target 0) never locks.
    if (target_zone_id != 0 && queue != nullptr && net_id != 0) {
        queue->TryEnqueue(MigrationRequest{net_id, zone.Id(), target_zone_id});
    }
}

} // namespace gs::game
