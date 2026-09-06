#include "ReplicationSystem.h"

#include "../spatial/AoiSystem.h"
#include "../visibility/VisibilitySystem.h"
#include "../zone/Zone.h"
#include "../zone/ZoneOwnership.h"
#include "ProtocolEncoder.h"
#include "SnapshotBuilder.h"

namespace gs::game {

std::size_t ReplicationSystem::BroadcastTransforms(Zone& zone, const SendFn& send)
{
    AssertZoneOwner(zone, "zone outgoing snapshot build");

    if (zone.Players().empty()) {
        return 0;
    }

    AoiSystem::RebuildIndex(zone);

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

        const auto candidates = AoiSystem::QueryCandidates(zone, viewer_net_id, viewer_position);
        auto visible_snapshots = VisibilitySystem::ReconcileViewer(zone, viewer_net_id, candidates, send);

        const auto viewer_snapshot = BuildPlayerSnapshot(zone, viewer_entity);
        send(binding.session, EncodeTransformFrame(viewer_snapshot, visible_snapshots, zone.TickIndex()));
        transform_records_sent += 1 + visible_snapshots.size();
    }
    return transform_records_sent;
}

} // namespace gs::game
