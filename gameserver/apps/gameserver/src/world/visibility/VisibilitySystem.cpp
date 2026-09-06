#include "VisibilitySystem.h"

#include <unordered_set>

#include "../replication/ProtocolEncoder.h"
#include "../replication/SnapshotBuilder.h"
#include "../zone/Zone.h"
#include "../zone/ZoneOwnership.h"

namespace gs::game {

std::vector<BorderEntitySnapshot> VisibilitySystem::ReconcileViewer(
    Zone& zone,
    std::uint32_t viewer_net_id,
    const std::vector<std::uint32_t>& candidates,
    const SendFn& send)
{
    AssertZoneOwner(zone, "zone visibility reconcile");

    auto* viewer = zone.FindPlayer(viewer_net_id);
    if (viewer == nullptr || !viewer->session) {
        return {};
    }

    std::unordered_set<std::uint32_t> new_visible;
    new_visible.reserve(candidates.size());
    std::vector<BorderEntitySnapshot> visible_snapshots;
    visible_snapshots.reserve(candidates.size());

    for (const auto net_id : candidates) {
        auto snapshot = ResolveVisibleSnapshot(zone, net_id);
        if (!snapshot) {
            continue;
        }
        new_visible.insert(snapshot->net_id);
        visible_snapshots.push_back(std::move(*snapshot));
    }

    for (const auto& visible_entity : visible_snapshots) {
        if (!viewer->visible_net_ids.contains(visible_entity.net_id)) {
            send(viewer->session, MakeSpawn(visible_entity));
        }
    }

    for (const std::uint32_t old_net_id : viewer->visible_net_ids) {
        if (!new_visible.contains(old_net_id)) {
            send(viewer->session, MakeDespawn(old_net_id));
        }
    }
    viewer->visible_net_ids = std::move(new_visible);
    return visible_snapshots;
}

} // namespace gs::game
