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

    // --- Transactional split (§3-4, §12) ---
    // Plan / Create(staged) / Commit / Abort. The partition TREE mutates
    // only in CommitSplit; staging touches Zone objects alone, so an abort
    // restores the exact pre-split shape with no orphan nodes.
    struct SplitPlan {
        ZoneId parent_id = 0;
        std::size_t parent_index = 0;
        RegionId region_id = 0;
        mx::map::Rect child_bounds[4];
        std::uint8_t child_depth = 0;
        bool valid = false;
    };
    // Pure validation, no mutation. False = routine skip (not a leaf, too
    // small, max depth, unknown region), never an abort.
    bool PlanSplit(ZoneId zone_id, SplitPlan& out_plan) const;
    // Freezes the parent (SplitPending, sim off) and creates 4 staged child
    // Zones (Staging, sim off). No tree/directory/graph-visible change
    // beyond excluding frozen zones from the neighbor graph. On internal
    // failure the parent is restored and false is returned (no tombstones
    // left behind by a failed create).
    bool CreateStagedSplit(const SplitPlan& plan, std::vector<ZoneId>& out_child_ids);
    // Validates the parent is drained, attaches the tree nodes, flips
    // children to active leaves and retires the parent. False = caller must
    // roll back transfers and AbortSplit.
    bool CommitSplit(ZoneId parent_id, const std::vector<ZoneId>& child_ids);
    // Restores the parent to Leaf+simulating, tombstones staged children
    // (Retired; caller must have rolled entities back first), rebuilds the
    // graph. Infallible by design (state flips only).
    void AbortSplit(ZoneId parent_id, const std::vector<ZoneId>& child_ids);

    // --- Transactional merge (§5, §13) ---
    struct MergePlan {
        ZoneId parent_node_id = 0;
        std::vector<ZoneId> child_ids;
        bool valid = false;
    };
    bool PlanMerge(ZoneId parent_node_id, MergePlan& out_plan) const;
    // Creates the staged merge target (Staging, sim off) and freezes the
    // children (Merging, sim off). The tree is untouched until CommitMerge.
    bool CreateStagedMergeTarget(const MergePlan& plan, ZoneId& out_merged_id);
    // Validates children drained, collapses the tree onto the merged zone,
    // retires children, activates the target. False = roll back + AbortMerge.
    bool CommitMerge(const MergePlan& plan, ZoneId merged_id);
    // Restores children to Leaf+simulating, tombstones the staged target.
    // Tree untouched (it never changed). Infallible by design.
    void AbortMerge(const MergePlan& plan, ZoneId merged_id);

    // --- Safe retirement (§6) ---
    // True only when the zone holds no authority state: no entities, no
    // player bindings, no session mappings, no queued commands, empty
    // spatial index. Debug builds assert (programmer error); production
    // returns false and the caller must abort its transaction.
    bool RetireZone(ZoneId zone_id);
    bool CanRetire(ZoneId zone_id) const;

    // Applies validated region limits (config binding). Supervisor only,
    // before any split runs.
    void ApplyRegionLimits(int max_partition_depth, float min_zone_size_m);

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
