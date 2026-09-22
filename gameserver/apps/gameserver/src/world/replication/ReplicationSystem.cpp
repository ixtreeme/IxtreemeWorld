#include "ReplicationSystem.h"

#include <algorithm>
#include <cstdint>

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
    // Pre-size the per-tick caches: without this every tick pays a full
    // rehash ladder (0->512 buckets) on top of the inserts themselves.
    // (A per-record shared-encoding cache was also tried here and measured:
    // at 19 bytes/record the map overhead eats the re-encode saving, so it
    // was removed again. Revisit if records grow.)
    {
        const std::size_t residents =
            zone.Players().size() +
            zone.Diagnostics().mob_count.load(std::memory_order_relaxed) + zone.Ghosts().size();
        spawn_cache.reserve(64);
        snapshot_cache.reserve(residents + 16);
    }

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
        // viewers discover it simultaneously. Spawn/despawn bytes are
        // measured for the load field's ReplicationPressure channel.
        std::uint64_t spawn_bytes = 0;
        auto visible_snapshots = VisibilitySystem::ReconcileViewer(
            zone, viewer_net_id, candidates, send, spawn_cache, snapshot_cache, &spawn_bytes);

        const auto viewer_snapshot = BuildPlayerSnapshot(zone, viewer_entity);
        auto frame = EncodeTransformFrame(viewer_snapshot, visible_snapshots, zone.TickIndex());
        const std::uint64_t frame_bytes = frame.size();
        const std::uint64_t records = 1 + visible_snapshots.size();
        send(binding.session, std::move(frame));
        transform_records_sent += static_cast<std::size_t>(records);

        // Load field attribution: replication is measured in BYTES (the only
        // measured channel) and attributed to the viewer position -- fanout
        // is included because every recipient's frame is encoded separately.
        if (auto* load = zone.LoadBins().CellFor(viewer_position.x, viewer_position.y)) {
            const std::uint64_t total_bytes = spawn_bytes + frame_bytes;
            load->repl_bytes += static_cast<std::uint32_t>(
                total_bytes > 0xFFFFFFFFu ? 0xFFFFFFFFu : total_bytes);
            load->repl_records += static_cast<std::uint32_t>(
                records > 0xFFFFFFFFu ? 0xFFFFFFFFu : records);
        }
    }
    return transform_records_sent;
}

} // namespace gs::game
