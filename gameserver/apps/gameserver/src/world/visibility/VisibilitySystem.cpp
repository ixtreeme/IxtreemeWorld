#include "VisibilitySystem.h"

#include <unordered_set>

#include "../components/ReplicationComponents.h"
#include "../replication/ProtocolEncoder.h"
#include "../replication/SnapshotBuilder.h"
#include "../zone/Zone.h"
#include "../zone/ZoneOwnership.h"

namespace gs::game {
namespace {

// Reused across reconcile calls on the same worker thread: clear() keeps
// buckets/capacity, so steady-state ticks perform no hash-table allocation.
thread_local std::unordered_set<std::uint32_t> t_new_visible;

// Current replicated-transform version from the candidate's entity handle
// (resident or ghost). Only called for already-visible entities, so the AOI
// hot path never pays a component lookup.
std::uint32_t CurrentTransformVersion(const flecs::entity& entity)
{
    return entity.has<TransformVersion>() ? entity.get<TransformVersion>().tick : 0;
}

// Snapshot lookup through the shared per-tick cache. The returned pointer is
// consumed immediately (before any further cache insert).
const BorderEntitySnapshot* ResolveOrCache(Zone& zone,
                                           std::uint32_t net_id,
                                           VisibilitySystem::SnapshotCache& cache)
{
    const auto cached = cache.find(net_id);
    if (cached != cache.end()) {
        return &cached->second;
    }
    auto built = ResolveVisibleSnapshot(zone, net_id);
    if (!built) {
        return nullptr;
    }
    return &cache.emplace(net_id, std::move(*built)).first->second;
}

} // namespace

void VisibilitySystem::ReconcileViewer(Zone& zone,
                                       std::uint32_t viewer_net_id,
                                       const std::vector<AoiCandidate>& candidates,
                                       const SendFn& send,
                                       SpawnCache& spawn_cache,
                                       SnapshotCache& snapshot_cache,
                                       bool refresh_all,
                                       std::vector<BorderEntitySnapshot>& out_updates,
                                       ReconcileStats& out_stats)
{
    AssertZoneOwner(zone, "zone visibility reconcile");

    out_updates.clear();
    out_stats = ReconcileStats{};

    auto* viewer = zone.FindPlayer(viewer_net_id);
    if (viewer == nullptr || !viewer->session) {
        return;
    }

    auto& new_visible = t_new_visible;
    new_visible.clear();
    new_visible.reserve(candidates.size());

    for (const AoiCandidate& candidate : candidates) {
        const std::uint32_t net_id = candidate.net_id;
        new_visible.insert(net_id);
        const auto existing = viewer->visible_net_versions.find(net_id);
        if (existing == viewer->visible_net_versions.end()) {
            // ENTER: the spawn carries the full initial state (including the
            // current transform), so no transform record is generated for the
            // same tick -- spawn + update coalescing.
            const BorderEntitySnapshot* snapshot = ResolveOrCache(zone, net_id, snapshot_cache);
            if (snapshot == nullptr) {
                new_visible.erase(net_id);
                continue;
            }
            const auto encoded = spawn_cache.find(net_id);
            if (encoded != spawn_cache.end()) {
                out_stats.payload_bytes += encoded->second.size();
                send(viewer->session, encoded->second);
            } else {
                auto payload = MakeSpawn(*snapshot);
                out_stats.payload_bytes += payload.size();
                send(viewer->session, payload);
                spawn_cache.emplace(net_id, std::move(payload));
            }
            // The spawn carries the state as of this tick: stamping the
            // current world tick (>= the entity's own version) means any
            // later change is newer and will be replicated.
            viewer->visible_net_versions[net_id] = zone.WorldTick();
            ++viewer->spawn_events;
            ++out_stats.spawns;
            continue;
        }
        // KEEP: replicate the transform only when it changed since the last
        // send (or on the staggered full refresh). Unchanged state produces
        // no record at all.
        const std::uint32_t version = CurrentTransformVersion(candidate.entity);
        if (refresh_all || version > existing->second) {
            const BorderEntitySnapshot* snapshot = ResolveOrCache(zone, net_id, snapshot_cache);
            if (snapshot == nullptr) {
                // Still indexed but no longer resolvable: treat as LEAVE so
                // the client never keeps a stale entity.
                new_visible.erase(net_id);
                continue;
            }
            out_updates.push_back(*snapshot);
            existing->second = version;
            ++viewer->update_events;
            ++out_stats.updates;
        } else {
            ++out_stats.suppressed;
        }
    }

    // LEAVE: anything no longer in the exact AOI set gets a removal. The
    // interest entry is dropped only after the despawn is handed to send().
    for (auto it = viewer->visible_net_versions.begin();
         it != viewer->visible_net_versions.end();) {
        if (!new_visible.contains(it->first)) {
            const auto payload = MakeDespawn(it->first);
            out_stats.payload_bytes += payload.size();
            send(viewer->session, payload);
            ++viewer->despawn_events;
            ++out_stats.despawns;
            it = viewer->visible_net_versions.erase(it);
        } else {
            ++it;
        }
    }
    out_stats.visible = viewer->visible_net_versions.size();
}

} // namespace gs::game
