#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

#include "network/Session.h"

#include "ReplicationConfig.h"

// Outbound replication pipeline per zone tick:
//   dirty/interest state -> AOI candidates -> visibility reconcile ->
//   frame encode -> recipient fanout -> send()
//
// Phase 5B: transform records are change-driven (the recipient's interest
// set stores the last transform version it was sent), spawns carry the full
// initial state, and a staggered periodic refresh bounds any missed update.
namespace gs::game {

class Zone;

class ReplicationSystem {
public:
    using SendFn =
        std::function<void(std::shared_ptr<gs::network::Session>, std::vector<std::uint8_t>)>;

    // `config` may be null (optimized defaults). Returns the number of
    // transform records handed to frames (viewer records included).
    static std::size_t BroadcastTransforms(Zone& zone,
                                           const SendFn& send,
                                           const ReplicationConfig* config = nullptr);
};

} // namespace gs::game
