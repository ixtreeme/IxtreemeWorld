#include "LoadFieldPublisher.h"

#include "../zone/Zone.h"
#include "../zone/ZoneOwnership.h"

namespace gs::game {

void LoadFieldPublisher::Publish(Zone& zone)
{
    AssertZoneOwner(zone, "zone load field publish");
    std::lock_guard lock(zone.ActivityMutex());
    zone.LoadBins().MovePendingTo(zone.PublishedLoadBins());
}

} // namespace gs::game
