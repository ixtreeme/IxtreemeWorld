#include "VisibilitySystem.h"

#include <unordered_set>

#include "../replication/ProtocolEncoder.h"
#include "../replication/SnapshotBuilder.h"
#include "../zone/Zone.h"
#include "../zone/ZoneOwnership.h"

namespace gs::game {
namespace {

// Reused across reconcile calls on the same worker thread: clear() keeps
// buckets/capacity, so steady-state ticks perform no hash-table allocation.
thread_local std::unordered_set<std::uint32_t> t_new_visible;
thread_local std::vector<BorderEntitySnapshot> t_visible_snapshots;

} // namespace

std::vector<BorderEntitySnapshot> VisibilitySystem::ReconcileViewer(
    Zone& zone,
    std::uint32_t viewer_net_id,
    const std::vector<std::uint32_t>& candidates,
    const SendFn& send,
    SpawnCache& spawn_cache,
    SnapshotCache& snapshot_cache)
{
    AssertZoneOwner(zone, "zone visibility reconcile");

    auto* viewer = zone.FindPlayer(viewer_net_id);
    if (viewer == nullptr || !viewer->session) {
        return {};
    }

    auto& new_visible = t_new_visible;
    new_visible.clear();
    new_visible.reserve(candidates.size());
    auto& visible_snapshots = t_visible_snapshots;
    visible_snapshots.clear();
    visible_snapshots.reserve(candidates.size());

    for (const auto net_id : candidates) {
        const BorderEntitySnapshot* snapshot = nullptr;
        const auto cached = snapshot_cache.find(net_id);
        if (cached != snapshot_cache.end()) {
            snapshot = &cached->second;
        } else {
            auto built = ResolveVisibleSnapshot(zone, net_id);
            if (!built) {
                continue;
            }
            snapshot = &snapshot_cache.emplace(net_id, std::move(*built)).first->second;
        }
        new_visible.insert(snapshot->net_id);
        visible_snapshots.push_back(*snapshot);
    }

    for (const auto& visible_entity : visible_snapshots) {
        if (!viewer->visible_net_ids.contains(visible_entity.net_id)) {
            const auto encoded = spawn_cache.find(visible_entity.net_id);
            if (encoded != spawn_cache.end()) {
                send(viewer->session, encoded->second);
            } else {
                auto payload = MakeSpawn(visible_entity);
                send(viewer->session, payload);
                spawn_cache.emplace(visible_entity.net_id, std::move(payload));
            }
        }
    }

    for (const std::uint32_t old_net_id : viewer->visible_net_ids) {
        if (!new_visible.contains(old_net_id)) {
            send(viewer->session, MakeDespawn(old_net_id));
        }
    }
    // The member set takes over the scratch buckets (no rehash); the scratch
    // recovers on next clear()+reserve(). The returned vector is an explicit
    // bounded copy -- the encoder needs its own immutable list.
    viewer->visible_net_ids = std::move(new_visible);
    return std::vector<BorderEntitySnapshot>(visible_snapshots);
}

} // namespace gs::game
