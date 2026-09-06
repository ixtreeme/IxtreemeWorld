#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

#include "network/Session.h"

// Replication answers ONLY: "what bytes does each client need?"
// Pipeline per zone tick: SpatialGrid -> AoiSystem -> VisibilitySystem ->
// encode (ProtocolEncoder) -> send. This module is the orchestrator; it holds
// no visibility state and no wire-format code itself.
namespace gs::game {

class Zone;

class ReplicationSystem {
public:
    using SendFn =
        std::function<void(std::shared_ptr<gs::network::Session>, std::vector<std::uint8_t>)>;

    // Returns the number of transform records emitted (diagnostics).
    static std::size_t BroadcastTransforms(Zone& zone, const SendFn& send);
};

} // namespace gs::game
