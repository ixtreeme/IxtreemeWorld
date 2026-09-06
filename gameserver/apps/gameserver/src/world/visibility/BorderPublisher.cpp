#include "BorderPublisher.h"

#include "../WorldConstants.h"
#include "../components/MobComponents.h"
#include "../components/NetworkComponents.h"
#include "../components/Tags.h"
#include "../replication/SnapshotBuilder.h"
#include "../spatial/SpatialTypes.h"
#include "../zone/Zone.h"
#include "../zone/ZoneOwnership.h"

namespace gs::game {

void BorderPublisher::Publish(Zone& zone)
{
    AssertZoneOwner(zone, "zone border publish");

    // The buffer is read lock-free by neighbor zones rebuilding ghosts, so the
    // fill is guarded by the zone's publish mutex (short critical section).
    std::lock_guard publish_lock(zone.PublishMutex());
    auto& out = zone.PublishBuffers()[zone.TickIndex() % zone.PublishBuffers().size()];
    out.clear();
    out.reserve(zone.Players().size() +
                static_cast<std::size_t>(zone.Diagnostics().mob_count.load(std::memory_order_relaxed)));

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
    // Ghosts carry MobTag for AOI matching but hold no gameplay components;
    // publishing them would read components they don't have, so only
    // authoritative residents are published (as before).
    zone.World().query<const MobTag>().each([&](flecs::entity entity, const MobTag&) {
        if (entity.has<GhostTag>()) {
            return;
        }
        const auto position = entity.get<Position>();
        if (IsInBorderBand(zone.Bounds(), position, kAoiRadiusMeters)) {
            out.push_back(BuildMobSnapshot(entity));
        }
    });
}

} // namespace gs::game
