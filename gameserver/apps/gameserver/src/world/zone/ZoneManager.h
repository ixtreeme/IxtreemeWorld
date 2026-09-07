#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "map/MapData.h"

#include "Zone.h"
#include "ZoneGraph.h"
#include "../partition/RegionDefinition.h"
#include "../partition/ZonePartition.h"

// Owns zone lifetimes and answers zone-lookup questions. Knows nothing about
// gameplay, threads, or networking: creation/lookup/destruction only.
//
// Partitioning: each region owns a forest of ZonePartition metadata whose
// leaves mirror the simulating zones. Split creates child zones + metadata;
// merge collapses them. Zone slots are append-only (indices stay stable, so
// OwnerMap fast-path caches never dangle); retired zones keep their slot
// but stop simulating and hold no entities.
namespace gs::game {

class ZoneManager {
public:
    void BuildFromWorldLogic(const mx::map::WorldLogic& logic, float fallback_extent);
    void Clear();

    std::size_t ZoneCount() const noexcept
    {
        return zones_.size();
    }
    Zone& GetZone(std::size_t index)
    {
        return *zones_.at(index);
    }
    const Zone& GetZone(std::size_t index) const
    {
        return *zones_.at(index);
    }

    // Returns ZoneCount() when not found (matches old FindZone* semantics).
    std::size_t FindIndexById(ZoneId id) const;
    // Tree descent over active leaves (O(depth)), linear fallback over
    // simulating zones for positions outside the partition forest.
    std::size_t FindIndexForPosition(float world_x, float world_y) const;

    // Partition forest: one root per region. Supervisor only.
    const std::vector<std::unique_ptr<ZonePartition>>& PartitionRoots() const noexcept
    {
        return partition_roots_;
    }
    const std::vector<RegionDefinition>& Regions() const noexcept
    {
        return regions_;
    }
    std::vector<ZonePartition*> GetActiveLeaves() const;

    // Quadtree split of an active leaf into 4 children. Creates the child
    // Zone objects + metadata and disables the parent; the caller moves the
    // entities (batch transfer) and updates directory/routing. Returns false
    // when the zone cannot split (not a leaf, too small, max depth).
    bool SplitZone(ZoneId zone_id, std::vector<ZoneId>& out_new_zone_ids);
    // Collapses sibling leaves back into one zone under their common parent.
    // Creates the merged Zone object; the caller moves entities, then the
    // children are retired via RetireZone().
    bool MergeZones(const std::vector<ZoneId>& zone_ids, ZoneId& out_merged_zone_id);
    // Marks a zone retired: stops simulation, keeps the slot + metadata.
    // The caller must have drained its entities first.
    void RetireZone(ZoneId zone_id);

    const std::vector<std::size_t>& NeighborsOf(std::size_t zone_index) const;
    bool AnyTickInProgress() const;

    // Pushes a command into a zone's inbound queue (oob-safe no-op).
    // Waking the supervisor/scheduler after the push is the caller's job.
    void PostCommand(std::size_t zone_index, ZoneCommandQueue::Command command);

    ZoneGraph& Graph() noexcept
    {
        return graph_;
    }

private:
    ZoneId AllocateZoneId();

    std::vector<std::unique_ptr<Zone>> zones_;
    ZoneGraph graph_;
    std::vector<std::unique_ptr<ZonePartition>> partition_roots_;
    std::vector<RegionDefinition> regions_;
    ZoneId next_zone_id_ = 1;
};

} // namespace gs::game
