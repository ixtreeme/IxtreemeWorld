#include "GhostSystem.h"

#include <algorithm>
#include <unordered_set>

#include "../components/MobComponents.h"
#include "../components/NetworkComponents.h"
#include "../components/Tags.h"
#include "../components/TransformComponents.h"
#include "../spatial/SpatialTypes.h"
#include "../zone/Zone.h"
#include "../zone/ZoneManager.h"
#include "../zone/ZoneOwnership.h"

namespace gs::game {

namespace {

// Mutable per-tick fields of a ghost snapshot. The static identity fields
// (net_id, name, class_id, mob_type_id, level) are set at creation and never
// change for a live entity, so an update never copies the name string.
void ApplyMutableSnapshot(GhostRecord& record, const BorderEntitySnapshot& snapshot)
{
    record.snapshot.position = snapshot.position;
    record.snapshot.heading = snapshot.heading;
    record.snapshot.move_state = snapshot.move_state;
    record.snapshot.hp_current = snapshot.hp_current;
    record.snapshot.hp_max = snapshot.hp_max;
}

bool SameSpatialCell(const Position& lhs, const Position& rhs)
{
    return SpatialCellCoord(lhs.x) == SpatialCellCoord(rhs.x) &&
           SpatialCellCoord(lhs.y) == SpatialCellCoord(rhs.y);
}

void CreateGhost(Zone& zone,
                 const BorderEntitySnapshot& snapshot,
                 std::uint32_t source_zone_id,
                 std::uint32_t reconcile_generation)
{
    auto entity = zone.World()
                      .entity()
                      .set<Position>(snapshot.position)
                      .set<Heading>(snapshot.heading)
                      .set<NetId>({snapshot.net_id})
                      // Phase 5B: the ghost's replicated transform starts at
                      // the current world tick (its first observer receives a
                      // spawn carrying the full state).
                      .set<TransformVersion>({zone.WorldTick()})
                      .add<GhostTag>();
    if (snapshot.mob_type_id != 0) {
        entity.set<MobTypeRef>({snapshot.mob_type_id}).add<MobTag>();
    }
    zone.Grid().Insert(snapshot.net_id, snapshot.position, entity);
    zone.Ghosts().push_back(GhostRecord{entity, snapshot, source_zone_id, reconcile_generation});
    zone.GhostIndex()[snapshot.net_id] = zone.Ghosts().size() - 1;
}

// O(1) removal: destroy the entity, drop the grid entry, swap-erase the
// record and fix the derived index. Ghost order is not semantically
// meaningful (consumers iterate / look up by net id only).
void RemoveGhostAt(Zone& zone, std::size_t slot)
{
    auto& ghosts = zone.Ghosts();
    auto& index = zone.GhostIndex();
    const std::uint32_t net_id = ghosts[slot].snapshot.net_id;
    if (ghosts[slot].entity.is_valid()) {
        ghosts[slot].entity.destruct();
    }
    zone.Grid().Remove(net_id, ghosts[slot].snapshot.position);
    const std::uint32_t last_net = ghosts.back().snapshot.net_id;
    ghosts[slot] = std::move(ghosts.back());
    ghosts.pop_back();
    if (slot < ghosts.size()) {
        index[last_net] = slot;
    }
    index.erase(net_id);
}

} // namespace

void GhostSystem::Reconcile(Zone& zone, ZoneManager& zones)
{
    AssertZoneOwner(zone, "zone ghost reconcile");
    auto& state = zone.GhostMaintenance();
    auto& diag = zone.Diagnostics();
    const std::size_t zone_index = zones.FindIndexById(zone.Id());
    const auto& neighbors = zones.NeighborsOf(zone_index);

    ++state.reconcile_generation;
    const std::uint32_t reconcile_generation = state.reconcile_generation;
    std::uint64_t added = 0;
    std::uint64_t removed = 0;
    std::uint64_t updated = 0;

    // The neighbor set is the address space of the ghost set: a topology
    // change (split/merge/retire) invalidates every cursor and forces a full
    // pass so ghosts from former neighbors are dropped deterministically.
    bool topology_changed = neighbors.size() != state.neighbors.size();
    if (!topology_changed) {
        for (std::size_t i = 0; i < neighbors.size(); ++i) {
            if (neighbors[i] != state.neighbors[i].zone_index) {
                topology_changed = true;
                break;
            }
        }
    }
    if (topology_changed) {
        state.force_full = true;
        state.neighbors.clear();
        state.neighbors.reserve(neighbors.size());
        for (const std::size_t neighbor_index : neighbors) {
            state.neighbors.push_back(GhostNeighborCursor{neighbor_index, 0, false});
        }
    }
    // A local residency change (spawn/despawn/transfer) can make a ghost
    // obsolete even when its source neighbor did not republish; the explicit
    // RemoveByNetId calls cover the immediate case, this is the safety net.
    const std::uint64_t local_residency_generation = zone.EntitySetGeneration();
    if (local_residency_generation != state.last_local_residency_generation) {
        state.force_full = true;
        state.last_local_residency_generation = local_residency_generation;
    }

    // Fast path: no neighbor republished since the last reconcile and nothing
    // local changed -> the current ghost set is exactly the desired set.
    // Generation reads are atomic; the buffer copy below still takes the
    // neighbor's publish mutex.
    bool any_changed = state.force_full;
    if (!any_changed) {
        for (const auto& cursor : state.neighbors) {
            if (cursor.zone_index >= zones.ZoneCount() ||
                zones.GetZone(cursor.zone_index).PublishGeneration() !=
                    cursor.publish_generation) {
                any_changed = true;
                break;
            }
        }
    }
    if (!any_changed) {
        diag.ghost_reconcile_skips_since_diag.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // One KEEP/ADD update for a single snapshot from `source_zone_id`.
    auto upsert_ghost = [&](const BorderEntitySnapshot& snapshot, ZoneId source_zone_id) {
        diag.ghost_candidates_examined_since_diag.fetch_add(1, std::memory_order_relaxed);
        if (snapshot.net_id == 0 || zone.IsResident(snapshot.net_id)) {
            return;
        }
        const auto existing = zone.GhostIndex().find(snapshot.net_id);
        if (existing != zone.GhostIndex().end()) {
            auto& record = zone.Ghosts()[existing->second];
            // Phase 5D: the grid entry stores the position the AOI scans, so
            // every ghost position change must reach it -- not only the cell
            // changes (Move is O(1) for the in-cell case).
            const std::int64_t old_cell =
                SpatialCellKey(SpatialCellCoord(record.snapshot.position.x),
                               SpatialCellCoord(record.snapshot.position.y));
            const bool cell_changed =
                !SameSpatialCell(record.snapshot.position, snapshot.position);
            zone.Grid().Move(record.entity, snapshot.net_id, old_cell, snapshot.position);
            if (cell_changed) {
                diag.ghost_spatial_queries_since_diag.fetch_add(1, std::memory_order_relaxed);
                ++updated;
            }
            // Phase 5B: bump the ghost's replicated-transform version only
            // when the replicated transform fields actually changed (an HP
            // refresh must not generate a transform record).
            const bool transform_changed =
                record.snapshot.position.x != snapshot.position.x ||
                record.snapshot.position.y != snapshot.position.y ||
                record.snapshot.position.z != snapshot.position.z ||
                record.snapshot.heading.angle != snapshot.heading.angle ||
                record.snapshot.move_state != snapshot.move_state;
            if (record.entity.is_valid()) {
                record.entity.set<Position>(snapshot.position);
                record.entity.set<Heading>(snapshot.heading);
            }
            ApplyMutableSnapshot(record, snapshot);
            if (transform_changed) {
                zone.NoteTransformChanged(record.entity);
            }
            record.source_zone_id = source_zone_id;
            record.last_seen_generation = reconcile_generation;
            diag.ghost_keep_since_diag.fetch_add(1, std::memory_order_relaxed);
        } else {
            CreateGhost(zone, snapshot, source_zone_id, reconcile_generation);
            ++added;
            diag.ghost_add_since_diag.fetch_add(1, std::memory_order_relaxed);
            diag.ghost_spatial_queries_since_diag.fetch_add(1, std::memory_order_relaxed);
        }
    };

    // Process neighbors in deterministic graph order (first-wins source
    // attribution on the rare transient duplicate during a migration).
    for (auto& cursor : state.neighbors) {
        cursor.changed_this_pass = false;
        if (cursor.zone_index >= zones.ZoneCount()) {
            continue;
        }
        Zone& neighbor = zones.GetZone(cursor.zone_index);
        // Unlocked fast path: a neighbor that has not republished since the
        // last reconcile cannot have changed. Re-checked under the publish
        // mutex below (the generation and the delta must be read atomically:
        // reading the generation outside and the delta inside could apply a
        // newer delta while recording an older generation, permanently
        // skipping the generation in between).
        if (!state.force_full && neighbor.PublishGeneration() == cursor.publish_generation) {
            continue;
        }

        // Delta path: the neighbor advanced exactly one generation and that
        // generation is a complete incremental delta -> touch only the
        // changed/removed nets instead of rescanning the whole buffer. Any
        // generation gap (or a full-refill generation) falls back to the full
        // scan, which is always correct. Snapshot/delta copies happen under
        // the neighbor's publish mutex, consistently with the generation.
        bool delta_applied = false;
        ZoneId neighbor_id = 0;
        {
            std::lock_guard publish_lock(neighbor.PublishMutex());
            const std::uint64_t publish_generation = neighbor.PublishGeneration();
            if (!state.force_full && publish_generation == cursor.publish_generation) {
                continue; // republished and already reconciled while locking
            }
            const std::uint64_t previous_generation = cursor.publish_generation;
            cursor.publish_generation = publish_generation;
            neighbor_id = neighbor.Id();
            if (!state.force_full && publish_generation == previous_generation + 1 &&
                neighbor.PublishDeltaValid()) {
                state.neighbor_scratch.clear();
                for (const std::uint32_t net : neighbor.PublishDeltaNets()) {
                    const auto it = neighbor.PublishIndex().find(net);
                    if (it != neighbor.PublishIndex().end()) {
                        state.neighbor_scratch.push_back(neighbor.PublishBuffer()[it->second]);
                    }
                }
                state.neighbor_removed_scratch = neighbor.PublishDeltaRemoved();
                delta_applied = true;
            } else {
                state.neighbor_scratch.clear();
                state.neighbor_scratch = neighbor.PublishBuffer();
            }
        }
        if (delta_applied) {
            diag.ghost_delta_reconciles_since_diag.fetch_add(1, std::memory_order_relaxed);
            for (const auto& snapshot : state.neighbor_scratch) {
                upsert_ghost(snapshot, neighbor_id);
            }
            for (const std::uint32_t net : state.neighbor_removed_scratch) {
                const auto existing = zone.GhostIndex().find(net);
                if (existing == zone.GhostIndex().end()) {
                    continue;
                }
                // First-wins: a transient duplicate may still be published by
                // another neighbor (migration) -- re-attribute to that
                // neighbor AND refresh the snapshot from its buffer (the
                // removed source's last copy may predate the transfer).
                ZoneId other_source = 0;
                BorderEntitySnapshot other_snapshot;
                for (const auto& other : state.neighbors) {
                    if (other.zone_index == cursor.zone_index ||
                        other.zone_index >= zones.ZoneCount()) {
                        continue;
                    }
                    Zone& other_zone = zones.GetZone(other.zone_index);
                    std::lock_guard other_lock(other_zone.PublishMutex());
                    const auto other_it = other_zone.PublishIndex().find(net);
                    if (other_it != other_zone.PublishIndex().end()) {
                        other_source = other_zone.Id();
                        other_snapshot = other_zone.PublishBuffer()[other_it->second];
                        break;
                    }
                }
                if (other_source != 0) {
                    auto& record = zone.Ghosts()[existing->second];
                    // Always sync the grid's stored position (phase 5D); the
                    // cell-change flag only drives the accounting.
                    const std::int64_t old_cell =
                        SpatialCellKey(SpatialCellCoord(record.snapshot.position.x),
                                       SpatialCellCoord(record.snapshot.position.y));
                    const bool cell_changed =
                        !SameSpatialCell(record.snapshot.position, other_snapshot.position);
                    zone.Grid().Move(record.entity, net, old_cell, other_snapshot.position);
                    if (cell_changed) {
                        diag.ghost_spatial_queries_since_diag.fetch_add(
                            1, std::memory_order_relaxed);
                        ++updated;
                    }
                    const bool transform_changed =
                        record.snapshot.position.x != other_snapshot.position.x ||
                        record.snapshot.position.y != other_snapshot.position.y ||
                        record.snapshot.position.z != other_snapshot.position.z ||
                        record.snapshot.heading.angle != other_snapshot.heading.angle ||
                        record.snapshot.move_state != other_snapshot.move_state;
                    if (record.entity.is_valid()) {
                        record.entity.set<Position>(other_snapshot.position);
                        record.entity.set<Heading>(other_snapshot.heading);
                    }
                    ApplyMutableSnapshot(record, other_snapshot);
                    if (transform_changed) {
                        zone.NoteTransformChanged(record.entity);
                    }
                    record.source_zone_id = other_source;
                    record.last_seen_generation = reconcile_generation;
                    continue;
                }
                RemoveGhostAt(zone, existing->second);
                ++removed;
                diag.ghost_remove_since_diag.fetch_add(1, std::memory_order_relaxed);
            }
            // Ghosts not named by the delta are unchanged: leave them
            // unstamped so the removal pass keeps them via this cursor
            // (changed_this_pass stays false).
        } else {
            // Full-scan generation (or the first/topology-forced pass): the
            // buffer copy above is the exact current content.
            cursor.changed_this_pass = true;
            for (const auto& snapshot : state.neighbor_scratch) {
                upsert_ghost(snapshot, neighbor_id);
            }
        }
    }

    // Removal pass: a ghost survives when it was seen this reconcile, or when
    // its source neighbor was skipped as unchanged in this pass. When the
    // source DID change and dropped the net, a transient migration duplicate
    // may still be published by a neighbor that did not republish this pass
    // (a full scan only re-stamps what it sees): re-attribute it with the
    // other neighbor's snapshot instead of dropping visibility. A local
    // resident can never be a ghost (migration commit), even when its former
    // source zone did not republish: explicit RemoveByNetId covers the
    // immediate case, this is the safety net.
    auto& ghosts = zone.Ghosts();
    for (std::size_t i = ghosts.size(); i-- > 0;) {
        auto& ghost = ghosts[i];
        bool keep = ghost.last_seen_generation == reconcile_generation;
        if (!keep) {
            for (const auto& cursor : state.neighbors) {
                if (!cursor.changed_this_pass && cursor.zone_index < zones.ZoneCount() &&
                    zones.GetZone(cursor.zone_index).Id() == ghost.source_zone_id) {
                    keep = true;
                    break;
                }
            }
        }
        const bool unseen = !keep;
        if (unseen) {
            for (const auto& other : state.neighbors) {
                if (other.changed_this_pass || other.zone_index >= zones.ZoneCount()) {
                    continue;
                }
                Zone& other_zone = zones.GetZone(other.zone_index);
                BorderEntitySnapshot other_snapshot;
                bool found = false;
                {
                    std::lock_guard other_lock(other_zone.PublishMutex());
                    const auto other_it =
                        other_zone.PublishIndex().find(ghost.snapshot.net_id);
                    if (other_it != other_zone.PublishIndex().end()) {
                        other_snapshot = other_zone.PublishBuffer()[other_it->second];
                        found = true;
                    }
                }
                if (!found) {
                    continue;
                }
                {
                    // Always sync the grid's stored position (phase 5D).
                    const std::int64_t old_cell =
                        SpatialCellKey(SpatialCellCoord(ghost.snapshot.position.x),
                                       SpatialCellCoord(ghost.snapshot.position.y));
                    const bool cell_changed =
                        !SameSpatialCell(ghost.snapshot.position, other_snapshot.position);
                    zone.Grid().Move(ghost.entity, ghost.snapshot.net_id, old_cell,
                                     other_snapshot.position);
                    if (cell_changed) {
                        diag.ghost_spatial_queries_since_diag.fetch_add(
                            1, std::memory_order_relaxed);
                        ++updated;
                    }
                }
                const bool transform_changed =
                    ghost.snapshot.position.x != other_snapshot.position.x ||
                    ghost.snapshot.position.y != other_snapshot.position.y ||
                    ghost.snapshot.position.z != other_snapshot.position.z ||
                    ghost.snapshot.heading.angle != other_snapshot.heading.angle ||
                    ghost.snapshot.move_state != other_snapshot.move_state;
                if (ghost.entity.is_valid()) {
                    ghost.entity.set<Position>(other_snapshot.position);
                    ghost.entity.set<Heading>(other_snapshot.heading);
                }
                ApplyMutableSnapshot(ghost, other_snapshot);
                if (transform_changed) {
                    zone.NoteTransformChanged(ghost.entity);
                }
                ghost.source_zone_id = other_zone.Id();
                ghost.last_seen_generation = reconcile_generation;
                keep = true;
                break;
            }
        }
        if (unseen && keep && zone.IsResident(ghost.snapshot.net_id)) {
            keep = false;
        }
        if (!keep) {
            RemoveGhostAt(zone, i);
            ++removed;
            diag.ghost_remove_since_diag.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (state.force_full) {
        state.force_full = false;
        diag.ghost_full_reconciles_since_diag.fetch_add(1, std::memory_order_relaxed);
    }
    // Ghost operations this reconcile: created + removed + moved-between-cells
    // (the expensive entity lifecycle work; a plain field update is not an
    // operation).
    diag.ghost_entities_since_diag.fetch_add(added + removed + updated,
                                             std::memory_order_relaxed);
    diag.ghost_count.store(static_cast<std::uint32_t>(ghosts.size()),
                           std::memory_order_relaxed);
}

void GhostSystem::RebuildExact(Zone& zone, ZoneManager& zones)
{
    AssertZoneOwner(zone, "zone ghost rebuild exact");

    Clear(zone);
    const auto& neighbors = zones.NeighborsOf(zones.FindIndexById(zone.Id()));
    if (neighbors.empty()) {
        return;
    }

    std::unordered_set<std::uint32_t> added_net_ids;
    for (const std::size_t neighbor_index : neighbors) {
        if (neighbor_index >= zones.ZoneCount()) {
            continue;
        }
        Zone& neighbor = zones.GetZone(neighbor_index);
        const ZoneId neighbor_id = neighbor.Id();
        std::vector<BorderEntitySnapshot> snapshots;
        {
            std::lock_guard publish_lock(neighbor.PublishMutex());
            snapshots = neighbor.PublishBuffer();
        }
        for (const auto& snapshot : snapshots) {
            if (snapshot.net_id == 0 || zone.IsResident(snapshot.net_id) ||
                !added_net_ids.insert(snapshot.net_id).second) {
                continue;
            }
            CreateGhost(zone, snapshot, neighbor_id, 0);
        }
    }
    zone.Diagnostics().ghost_count.store(static_cast<std::uint32_t>(zone.Ghosts().size()),
                                         std::memory_order_relaxed);
    zone.Diagnostics().ghost_full_fallbacks_since_diag.fetch_add(1, std::memory_order_relaxed);
}

void GhostSystem::Clear(Zone& zone)
{
    AssertZoneOwner(zone, "zone ghost clear");

    for (auto& ghost : zone.Ghosts()) {
        zone.Grid().Remove(ghost.snapshot.net_id, ghost.snapshot.position);
        if (ghost.entity.is_valid()) {
            ghost.entity.destruct();
        }
    }
    zone.Ghosts().clear();
    zone.GhostIndex().clear();
    auto& state = zone.GhostMaintenance();
    state.neighbors.clear();
    state.force_full = true;
    zone.Diagnostics().ghost_count.store(0, std::memory_order_relaxed);
}

void GhostSystem::RemoveByNetId(Zone& zone, std::uint32_t net_id)
{
    AssertZoneOwner(zone, "zone ghost dedup removal");

    const auto it = zone.GhostIndex().find(net_id);
    if (it == zone.GhostIndex().end()) {
        return;
    }
    RemoveGhostAt(zone, it->second);
    zone.Diagnostics().ghost_count.store(static_cast<std::uint32_t>(zone.Ghosts().size()),
                                         std::memory_order_relaxed);
}

} // namespace gs::game
