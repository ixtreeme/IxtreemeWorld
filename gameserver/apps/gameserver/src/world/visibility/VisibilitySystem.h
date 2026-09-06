#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "network/Session.h"

#include "../visibility/BorderSnapshot.h"

// Visibility answers ONLY: "which of the AOI candidates does this viewer
// actually see right now?" It diffs the candidate set against the viewer's
// known set, emits spawn/despawn events for the delta, and returns the full
// visible snapshot list for transform replication.
namespace gs::game {

class Zone;

class VisibilitySystem {
public:
    using SendFn = std::function<void(std::shared_ptr<gs::network::Session>, std::vector<std::uint8_t>)>;

    static std::vector<BorderEntitySnapshot> ReconcileViewer(Zone& zone,
                                                             std::uint32_t viewer_net_id,
                                                             const std::vector<std::uint32_t>& candidates,
                                                             const SendFn& send);
};

} // namespace gs::game
