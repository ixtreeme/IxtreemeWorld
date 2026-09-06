#include "WorldMessageRouter.h"

#include "../zone/ZoneManager.h"

namespace gs::game {

WorldMessageRouter::WorldMessageRouter(ZoneManager& zones, WorldDirectory& directory)
    : zones_(zones)
    , directory_(directory)
{
}

DeliveryResult WorldMessageRouter::RouteZoneCommand(std::size_t zone_index,
                                                    ZoneCommandQueue::Command command,
                                                    MessagePriority priority)
{
    (void)priority; // Local FIFO preserves order today; priority takes
                    // effect at the future remote transport (§39).
    if (zone_index >= zones_.ZoneCount()) {
        metrics_.unavailable.fetch_add(1, std::memory_order_relaxed);
        return DeliveryResult::DestinationUnavailable;
    }
    const auto location = directory_.ResolveZone(zones_.GetZone(zone_index).Id());
    if (!location) {
        metrics_.directory_miss.fetch_add(1, std::memory_order_relaxed);
        return DeliveryResult::DestinationUnavailable;
    }
    if (!directory_.IsLocal(*location)) {
        if (directory_.IsDraining(*location)) {
            metrics_.draining.fetch_add(1, std::memory_order_relaxed);
            return DeliveryResult::DestinationDraining;
        }
        if (mode_.load(std::memory_order_relaxed) == RemoteMode::EmulatedLoopback) {
            zones_.PostCommand(zone_index, std::move(command));
            metrics_.remote_emulated.fetch_add(1, std::memory_order_relaxed);
            return DeliveryResult::DeliveredRemoteEmulated;
        }
        metrics_.remote_unsupported.fetch_add(1, std::memory_order_relaxed);
        return DeliveryResult::RemoteNotSupported;
    }
    zones_.PostCommand(zone_index, std::move(command));
    metrics_.local_delivered.fetch_add(1, std::memory_order_relaxed);
    return DeliveryResult::DeliveredLocal;
}

RoutingMetrics::Snapshot WorldMessageRouter::MetricsSnapshot() const
{
    return metrics_.TakeSnapshot();
}

} // namespace gs::game
