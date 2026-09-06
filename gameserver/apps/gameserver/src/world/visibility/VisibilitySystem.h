#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include "network/Session.h"

#include "../visibility/BorderSnapshot.h"

// Visibility answers ONLY: "which of the AOI candidates does this viewer
// actually see right now?" It diffs the candidate set against the viewer's
// known set, emits spawn/despawn events for the delta only (entered -> spawn,
// left -> despawn, still visible -> transform updates via replication), and
// returns the full visible snapshot list for transform replication.
//
// Spawn encoding is shared through the caller-provided per-tick cache so one
// Cap'n Proto build serves every viewer discovering the same net that tick;
// bytes on the wire are unchanged.
namespace gs::game {

class Zone;

class VisibilitySystem {
public:
    using SendFn = std::function<void(std::shared_ptr<gs::network::Session>, std::vector<std::uint8_t>)>;
    using SpawnCache = std::unordered_map<std::uint32_t, std::vector<std::uint8_t>>;
    // Per-tick snapshot cache shared across viewers: the same entity is
    // visible to many viewers, but its snapshot is built once per tick.
    using SnapshotCache = std::unordered_map<std::uint32_t, BorderEntitySnapshot>;

    static std::vector<BorderEntitySnapshot> ReconcileViewer(Zone& zone,
                                                             std::uint32_t viewer_net_id,
                                                             const std::vector<std::uint32_t>& candidates,
                                                             const SendFn& send,
                                                             SpawnCache& spawn_cache,
                                                             SnapshotCache& snapshot_cache);
};

} // namespace gs::game
