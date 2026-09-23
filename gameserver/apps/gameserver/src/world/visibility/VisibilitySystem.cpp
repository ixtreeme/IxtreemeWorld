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
// (resident or ghost). Only read on a record-cache miss, i.e. once per entity
// per tick instead of once per interested recipient.
std::uint32_t CurrentTransformVersion(const flecs::entity& entity)
{
    return entity.has<TransformVersion>() ? entity.get<TransformVersion>().tick : 0;
}

// Canonical 19-byte record for a resident or ghost entity. Ghosts carry no
// MoveIntent, so their move state comes from the ghost record (O(1)).
TransformRecord BuildCanonicalRecord(Zone& zone,
                                     std::uint32_t net_id,
                                     const flecs::entity& entity)
{
    const auto position = entity.get<Position>();
    const auto heading = entity.get<Heading>();
    MoveState state = MoveState::Idle;
    if (entity.has<MoveIntent>()) {
        state = entity.get<MoveIntent>().state;
    } else if (const GhostRecord* ghost = zone.FindGhost(net_id)) {
        state = ghost->snapshot.move_state;
    }
    return EncodeTransformRecord(net_id, position, heading, state);
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

// Recipient-independent despawn payload, built once per net per tick.
const std::vector<std::uint8_t>& DespawnPayloadCached(
    VisibilitySystem::DespawnCache& cache,
    std::uint32_t net_id,
    ReconcileStats& stats)
{
    const auto it = cache.find(net_id);
    if (it != cache.end()) {
        ++stats.despawn_cache_hits;
        return it->second;
    }
    ++stats.despawn_cache_misses;
    return cache.emplace(net_id, MakeDespawn(net_id)).first->second;
}

} // namespace

void VisibilitySystem::ReconcileViewer(Zone& zone,
                                       std::uint32_t viewer_net_id,
                                       const std::vector<AoiCandidate>& candidates,
                                       const SendFn& send,
                                       SpawnCache& spawn_cache,
                                       SnapshotCache& snapshot_cache,
                                       DespawnCache& despawn_cache,
                                       RecordCache& record_cache,
                                       bool refresh_all,
                                       std::vector<std::uint32_t>& out_record_slots,
                                       ReconcileStats& out_stats)
{
    AssertZoneOwner(zone, "zone visibility reconcile");

    out_record_slots.clear();
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
            // same tick -- spawn + update coalescing. The spawn payload is
            // serialized once per net per tick and shared across recipients.
            const BorderEntitySnapshot* snapshot = ResolveOrCache(zone, net_id, snapshot_cache);
            if (snapshot == nullptr) {
                new_visible.erase(net_id);
                continue;
            }
            const auto encoded = spawn_cache.find(net_id);
            if (encoded != spawn_cache.end()) {
                out_stats.payload_bytes += encoded->second.size();
                out_stats.payload_copied_bytes += encoded->second.size();
                send(viewer->session, encoded->second);
            } else {
                auto payload = MakeSpawn(*snapshot);
                out_stats.payload_bytes += payload.size();
                out_stats.payload_copied_bytes += payload.size();
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
        // KEEP: consult the canonical record cache. One hash lookup and one
        // version compare per recipient; the 19-byte record itself is
        // serialized once per entity per tick, regardless of recipient count.
        ++record_cache.requests;
        auto cached = record_cache.index.find(net_id);
        if (cached == record_cache.index.end()) {
            const std::uint32_t version = CurrentTransformVersion(candidate.entity);
            const std::uint32_t slot = static_cast<std::uint32_t>(record_cache.records.size());
            record_cache.records.push_back(BuildCanonicalRecord(zone, net_id, candidate.entity));
            cached = record_cache.index.emplace(net_id, RecordCache::Entry{version, slot}).first;
            ++record_cache.serializations;
        }
        const RecordCache::Entry& entry = cached->second;
        if (refresh_all || entry.version > existing->second) {
            out_record_slots.push_back(entry.slot);
            existing->second = entry.version;
            ++viewer->update_events;
            ++out_stats.updates;
        } else {
            ++out_stats.suppressed;
        }
    }

    // LEAVE: anything no longer in the exact AOI set gets a removal. The
    // despawn payload is recipient-independent and shared per tick; the
    // interest entry is dropped only after the payload is handed to send().
    for (auto it = viewer->visible_net_versions.begin();
         it != viewer->visible_net_versions.end();) {
        if (!new_visible.contains(it->first)) {
            const auto& payload = DespawnPayloadCached(despawn_cache, it->first, out_stats);
            out_stats.payload_bytes += payload.size();
            out_stats.payload_copied_bytes += payload.size();
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
