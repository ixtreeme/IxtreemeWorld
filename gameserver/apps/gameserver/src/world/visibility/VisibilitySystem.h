#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include "network/Session.h"

#include "../spatial/AoiSystem.h"
#include "../visibility/BorderSnapshot.h"

// Visibility answers ONLY: "which of the AOI candidates does this viewer
// actually see right now?" It diffs the candidate set against the viewer's
// interest set and emits exactly the lifecycle events the delta requires:
//
//   ENTER -> spawn (full initial state; the transform is part of it, so no
//            separate transform record is generated for the same tick)
//   KEEP  -> transform record only when the entity's TransformVersion is
//            newer than the version this viewer was last sent (phase 5B
//            dirty replication), or on the staggered periodic refresh
//   LEAVE -> despawn
//
// The interest set (viewer->visible_net_versions) is the single source of
// truth for "what does this recipient already know"; it is updated only
// after the corresponding payload is handed to send().
namespace gs::game {

class Zone;

struct ReconcileStats {
    std::size_t spawns = 0;          // ENTER events (initial state sent)
    std::size_t despawns = 0;        // LEAVE events (removal sent)
    std::size_t updates = 0;         // transform records (dirty or refresh)
    std::size_t suppressed = 0;      // visible but already caught up
    std::size_t visible = 0;         // final visible set size
    std::uint64_t payload_bytes = 0; // spawn/despawn payload bytes handed to send
    std::uint64_t payload_copied_bytes = 0; // bytes copied into those payloads
    std::uint64_t despawn_cache_hits = 0;
    std::uint64_t despawn_cache_misses = 0;
};

// Per-zone-tick canonical transform record cache (phase 5C): each entity
// version is serialized once into a 19-byte record; every interested
// recipient references the slot. Reused across ticks (clear keeps capacity).
struct RecordCache {
    std::vector<TransformRecord> records;
    struct Entry {
        std::uint32_t version = 0;
        std::uint32_t slot = 0;
    };
    std::unordered_map<std::uint32_t, Entry> index;
    std::uint64_t requests = 0;
    std::uint64_t serializations = 0;

    void Clear() noexcept
    {
        records.clear();
        index.clear();
        requests = 0;
        serializations = 0;
    }
};

class VisibilitySystem {
public:
    using SendFn = std::function<void(std::shared_ptr<gs::network::Session>, std::vector<std::uint8_t>)>;
    using SpawnCache = std::unordered_map<std::uint32_t, std::vector<std::uint8_t>>;
    // Per-tick snapshot cache shared across viewers: the same entity is
    // visible to many viewers, but its spawn snapshot is built once per tick.
    using SnapshotCache = std::unordered_map<std::uint32_t, BorderEntitySnapshot>;
    // Phase 5C: recipient-independent despawn payloads, built once per net
    // per tick (the spawn cache's counterpart).
    using DespawnCache = std::unordered_map<std::uint32_t, std::vector<std::uint8_t>>;

    // `refresh_all` forces a full transform resend for every visible entity
    // (legacy behavior when dirty replication is off; staggered self-healing
    // otherwise). `out_record_slots` is caller-owned scratch filled with the
    // canonical record slots to replicate this tick (in candidate order); it
    // is cleared first. The record content lives in `record_cache`.
    static void ReconcileViewer(Zone& zone,
                                std::uint32_t viewer_net_id,
                                const std::vector<AoiCandidate>& candidates,
                                const SendFn& send,
                                SpawnCache& spawn_cache,
                                SnapshotCache& snapshot_cache,
                                DespawnCache& despawn_cache,
                                RecordCache& record_cache,
                                bool refresh_all,
                                std::vector<std::uint32_t>& out_record_slots,
                                ReconcileStats& out_stats);
};

} // namespace gs::game
