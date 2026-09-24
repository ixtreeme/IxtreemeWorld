#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "map/MapData.h"

#include "Zone.h"
#include "ZoneGraph.h"
#include "../activity/LoadFieldTypes.h"
#include "../partition/RegionDefinition.h"
#include "../partition/ZonePartition.h"

// Owns zone lifetimes and answers zone-lookup questions. Knows nothing about
// gameplay, threads, or networking: creation/lookup/destruction only.
//
// Partitioning: each region owns a forest of ZonePartition metadata whose
// leaves mirror the simulating zones. Split creates child zones + metadata;
// merge collapses them. A slot index stays stable for the whole life of the
// zone in it (OwnerMap fast-path caches never dangle); a retired zone keeps
// its slot, simulating nothing and holding no entities, until reclamation
// proves the slot unreferenced -- then a new zone may take the slot (see
// "Retired slot reclamation"). Zone ids are never reused.
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
    // small, max depth, unknown region), never an abort. `out_reason`
    // (optional) carries the structured why-not for diagnostics. `center`
    // (optional) is the adaptive scorer's cut point; it is clamped into the
    // region's min-zone-size-safe range, so an invalid candidate degrades to
    // the nearest valid cut instead of bypassing a gate. Null = the historic
    // geometric midpoint.
    bool PlanSplit(ZoneId zone_id,
                   SplitPlan& out_plan,
                   SplitRejectReason* out_reason = nullptr,
                   const SplitCenter* center = nullptr) const;
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

    // --- Retired slot reclamation (hardening H9) ---
    // A retired zone keeps its slot (and its Zone + flecs world) until
    // reclamation PROVES nothing can still reach it. Then its storage is
    // released at once (a vacant placeholder with the same id stays in the
    // slot), the lowest free slot is reused by the next zone a split/merge
    // creates, and free slots at the end of the table are trimmed. The table
    // follows the live topology plus the reclaim backlog instead of growing
    // by five slots per split->merge cycle. Proof, per retired slot
    // (supervisor, quiescent window -- no tick in flight):
    //   - still Retired, not simulating, no tick claim;
    //   - no authority/ghost/grid state and no queued commands;
    //   - no partition node carries its zone id (a split parent stays the
    //     merge anchor until the merge re-points the node);
    //   - no live zone's graph edge or ghost cursor points at the slot;
    //   - `externally_referenced(slot)` is false (runtime-held indices,
    //     e.g. the owner map's fast-path zone_index);
    //   - retired for at least kReclaimGraceTicks world ticks.
    // Zone ids are never reused: a stale id simply stops resolving.
    static constexpr std::uint32_t kReclaimGraceTicks = 2;
    struct ReclaimStats {
        std::size_t retired_pending = 0;  // retired, not yet proven free
        std::size_t reusable = 0;         // proven free, waiting for reuse
        std::uint64_t reclaimed_total = 0;
        std::uint64_t reused_total = 0;
        std::uint64_t trimmed_total = 0; // vacant slots popped off the table tail
        std::uint64_t blocked_state = 0;  // last pass: slots held back, by reason
        std::uint64_t blocked_node = 0;
        std::uint64_t blocked_neighbor = 0;
        std::uint64_t blocked_external = 0;
    };
    // Returns the ids of the zones whose slots were proven free this pass
    // (the caller drops their remaining id-keyed bookkeeping, e.g. the
    // directory's retired-assignment retention).
    std::vector<ZoneId> ReclaimRetiredSlots(std::uint32_t world_tick,
                                    const std::function<bool(std::size_t)>& externally_referenced);
    bool HasRetiredPending() const noexcept
    {
        return !retired_pending_.empty();
    }
    ReclaimStats GetReclaimStats() const;
    // Validator hook: slots currently proven free (Retired, awaiting reuse).
    const std::vector<std::size_t>& ReusableSlots() const noexcept
    {
        return reusable_slots_;
    }
    // Stamps newly retired slots with the world tick they retired at (the
    // grace clock). Supervisor only, before ReclaimRetiredSlots.
    void SetWorldTick(std::uint32_t world_tick) noexcept
    {
        world_tick_ = world_tick;
    }

    // Applies validated region limits (config binding). Supervisor only,
    // before any split runs.
    void ApplyRegionLimits(int max_partition_depth, float min_zone_size_m);

    // Binds the world-space load field mapping to every zone (existing and
    // future): each zone sizes its local load-bin rectangle to its own bounds
    // over this grid. Call before Start or between supervisor passes; never
    // while a zone tick is in flight.
    void ApplyLoadFieldMapping(const LoadFieldMapping& mapping);

    const std::vector<std::size_t>& NeighborsOf(std::size_t zone_index) const;
    bool AnyTickInProgress() const;
    // Hardening H1: commands wait for their zone's next 20 Hz tick, so "no
    // tick in flight" no longer implies "every posted command was applied".
    // Audits and forced topology operations need both conditions.
    bool AnyCommandsPending() const;
    // Pending commands in the zone of node `node_id` or, for an inner
    // partition node, in any of its direct children.
    bool CommandsPendingUnder(ZoneId node_id) const;

    // Pushes a command into a zone's inbound queue (oob-safe no-op).
    // Waking the supervisor/scheduler after the push is the caller's job.
    void PostCommand(std::size_t zone_index, ZoneCommandQueue::Command command);

    ZoneGraph& Graph() noexcept
    {
        return graph_;
    }

private:
    ZoneId AllocateZoneId();
    // Places a new zone into a reclaimed slot (destroying the retired Zone
    // it held) or appends one. Returns the slot index.
    std::size_t PlaceZone(std::unique_ptr<Zone> zone);
    // Queues a slot that just became Retired for reclamation.
    void NoteRetired(std::size_t index);
    // Slot-level reclaim predicate (state + node + neighbor checks).
    bool SlotReclaimable(std::size_t index, ReclaimStats& blocked) const;
    // Pops proven-free slots off the end of the table (no live index moves).
    void TrimVacantTail();

    struct RetiredSlot {
        std::size_t index = 0;
        std::uint32_t retired_tick = 0;
    };
    std::vector<RetiredSlot> retired_pending_;
    std::vector<std::size_t> reusable_slots_;
    std::uint32_t world_tick_ = 0;
    std::uint64_t reclaimed_total_ = 0;
    std::uint64_t reused_total_ = 0;
    std::uint64_t trimmed_total_ = 0;
    ReclaimStats blocked_{};

    std::vector<std::unique_ptr<Zone>> zones_;
    ZoneGraph graph_;
    std::vector<std::unique_ptr<ZonePartition>> partition_roots_;
    std::vector<RegionDefinition> regions_;
    ZoneId next_zone_id_ = 1;
    LoadFieldMapping load_field_mapping_{};
};

} // namespace gs::game
