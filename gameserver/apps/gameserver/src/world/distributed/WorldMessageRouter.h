#pragma once

#include <atomic>
#include <cstddef>
#include <functional>

#include "../zone/ZoneCommandQueue.h"
#include "Routing.h"
#include "WorldDirectory.h"

// Routes zone-bound commands to the owning location. Gameplay code posts
// through here instead of touching ZoneManager indices directly, so the
// SAME call sites work whether the destination is local, emulated-remote,
// or (in the future) truly remote:
//
//   local               -> ZoneManager::PostCommand (today's path, unchanged)
//   emulated-remote     -> local delivery + remote_emulated counter (the
//                          benchmark's logical-distribution proof)
//   unknown/unavailable -> DestinationUnavailable, command dropped, counted
//                          (safe for latest-wins inputs; attacks to unknown
//                          zones were already no-ops via the OwnerMap miss)
//   draining (remote)   -> DestinationDraining, counted, not delivered
//
// Local delivery is lossless FIFO (parity). The caller still wakes the
// scheduler after routing; the router never blocks.
namespace gs::game {

class ZoneManager;

class WorldMessageRouter {
public:
    enum class RemoteMode : std::uint8_t {
        Disabled = 0,      // Non-local destinations are unavailable (default).
        EmulatedLoopback,  // Non-local destinations deliver locally but
                           // count as remote (benchmark emulation only).
    };

    WorldMessageRouter(ZoneManager& zones, WorldDirectory& directory);

    // May be called from another thread (benchmark emulation setup);
    // routing reads it atomically.
    void SetRemoteMode(RemoteMode mode)
    {
        mode_.store(mode, std::memory_order_relaxed);
    }

    DeliveryResult RouteZoneCommand(std::size_t zone_index,
                                    ZoneCommandQueue::Command command,
                                    MessagePriority priority = MessagePriority::Gameplay);

    RoutingMetrics::Snapshot MetricsSnapshot() const;

private:
    ZoneManager& zones_;
    WorldDirectory& directory_;
    std::atomic<RemoteMode> mode_{RemoteMode::Disabled};
    RoutingMetrics metrics_;
};

} // namespace gs::game
