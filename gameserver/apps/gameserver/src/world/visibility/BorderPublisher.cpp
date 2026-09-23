#include "BorderPublisher.h"

#include <algorithm>

#include "../WorldConstants.h"
#include "../components/CombatComponents.h"
#include "../components/MobComponents.h"
#include "../components/MovementComponents.h"
#include "../components/NetworkComponents.h"
#include "../components/Tags.h"
#include "../replication/SnapshotBuilder.h"
#include "../spatial/SpatialTypes.h"
#include "../zone/Zone.h"
#include "../zone/ZoneOwnership.h"

namespace gs::game {

namespace {

// A full refill still runs every this many ticks as a bounded self-healing
// safety net (1 s at 20 Hz): any state change path that forgets to mark an
// entity dirty is corrected within one second instead of never. The dirty
// paths (movement, combat HP, move intent) are complete for the published
// fields by construction; this is defence in depth, not the hot path.
constexpr std::uint32_t kPublishRefreshTicks = 20;

void UpdateMutableSnapshotFields(BorderEntitySnapshot& target, flecs::entity entity)
{
    // Static fields (net_id, name, class_id, mob_type_id, level) are set when
    // the entry is created and never change for a live entity; only these
    // change per tick.
    target.position = entity.get<Position>();
    target.heading = entity.get<Heading>();
    target.move_state = entity.get<MoveIntent>().state;
    const auto hp = entity.get<Hp>();
    target.hp_current = hp.current;
    target.hp_max = hp.max;
}

// Exact, order-insensitive content comparison: the scratch (freshly built
// from authority) matches the canonical buffer iff both hold exactly the same
// snapshots. No hashing: a collision would silently drop a state change.
bool SamePublishContent(Zone& zone)
{
    const auto& scratch = zone.PublishScratch();
    const auto& buffer = zone.PublishBuffer();
    const auto& index = zone.PublishIndex();
    if (scratch.size() != buffer.size()) {
        return false;
    }
    for (const auto& candidate : scratch) {
        const auto it = index.find(candidate.net_id);
        if (it == index.end() || !SameBorderSnapshot(candidate, buffer[it->second])) {
            return false;
        }
    }
    return true;
}

void RebuildPublishIndex(Zone& zone)
{
    auto& index = zone.PublishIndex();
    index.clear();
    const auto& buffer = zone.PublishBuffer();
    index.reserve(buffer.size());
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        index[buffer[i].net_id] = i;
    }
}

// Full refill from authority (spawn/despawn/transfer, periodic refresh).
void FullRefill(Zone& zone)
{
    auto& scratch = zone.PublishScratch();
    scratch.clear();
    scratch.reserve(zone.Players().size() +
                    static_cast<std::size_t>(zone.Diagnostics().mob_count.load(
                        std::memory_order_relaxed)));

    for (const auto& [net_id, binding] : zone.Players()) {
        const auto entity = zone.FindEntity(net_id);
        if (!entity.is_valid()) {
            continue;
        }
        const auto position = entity.get<Position>();
        if (IsInBorderBand(zone.Bounds(), position, kAoiRadiusMeters)) {
            scratch.push_back(BuildPlayerSnapshot(zone, entity));
        }
    }
    // Ghosts carry MobTag for AOI matching but hold no gameplay components;
    // publishing them would read components they don't have, so only
    // authoritative residents are published.
    zone.World().query<const MobTag>().each([&](flecs::entity entity, const MobTag&) {
        if (entity.has<GhostTag>()) {
            return;
        }
        const auto position = entity.get<Position>();
        if (IsInBorderBand(zone.Bounds(), position, kAoiRadiusMeters)) {
            scratch.push_back(BuildMobSnapshot(entity));
        }
    });

    if (!SamePublishContent(zone)) {
        zone.PublishBuffer().swap(zone.PublishScratch());
        RebuildPublishIndex(zone);
        // A full refill rewrote arbitrary entries: the previous generation's
        // delta no longer describes the transition, so consumers rescan.
        zone.InvalidatePublishDelta();
        zone.BumpPublishGeneration();
    }
    zone.Diagnostics().ghost_publish_refreshes_since_diag.fetch_add(
        1, std::memory_order_relaxed);
}

// Incremental path: re-evaluate only the entities that actually changed.
void UpdateDirtyEntities(Zone& zone)
{
    auto& buffer = zone.PublishBuffer();
    auto& index = zone.PublishIndex();
    auto& diag = zone.Diagnostics();
    auto& delta_nets = zone.PublishDeltaNetsScratch();
    auto& delta_removed = zone.PublishDeltaRemovedScratch();
    delta_nets.clear();
    delta_removed.clear();
    bool changed = false;
    for (const flecs::entity entity : zone.DirtyPublishEntities()) {
        if (!entity.is_valid() || entity.has<GhostTag>() || !entity.has<NetId>() ||
            !entity.has<Position>() || !entity.has<Heading>() || !entity.has<MoveIntent>() ||
            !entity.has<Hp>()) {
            continue;
        }
        const std::uint32_t net_id = entity.get<NetId>().value;
        const auto position = entity.get<Position>();
        const bool in_band = IsInBorderBand(zone.Bounds(), position, kAoiRadiusMeters);
        const auto it = index.find(net_id);
        if (in_band) {
            if (it != index.end()) {
                UpdateMutableSnapshotFields(buffer[it->second], entity);
                delta_nets.push_back(net_id);
                diag.ghost_publish_updates_since_diag.fetch_add(1, std::memory_order_relaxed);
                changed = true;
            } else {
                const bool is_player = entity.has<PlayerTag>();
                buffer.push_back(is_player ? BuildPlayerSnapshot(zone, entity)
                                           : BuildMobSnapshot(entity));
                index[buffer.back().net_id] = buffer.size() - 1;
                delta_nets.push_back(net_id);
                diag.ghost_publish_adds_since_diag.fetch_add(1, std::memory_order_relaxed);
                changed = true;
            }
        } else if (it != index.end()) {
            // Left the border band: swap-erase keeps the buffer dense and the
            // index consistent in O(1).
            const std::size_t slot = it->second;
            const std::uint32_t last_net = buffer.back().net_id;
            buffer[slot] = std::move(buffer.back());
            buffer.pop_back();
            if (slot < buffer.size()) {
                index[last_net] = slot;
            }
            index.erase(it);
            delta_removed.push_back(net_id);
            diag.ghost_publish_removes_since_diag.fetch_add(1, std::memory_order_relaxed);
            changed = true;
        }
    }
    if (changed) {
        zone.CommitPublishDelta();
    }
}

} // namespace

void BorderPublisher::Publish(Zone& zone)
{
    AssertZoneOwner(zone, "zone border publish");

    const std::uint64_t entity_generation = zone.EntitySetGeneration();
    const bool entity_set_changed = entity_generation != zone.PublishedEntitySetGeneration();
    const bool has_dirty = !zone.DirtyPublishEntities().empty();
    const bool refresh_due = (zone.TickIndex() % kPublishRefreshTicks) == 0;
    const bool force = zone.ConsumeForcePublish();
    if (!entity_set_changed && !has_dirty && !refresh_due && !force) {
        // Nothing can have changed the border set or any published field:
        // keep the buffer and its generation (consumers skip it entirely).
        zone.Diagnostics().ghost_publish_skips_since_diag.fetch_add(1,
                                                                    std::memory_order_relaxed);
        return;
    }

    // The buffer is read under the mutex by neighbor zones reconciling
    // ghosts, so the fill/update is guarded by the zone's publish mutex
    // (short critical section).
    std::lock_guard publish_lock(zone.PublishMutex());
    if (entity_set_changed || refresh_due || force) {
        FullRefill(zone);
    } else {
        UpdateDirtyEntities(zone);
    }
    zone.NotePublishedEntitySetGeneration(entity_generation);
}

} // namespace gs::game
