#include "ReplicationSystem.h"

#include <unordered_map>

#include "../spatial/AoiSystem.h"
#include "../visibility/VisibilitySystem.h"
#include "../zone/Zone.h"
#include "../zone/ZoneOwnership.h"
#include "ProtocolEncoder.h"
#include "SnapshotBuilder.h"

namespace gs::game {

// Pipeline: AOI candidates -> visibility reconcile -> encode -> send.
// The spatial index is maintained incrementally elsewhere; the ghost cache
// is built once per zone tick (not per viewer). Spawn payloads are encoded
// once per newly-visible net per tick and shared across viewers -- bytes on
// the wire are unchanged, only repeated serialization is removed.
std::size_t ReplicationSystem::BroadcastTransforms(Zone& zone, const SendFn& send)
{
    AssertZoneOwner(zone, "zone outgoing snapshot build");

    if (zone.Players().empty()) {
        return 0;
    }

    const auto ghosts = AoiSystem::BuildGhostCache(zone);
    VisibilitySystem::SpawnCache spawn_cache;
    VisibilitySystem::SnapshotCache snapshot_cache;

    std::size_t transform_records_sent = 0;
    for (const auto& [viewer_net_id, binding] : zone.Players()) {
        if (!binding.session) {
            continue;
        }

        const auto viewer_entity = zone.FindEntity(viewer_net_id);
        if (!viewer_entity.is_valid()) {
            continue;
        }
        const auto viewer_position = viewer_entity.get<Position>();

        const auto candidates = AoiSystem::QueryCandidates(zone, viewer_net_id, viewer_position, ghosts);
        zone.Diagnostics().aoi_queries_since_diag.fetch_add(1, std::memory_order_relaxed);

        // Visibility reconcile with a shared per-tick spawn cache: encoding
        // a spawn happens once per newly-visible net per tick even when many
        // viewers discover it simultaneously.
        auto visible_snapshots = VisibilitySystem::ReconcileViewer(
            zone, viewer_net_id, candidates, send, spawn_cache, snapshot_cache);

        const auto viewer_snapshot = BuildPlayerSnapshot(zone, viewer_entity);
        send(binding.session, EncodeTransformFrame(viewer_snapshot, visible_snapshots, zone.TickIndex()));
        transform_records_sent += 1 + visible_snapshots.size();
    }
    return transform_records_sent;
}

} // namespace gs::game
