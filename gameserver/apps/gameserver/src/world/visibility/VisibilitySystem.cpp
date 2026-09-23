#include "VisibilitySystem.h"

#include <unordered_set>


#include "../components/CombatComponents.h"
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

// Network LOD tier: 0 critical, 1 near, 2 normal, 3 reduced.
std::uint32_t PeriodForTier(std::uint8_t tier, const ReplicationConfig::NetworkLodTierConfig& cfg)
{
    switch (tier) {
    case 0:
        return cfg.critical_period_ticks;
    case 1:
        return cfg.near_period_ticks;
    case 2:
        return cfg.normal_period_ticks;
    default:
        return cfg.reduced_period_ticks;
    }
}

// Recipient-relative tier: distance bands, promoted to critical by active
// combat state. Disabled Network LOD collapses everything to near (20 Hz).
std::uint8_t TierFor(const AoiCandidate& candidate,
                     const flecs::entity& entity,
                     const ReplicationConfig& config)
{
    // The v1 reference path has no Network LOD scheduling (every changed
    // entity is due immediately); the Network LOD is a v2-only system.
    if (!config.v2_enabled || !config.network_lod_enabled) {
        return 1;
    }
    if (entity.is_valid() && entity.has<AttackCooldown>() &&
        entity.get<AttackCooldown>().remaining > 0.0f) {
        return 0;
    }
    const auto& cfg = config.network_lod;
    if (candidate.distance_sq <= cfg.near_distance_m * cfg.near_distance_m) {
        return 1;
    }
    if (candidate.distance_sq <= cfg.normal_distance_m * cfg.normal_distance_m) {
        return 2;
    }
    return 3;
}

std::uint32_t CurrentTransformVersion(const flecs::entity& entity)
{
    return entity.has<TransformVersion>() ? entity.get<TransformVersion>().tick : 0;
}

// Current replicated transform values (resident or ghost).
void ReadTransform(Zone& zone,
                   std::uint32_t net_id,
                   const flecs::entity& entity,
                   float& x,
                   float& y,
                   float& z,
                   std::uint16_t& heading_q,
                   std::uint8_t& move_state)
{
    const auto position = entity.get<Position>();
    const auto heading = entity.get<Heading>();
    MoveState state = MoveState::Idle;
    if (entity.has<MoveIntent>()) {
        state = entity.get<MoveIntent>().state;
    } else if (const GhostRecord* ghost = zone.FindGhost(net_id)) {
        state = ghost->snapshot.move_state;
    }
    x = position.x;
    y = position.y;
    z = position.z;
    heading_q = QuantizeHeading(heading.angle);
    move_state = static_cast<std::uint8_t>(state);
}

TransformRecord BuildCanonicalRecord(Zone& zone,
                                     std::uint32_t net_id,
                                     const flecs::entity& entity)
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    std::uint16_t heading_q = 0;
    std::uint8_t move_state = 0;
    ReadTransform(zone, net_id, entity, x, y, z, heading_q, move_state);
    const Position position{x, y, z};
    Heading heading;
    heading.angle = 0.0f;
    // QuantizeHeading is applied inside EncodeTransformRecord, so rebuild the
    // angle from the quantized value to keep the canonical bytes stable.
    const float angle = (static_cast<float>(heading_q) / 65535.0f) * kTwoPi;
    return EncodeTransformRecord(net_id, position, Heading{angle},
                                 static_cast<MoveState>(move_state));
}

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
                                       const ReplicationConfig& config,
                                       std::uint32_t world_tick,
                                       bool refresh_all,
                                       std::vector<std::uint32_t>& out_record_slots,
                                       std::vector<std::uint8_t>& out_delta_payload,
                                       ReconcileStats& out_stats)
{
    AssertZoneOwner(zone, "zone visibility reconcile");

    out_record_slots.clear();
    out_delta_payload.clear();
    out_stats = ReconcileStats{};

    auto* viewer = zone.FindPlayer(viewer_net_id);
    if (viewer == nullptr || !viewer->session) {
        return;
    }

    auto& new_visible = t_new_visible;
    new_visible.clear();
    new_visible.reserve(candidates.size());

    std::uint32_t delta_count = 0;
    for (const AoiCandidate& candidate : candidates) {
        const std::uint32_t net_id = candidate.net_id;
        new_visible.insert(net_id);
        // Authoritative entity for this net (resident first, then the ghost
        // record): the grid entry's handle is only a fast-path hint and a
        // stale one would silently read old component values.
        flecs::entity source = zone.FindEntity(net_id);
        if (!source.is_valid()) {
            if (const GhostRecord* ghost = zone.FindGhost(net_id)) {
                source = ghost->entity;
            } else {
                source = candidate.entity;
            }
        }
        const auto existing = viewer->visible_net_versions.find(net_id);
        if (existing == viewer->visible_net_versions.end()) {
            // ENTER: the spawn carries the full initial state (including the
            // current transform), so no delta is generated for the same tick.
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
            Zone::RecipientEntity known;
            known.version = CurrentTransformVersion(source);
            known.x = snapshot->position.x;
            known.y = snapshot->position.y;
            known.z = snapshot->position.z;
            known.heading_q = QuantizeHeading(snapshot->heading.angle);
            known.move_state = static_cast<std::uint8_t>(snapshot->move_state);
            known.tier = TierFor(candidate, source, config);
            known.last_sent_tick = world_tick;
            known.next_due_tick =
                world_tick + PeriodForTier(known.tier, config.network_lod);
            viewer->visible_net_versions.emplace(net_id, known);
            ++viewer->spawn_events;
            ++out_stats.spawns;
            continue;
        }

        // KEEP: Network LOD due check, then the field-level delta against the
        // recipient's known state.
        Zone::RecipientEntity& known = existing->second;
        known.tier = TierFor(candidate, source, config);
        const std::uint32_t period = PeriodForTier(known.tier, config.network_lod);
        const bool due = refresh_all || world_tick >= known.next_due_tick;
        if (!due) {
            ++viewer->suppressed_events;
            ++out_stats.suppressed;
            continue;
        }
        // Starvation is about the PENDING CHANGE age (the entity's last
        // change tick), not the time since the last send: a static entity
        // needs no send at all.
        const std::uint32_t change_tick = CurrentTransformVersion(source);
        const std::uint32_t pending_age =
            world_tick >= change_tick ? world_tick - change_tick : 0;
        const bool starved = pending_age > config.max_defer_ticks;
        const bool critical = refresh_all || known.tier == 0 || starved;
        const bool budget_limited = config.v2_enabled &&
                                    (config.budget_max_records > 0 ||
                                     config.budget_max_bytes > 0);
        if (budget_limited && !critical) {
            const bool records_full =
                config.budget_max_records > 0 && delta_count >= config.budget_max_records;
            const bool bytes_full =
                config.budget_max_bytes > 0 &&
                out_delta_payload.size() >= config.budget_max_bytes;
            if (records_full || bytes_full) {
                known.next_due_tick = world_tick + 1; // retry next frame
                ++out_stats.deferred;
                ++out_stats.budget_hits;
                continue;
            }
        }

        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        std::uint16_t heading_q = 0;
        std::uint8_t move_state = 0;
        ReadTransform(zone, net_id, source, x, y, z, heading_q, move_state);

        std::uint8_t mask = 0;
        if (x != known.x || y != known.y || z != known.z) {
            mask |= kTransformFieldPosition;
        }
        if (heading_q != known.heading_q) {
            mask |= kTransformFieldHeading;
        }
        if (move_state != known.move_state) {
            mask |= kTransformFieldMoveState;
        }
        if (mask == 0) {
            known.version = CurrentTransformVersion(source);
            known.next_due_tick = world_tick + period;
            ++viewer->suppressed_events;
            ++out_stats.suppressed;
            continue;
        }
        if (refresh_all) {
            mask = kTransformFieldAll;
        }

        if (!config.v2_enabled) {
            // v1 reference: the canonical 19-byte record via the shared cache.
            ++record_cache.requests;
            auto cached = record_cache.index.find(net_id);
            if (cached == record_cache.index.end()) {
                const std::uint32_t version = CurrentTransformVersion(source);
                const std::uint32_t slot =
                    static_cast<std::uint32_t>(record_cache.records.size());
                record_cache.records.push_back(BuildCanonicalRecord(zone, net_id, source));
                cached = record_cache.index
                             .emplace(net_id, RecordCache::Entry{version, slot})
                             .first;
                ++record_cache.serializations;
            }
            out_record_slots.push_back(cached->second.slot);
            ++out_stats.full_records;
        } else {
            AppendTransformDelta(out_delta_payload,
                                 net_id,
                                 mask,
                                 Position{x, y, z},
                                 (static_cast<float>(heading_q) / 65535.0f) * kTwoPi,
                                 static_cast<MoveState>(move_state));
            ++delta_count;
            const std::size_t record_size = 5 + ((mask & kTransformFieldPosition) ? 12 : 0) +
                                            ((mask & kTransformFieldHeading) ? 2 : 0) +
                                            ((mask & kTransformFieldMoveState) ? 1 : 0);
            out_stats.delta_bytes += record_size;
            if (mask == kTransformFieldAll) {
                ++out_stats.full_records;
            } else {
                ++out_stats.delta_records;
            }
        }
        if (starved) {
            ++out_stats.starvation_bypasses;
            ++out_stats.critical_records;
        } else if (known.tier == 0) {
            ++out_stats.critical_records;
        }
        if (pending_age > out_stats.max_defer_ticks) {
            out_stats.max_defer_ticks = pending_age;
        }
        ++out_stats.tier_counts[known.tier & 0x03];
        known.version = CurrentTransformVersion(source);
        known.x = x;
        known.y = y;
        known.z = z;
        known.heading_q = heading_q;
        known.move_state = move_state;
        known.last_sent_tick = world_tick;
        known.next_due_tick = world_tick + period;
        ++viewer->update_events;
        ++out_stats.updates;
    }

    // LEAVE: anything no longer in the exact AOI set gets a removal. The
    // despawn payload is recipient-independent and shared per tick; the
    // interest entry (the client baseline) is dropped with it.
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
