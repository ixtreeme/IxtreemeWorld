#include "ZoneManager.h"

#include <algorithm>
#include <cassert>

#include "common/Logging.h"

#include "../visibility/GhostSystem.h"
#include "ZoneOwnership.h"

namespace gs::game {
namespace {
ZonePartition* FindNodeInRoots(const std::vector<std::unique_ptr<ZonePartition>>& roots, ZoneId id)
{
    for (const auto& root : roots) {
        if (auto* found = FindPartitionNode(root.get(), id)) {
            return found;
        }
    }
    return nullptr;
}

// A zone that leaves the simulating topology (retired or tombstoned) never
// ticks again, so its LOD tier gauges would freeze at their last evaluation.
// World-wide tier reports sum the gauges across ALL zone slots, so a stale
// gauge would permanently double-count residents (e.g. an aborted split's
// staged children that received some transfers before the rollback). Reset
// them whenever a zone is taken out of simulation.
void ResetZoneDiagnosticGauges(Zone& zone) noexcept
{
    auto& diag = zone.Diagnostics();
    diag.lod_full.store(0, std::memory_order_relaxed);
    diag.lod_reduced.store(0, std::memory_order_relaxed);
    diag.lod_low.store(0, std::memory_order_relaxed);
    diag.lod_dormant.store(0, std::memory_order_relaxed);
    diag.ghost_count.store(0, std::memory_order_relaxed);
}

} // namespace

void ZoneManager::BuildInitialPartition(const InitialPartition& partition)
{
    zones_.clear();
    partition_roots_.clear();
    retired_pending_.clear();
    reusable_slots_.clear();
    regions_ = partition.regions;

    // Server zone ids are allocated here, 1..N in leaf order -- independent
    // of any map AreaId (R2).
    zones_.reserve(partition.leaves.size());
    ZoneId next_id = 1;
    for (const auto& leaf : partition.leaves) {
        auto zone = std::make_unique<Zone>(next_id++, leaf.name, leaf.bounds);
        zone->SetRegion(leaf.region);
        zones_.push_back(std::move(zone));
    }
    next_zone_id_ = next_id;

    // One partition root per region; every initial leaf is a depth-1 child
    // of its region's root (BuildInitialPartition guarantees the leaves tile
    // their region, so no point of the world is left without an owner and
    // no zone escapes the load monitor).
    partition_roots_.reserve(regions_.size());
    for (const auto& region : regions_) {
        auto root = std::make_unique<ZonePartition>(0, region.id, region.bounds, 0);
        for (auto& zone : zones_) {
            if (zone->Region() == region.id) {
                auto leaf = std::make_unique<ZonePartition>(zone->Id(), region.id, zone->Bounds(), 1);
                leaf->parent = root.get();
                root->children.push_back(std::move(leaf));
            }
        }
        if (root->children.empty()) {
            // Not produced by BuildInitialPartition; kept as a loud guard.
            LOG_ERROR("partition: region {} '{}' has no initial leaf", region.id, region.name);
            root->simulation_enabled = false;
        }
        partition_roots_.push_back(std::move(root));
    }

    graph_.Rebuild(zones_);

    const auto now = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < zones_.size(); ++i) {
        zones_[i]->NextTick() = now;
        zones_[i]->ConfigureLoadBins(load_field_mapping_);
        const auto& zone = *zones_[i];
        LOG_INFO("Game sim zone registered: id={} name='{}' bounds=({}, {})-({}, {}) neighbors={} region={}",
                 zone.Id(),
                 zone.Name(),
                 zone.Bounds().min_x,
                 zone.Bounds().min_y,
                 zone.Bounds().max_x,
                 zone.Bounds().max_y,
                 graph_.Neighbors(i).size(),
                 zone.Region());
    }
}

void ZoneManager::Clear()
{
    zones_.clear();
    partition_roots_.clear();
    regions_.clear();
    next_zone_id_ = 1;
    retired_pending_.clear();
    reusable_slots_.clear();
    graph_.Rebuild(zones_);
}

ZoneId ZoneManager::AllocateZoneId()
{
    return next_zone_id_++;
}

std::size_t ZoneManager::PlaceZone(std::unique_ptr<Zone> zone)
{
    while (!reusable_slots_.empty()) {
        // Lowest free slot first: live zones pack toward the front, so the
        // vacant tail can be trimmed (TrimVacantTail) and the table follows
        // the CURRENT topology instead of its historical high-water mark.
        const auto lowest = std::min_element(reusable_slots_.begin(), reusable_slots_.end());
        const std::size_t index = *lowest;
        reusable_slots_.erase(lowest);
        // Proven free at reclaim time, and the supervisor cannot re-reference
        // a Retired slot since; the cheap recheck only keeps a logic error
        // from ever destroying a live zone.
        if (index < zones_.size() && zones_[index]->Partition() == PartitionState::Retired &&
            !zones_[index]->SimulationEnabled() && zones_[index]->Entities().empty()) {
            zones_[index] = std::move(zone); // destroys the retired Zone + flecs world
            ++reused_total_;
            return index;
        }
    }
    zones_.push_back(std::move(zone));
    return zones_.size() - 1;
}

void ZoneManager::NoteRetired(std::size_t index)
{
    for (const auto& pending : retired_pending_) {
        if (pending.index == index) {
            return;
        }
    }
    retired_pending_.push_back(RetiredSlot{index, world_tick_});
}

bool ZoneManager::SlotReclaimable(std::size_t index, ReclaimStats& blocked) const
{
    const Zone& zone = *zones_[index];
    if (zone.Partition() != PartitionState::Retired || zone.SimulationEnabled() ||
        zone.TickInProgress().load(std::memory_order_acquire) || !zone.Entities().empty() ||
        !zone.Players().empty() || !zone.NetBySession().empty() || !zone.Commands().Empty() ||
        zone.Grid().Size() != 0 || !zone.Ghosts().empty()) {
        ++blocked.blocked_state;
        return false;
    }
    for (const auto& root : partition_roots_) {
        if (FindPartitionNode(root.get(), zone.Id()) != nullptr) {
            ++blocked.blocked_node; // e.g. a split parent: the merge anchor
            return false;
        }
    }
    for (std::size_t other = 0; other < zones_.size(); ++other) {
        if (other == index || zones_[other]->Partition() == PartitionState::Retired) {
            continue; // a retired zone never reads its cursors again
        }
        for (const std::size_t neighbor : graph_.Neighbors(other)) {
            if (neighbor == index) {
                ++blocked.blocked_neighbor;
                return false;
            }
        }
        for (const auto& cursor : zones_[other]->GhostMaintenance().neighbors) {
            if (cursor.zone_index == index) {
                ++blocked.blocked_neighbor; // reconciles away on that zone's next tick
                return false;
            }
        }
    }
    return true;
}

std::vector<ZoneId> ZoneManager::ReclaimRetiredSlots(
    std::uint32_t world_tick, const std::function<bool(std::size_t)>& externally_referenced)
{
    blocked_ = ReclaimStats{}; // blocked_* are last-pass gauges
    std::vector<ZoneId> reclaimed;
    for (auto it = retired_pending_.begin(); it != retired_pending_.end();) {
        const std::size_t index = it->index;
        if (world_tick - it->retired_tick < kReclaimGraceTicks || !SlotReclaimable(index, blocked_)) {
            ++it;
            continue;
        }
        if (externally_referenced && externally_referenced(index)) {
            ++blocked_.blocked_external;
            ++it;
            continue;
        }
        // Proven unreferenced: release the retired zone's storage NOW (flecs
        // world with its deleted entities' tables, spatial grid, load bins,
        // index maps) instead of when a split/merge reuses the slot. The slot
        // keeps a vacant placeholder -- same id (ids are never reused, so
        // stale-id lookups still resolve to a Retired zone), empty bounds.
        const ZoneId id = zones_[index]->Id();
        auto vacant = std::make_unique<Zone>(id, "vacant", mx::map::Rect{});
        vacant->SetRegion(zones_[index]->Region());
        vacant->SetPartition(PartitionState::Retired);
        vacant->SetSimulationEnabled(false);
        zones_[index] = std::move(vacant);
        reusable_slots_.push_back(index);
        ++reclaimed_total_;
        reclaimed.push_back(id);
        it = retired_pending_.erase(it);
    }
    if (!reclaimed.empty()) {
        TrimVacantTail();
    }
    return reclaimed;
}

void ZoneManager::TrimVacantTail()
{
    // Only proven-free slots are removed, and only from the END, so no live
    // index moves. Every holder was proven absent for these slots, and an
    // index at or past ZoneCount() fails loudly in GetZone (.at()).
    bool trimmed = false;
    while (!zones_.empty()) {
        const std::size_t last = zones_.size() - 1;
        const auto free_it = std::find(reusable_slots_.begin(), reusable_slots_.end(), last);
        if (free_it == reusable_slots_.end()) {
            break;
        }
        reusable_slots_.erase(free_it);
        zones_.pop_back();
        ++trimmed_total_;
        trimmed = true;
    }
    if (trimmed) {
        graph_.Rebuild(zones_);
    }
}

ZoneManager::ReclaimStats ZoneManager::GetReclaimStats() const
{
    ReclaimStats stats = blocked_;
    stats.retired_pending = retired_pending_.size();
    stats.reusable = reusable_slots_.size();
    stats.reclaimed_total = reclaimed_total_;
    stats.reused_total = reused_total_;
    stats.trimmed_total = trimmed_total_;
    return stats;
}

std::size_t ZoneManager::FindIndexById(ZoneId id) const
{
    for (std::size_t i = 0; i < zones_.size(); ++i) {
        if (zones_[i]->Id() == id) {
            return i;
        }
    }
    return zones_.size();
}

std::size_t ZoneManager::FindIndexForPosition(float world_x, float world_y) const
{
    // O(depth) half-open descent. The regions tile the world and their leaves
    // tile each region, so a point inside the world has exactly one owner and
    // a point outside it (or non-finite) has none -- it never slides into an
    // edge zone.
    for (const auto& root : partition_roots_) {
        if (!root->bounds.ContainsHalfOpen(world_x, world_y)) {
            continue;
        }
        if (auto* leaf = FindLeaf(root.get(), world_x, world_y)) {
            const std::size_t index = FindIndexById(leaf->zone_id);
            if (index < zones_.size()) {
                return index;
            }
        }
        return zones_.size(); // a region owns the point but no active leaf does
    }
    return zones_.size();
}

std::vector<ZonePartition*> ZoneManager::GetActiveLeaves() const
{
    std::vector<ZonePartition*> leaves;
    for (const auto& root : partition_roots_) {
        CollectActiveLeaves(root.get(), leaves);
    }
    return leaves;
}

bool ZoneManager::PlanSplit(ZoneId zone_id,
                            SplitPlan& out_plan,
                            SplitRejectReason* out_reason,
                            const SplitCenter* center) const
{
    auto reject = [out_reason](SplitRejectReason reason) {
        if (out_reason != nullptr) {
            *out_reason = reason;
        }
        return false;
    };
    if (out_reason != nullptr) {
        *out_reason = SplitRejectReason::None;
    }
    out_plan = SplitPlan{};
    const std::size_t zone_index = FindIndexById(zone_id);
    if (zone_index >= zones_.size()) {
        return reject(SplitRejectReason::UnknownZone);
    }
    const Zone& zone = *zones_[zone_index];
    if (!zone.SimulationEnabled() || zone.Partition() != PartitionState::Leaf) {
        return reject(SplitRejectReason::NotSimulating);
    }
    if (!zone.Commands().Empty()) {
        // Racing commands would strand in the frozen parent: retry next
        // cycle instead of stranding work.
        return reject(SplitRejectReason::CommandsPending);
    }

    const RegionDefinition* region = nullptr;
    for (const auto& r : regions_) {
        if (r.id == zone.Region()) {
            region = &r;
            break;
        }
    }
    if (region == nullptr) {
        return reject(SplitRejectReason::NoRegion);
    }

    const ZonePartition* leaf = FindNodeInRoots(partition_roots_, zone_id);
    if (leaf == nullptr || !leaf->IsLeaf()) {
        return reject(SplitRejectReason::NotLeaf);
    }
    if (leaf->depth >= region->max_partition_depth) {
        return reject(SplitRejectReason::MaxDepth);
    }

    const mx::map::Rect bounds = zone.Bounds();
    const float half_w = (bounds.max_x - bounds.min_x) * 0.5f;
    const float half_h = (bounds.max_y - bounds.min_y) * 0.5f;
    if (half_w < region->min_zone_size || half_h < region->min_zone_size) {
        return reject(SplitRejectReason::TooSmall);
    }

    // Quadtree children tile the parent exactly (shared cut point, no gaps by
    // construction). Half-open ownership is enforced by FindLeaf. The
    // adaptive scorer may supply a load-aware cut point; it is clamped into
    // the min-zone-size-safe range so every child still clears the floor.
    float cut_x = bounds.min_x + half_w;
    float cut_y = bounds.min_y + half_h;
    if (center != nullptr) {
        cut_x = std::clamp(center->x, bounds.min_x + region->min_zone_size,
                           bounds.max_x - region->min_zone_size);
        cut_y = std::clamp(center->y, bounds.min_y + region->min_zone_size,
                           bounds.max_y - region->min_zone_size);
    }
    out_plan.parent_id = zone_id;
    out_plan.parent_index = zone_index;
    out_plan.region_id = zone.Region();
    BuildQuadtreeChildBounds(bounds, cut_x, cut_y, out_plan.child_bounds);
    out_plan.child_depth = static_cast<std::uint8_t>(leaf->depth + 1);
    out_plan.valid = true;
    return true;
}

bool ZoneManager::CreateStagedSplit(const SplitPlan& plan, std::vector<ZoneId>& out_child_ids)
{
    out_child_ids.clear();
    if (!plan.valid) {
        return false;
    }
    // Re-check liveness: cheap, and turns a raced plan into a clean refusal
    // instead of a half-built split.
    ZonePartition* leaf = FindNodeInRoots(partition_roots_, plan.parent_id);
    const std::size_t zone_index = FindIndexById(plan.parent_id);
    if (leaf == nullptr || !leaf->IsLeaf() || zone_index >= zones_.size()) {
        return false;
    }
    Zone& zone = *zones_[zone_index];
    if (!zone.SimulationEnabled() || zone.Partition() != PartitionState::Leaf) {
        return false;
    }

    static const char* kSuffix[4] = {"_nw", "_ne", "_sw", "_se"};
    zone.SetPartition(PartitionState::SplitPending);
    zone.SetSimulationEnabled(false);
    try {
        zones_.reserve(zones_.size() + 4);
        const auto now = std::chrono::steady_clock::now();
        for (int i = 0; i < 4; ++i) {
            const ZoneId child_id = AllocateZoneId();
            auto child =
                std::make_unique<Zone>(child_id, zone.Name() + kSuffix[i], plan.child_bounds[i]);
            child->SetRegion(plan.region_id);
            child->SetPartition(PartitionState::Staging);
            child->SetSimulationEnabled(false);
            child->NextTick() = now;
            // Staged children cannot tick yet, so configuring their load bins
            // here is race-free; a commit turns them into active leaves.
            child->ConfigureLoadBins(load_field_mapping_);
            PlaceZone(std::move(child));
            out_child_ids.push_back(child_id);
        }
    } catch (...) {
        // Nothing transferred yet: tombstone what was appended, restore the
        // parent, and report refusal (no orphan topology: the tree was never
        // touched).
        for (const ZoneId id : out_child_ids) {
            const std::size_t index = FindIndexById(id);
            if (index < zones_.size()) {
                zones_[index]->SetPartition(PartitionState::Retired);
                NoteRetired(index);
                zones_[index]->SetSimulationEnabled(false);
            }
        }
        out_child_ids.clear();
        zone.SetPartition(PartitionState::Leaf);
        zone.SetSimulationEnabled(true);
        graph_.Rebuild(zones_);
        return false;
    }
    graph_.Rebuild(zones_);
    return true;
}

bool ZoneManager::CommitSplit(ZoneId parent_id, const std::vector<ZoneId>& child_ids)
{
    if (child_ids.size() != 4) {
        return false;
    }
    const std::size_t parent_index = FindIndexById(parent_id);
    ZonePartition* leaf = FindNodeInRoots(partition_roots_, parent_id);
    if (parent_index >= zones_.size() || leaf == nullptr || !leaf->IsLeaf()) {
        return false;
    }
    Zone& parent = *zones_[parent_index];
    // Commit gate: every resident must have left the parent. Grid follows
    // entities (validator enforces the correspondence); commands must have
    // drained (plan phase requires an empty queue; a racing command aborts).
    if (!parent.Entities().empty() || !parent.Players().empty() || !parent.NetBySession().empty() ||
        !parent.Commands().Empty()) {
        return false;
    }
    for (const ZoneId child_id : child_ids) {
        const std::size_t index = FindIndexById(child_id);
        if (index >= zones_.size()) {
            return false;
        }
        const Zone& child = *zones_[index];
        if (child.Partition() != PartitionState::Staging || child.SimulationEnabled()) {
            return false;
        }
    }

    // Retire the drained parent first: still no tree change, so a refusal
    // here aborts cleanly.
    if (!RetireZone(parent_id)) {
        return false;
    }

    // NOW the tree mutates: attach staged children, retire the parent node.
    const auto now = std::chrono::steady_clock::now();
    for (const ZoneId child_id : child_ids) {
        const Zone& child = *zones_[FindIndexById(child_id)];
        auto node = std::make_unique<ZonePartition>(child_id, child.Region(), child.Bounds(),
                                                    static_cast<std::uint8_t>(leaf->depth + 1));
        node->parent = leaf;
        leaf->children.push_back(std::move(node));
    }
    leaf->state = PartitionState::Retired;
    leaf->simulation_enabled = false;
    leaf->last_split_time = now;
    // The node just became a GROUP: its leaf-level sustained-low state is
    // stale and its group timer starts from scratch (phase-3 stability).
    leaf->sustained_breach_since = {};
    leaf->field_low_since = {};
    leaf->group_low_since = {};

    for (const ZoneId child_id : child_ids) {
        Zone& child = *zones_[FindIndexById(child_id)];
        child.SetPartition(PartitionState::Leaf);
        child.SetSimulationEnabled(true);
        child.NextTick() = now;
    }

    graph_.Rebuild(zones_);
    return true;
}

void ZoneManager::AbortSplit(ZoneId parent_id, const std::vector<ZoneId>& child_ids)
{
    // Infallible by design: state flips only. The caller rolls entity
    // transfers back BEFORE calling; anything left behind is caught loudly
    // by the debug validator (never silently dropped).
    const std::size_t parent_index = FindIndexById(parent_id);
    if (parent_index < zones_.size()) {
        Zone& parent = *zones_[parent_index];
        parent.SetPartition(PartitionState::Leaf);
        parent.SetSimulationEnabled(true);
        parent.NextTick() = std::chrono::steady_clock::now();
        parent.RefreshResidentCounts();
    }
    for (const ZoneId child_id : child_ids) {
        const std::size_t index = FindIndexById(child_id);
        if (index >= zones_.size()) {
            continue;
        }
        Zone& child = *zones_[index];
        assert(child.Entities().empty() && child.Players().empty() &&
               "AbortSplit: staged child still holds residents (rollback incomplete)");
        child.SetPartition(PartitionState::Retired);
        NoteRetired(index);
        child.SetSimulationEnabled(false);
        child.RefreshResidentCounts();
        // Partial transfers during the aborted split bumped this staged
        // child's LOD gauges (NoteLodInsert) before the rollback; the child
        // never simulates again, so the stale bumps must not leak into the
        // world-wide tier sums.
        ResetZoneDiagnosticGauges(child);
    }
    // The tree was never touched during staging: nothing to detach.
    graph_.Rebuild(zones_);
}

bool ZoneManager::PlanMerge(ZoneId parent_node_id, MergePlan& out_plan) const
{
    out_plan = MergePlan{};
    const ZonePartition* parent = FindNodeInRoots(partition_roots_, parent_node_id);
    if (parent == nullptr || parent->parent == nullptr || parent->children.size() < 2) {
        return false;
    }
    // All ids must be active leaves under ONE common parent (exact sibling
    // set: no partial collapse, keeps tiling exact).
    for (const auto& child : parent->children) {
        if (!child->IsLeaf()) {
            return false;
        }
        const std::size_t index = FindIndexById(child->zone_id);
        if (index >= zones_.size()) {
            return false;
        }
        const Zone& zone = *zones_[index];
        if (!zone.SimulationEnabled() || zone.Partition() != PartitionState::Leaf) {
            return false;
        }
        if (!zone.Commands().Empty()) {
            return false;
        }
        out_plan.child_ids.push_back(child->zone_id);
    }
    out_plan.parent_node_id = parent_node_id;
    out_plan.valid = true;
    return true;
}

bool ZoneManager::CreateStagedMergeTarget(const MergePlan& plan, ZoneId& out_merged_id)
{
    if (!plan.valid) {
        return false;
    }
    ZonePartition* parent = FindNodeInRoots(partition_roots_, plan.parent_node_id);
    if (parent == nullptr) {
        return false;
    }
    // Freeze the children first (still authoritative until commit, but
    // visibly non-schedulable); tree untouched so abort is trivial.
    for (const ZoneId id : plan.child_ids) {
        const std::size_t index = FindIndexById(id);
        if (index >= zones_.size()) {
            // Restore whatever was frozen so far; nothing else mutated.
            for (const ZoneId done : plan.child_ids) {
                if (done == id) {
                    break;
                }
                const std::size_t done_index = FindIndexById(done);
                if (done_index < zones_.size()) {
                    zones_[done_index]->SetPartition(PartitionState::Leaf);
                    zones_[done_index]->SetSimulationEnabled(true);
                }
            }
            return false;
        }
        zones_[index]->SetPartition(PartitionState::Merging);
        zones_[index]->SetSimulationEnabled(false);
    }
    try {
        zones_.reserve(zones_.size() + 1);
        const ZoneId merged_id = AllocateZoneId();
        auto merged = std::make_unique<Zone>(merged_id, "merged", parent->bounds);
        merged->SetRegion(parent->region_id);
        merged->SetPartition(PartitionState::Staging);
        merged->SetSimulationEnabled(false);
        merged->NextTick() = std::chrono::steady_clock::now();
        merged->ConfigureLoadBins(load_field_mapping_);
        PlaceZone(std::move(merged));
        out_merged_id = merged_id;
    } catch (...) {
        for (const ZoneId id : plan.child_ids) {
            const std::size_t index = FindIndexById(id);
            if (index < zones_.size()) {
                zones_[index]->SetPartition(PartitionState::Leaf);
                zones_[index]->SetSimulationEnabled(true);
            }
        }
        graph_.Rebuild(zones_);
        return false;
    }
    graph_.Rebuild(zones_);
    return true;
}

bool ZoneManager::CommitMerge(const MergePlan& plan, ZoneId merged_id)
{
    if (!plan.valid) {
        return false;
    }
    const std::size_t merged_index = FindIndexById(merged_id);
    ZonePartition* parent = FindNodeInRoots(partition_roots_, plan.parent_node_id);
    if (merged_index >= zones_.size() || parent == nullptr) {
        return false;
    }
    Zone& merged = *zones_[merged_index];
    if (merged.Partition() != PartitionState::Staging || merged.SimulationEnabled()) {
        return false;
    }
    // Exact sibling set still intact?
    if (parent->children.size() != plan.child_ids.size()) {
        return false;
    }
    for (const ZoneId id : plan.child_ids) {
        bool found = false;
        for (const auto& child : parent->children) {
            if (child->zone_id == id) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
        const std::size_t index = FindIndexById(id);
        if (index >= zones_.size()) {
            return false;
        }
        // Commit gate: children fully drained (validated again here so a
        // refusal aborts before ANY retire is applied).
        const Zone& child = *zones_[index];
        if (!child.Entities().empty() || !child.Players().empty() || !child.NetBySession().empty() ||
            !child.Commands().Empty()) {
            return false;
        }
    }
    // All gates passed: apply retires (each re-checked inside RetireZone;
    // single-threaded, so no race between gate and apply).
    for (const ZoneId id : plan.child_ids) {
        if (!RetireZone(id)) {
            return false;
        }
    }

    // NOW the tree mutates (only place): collapse onto the merged zone.
    parent->children.clear();
    parent->zone_id = merged_id;
    parent->state = PartitionState::Leaf;
    parent->simulation_enabled = true;
    parent->last_merge_time = std::chrono::steady_clock::now();
    // The node is a leaf again: group state is meaningless, and the stale
    // leaf timers must not leak into the merged zone's future split gate.
    parent->group_low_since = {};
    parent->field_low_since = {};
    parent->sustained_breach_since = {};

    merged.SetPartition(PartitionState::Leaf);
    merged.SetSimulationEnabled(true);
    merged.NextTick() = std::chrono::steady_clock::now();

    graph_.Rebuild(zones_);
    return true;
}

void ZoneManager::AbortMerge(const MergePlan& plan, ZoneId merged_id)
{
    // Infallible by design. Children resume authority with whatever the
    // caller rolled back into them; the staged target becomes a tombstone.
    // The tree never changed, so there is nothing to detach.
    for (const ZoneId id : plan.child_ids) {
        const std::size_t index = FindIndexById(id);
        if (index >= zones_.size()) {
            continue;
        }
        Zone& child = *zones_[index];
        child.SetPartition(PartitionState::Leaf);
        child.SetSimulationEnabled(true);
        child.NextTick() = std::chrono::steady_clock::now();
        child.RefreshResidentCounts();
    }
    const std::size_t merged_index = FindIndexById(merged_id);
    if (merged_index < zones_.size()) {
        Zone& merged = *zones_[merged_index];
        assert(merged.Entities().empty() && merged.Players().empty() &&
               "AbortMerge: staged target still holds residents (rollback incomplete)");
        merged.SetPartition(PartitionState::Retired);
        NoteRetired(merged_index);
        merged.SetSimulationEnabled(false);
        merged.RefreshResidentCounts();
        // The staged merge target may have received transfers before the
        // rollback bumped its gauges; it never simulates again.
        ResetZoneDiagnosticGauges(merged);
    }
    // A failed merge attempt resets the group's sustained-low timer: the
    // conservative direction is to re-earn the merge eligibility.
    for (const auto& root : partition_roots_) {
        if (ZonePartition* parent = FindPartitionNode(root.get(), plan.parent_node_id)) {
            parent->group_low_since = {};
            break;
        }
    }
    graph_.Rebuild(zones_);
}

bool ZoneManager::CanRetire(ZoneId zone_id) const
{
    const std::size_t index = FindIndexById(zone_id);
    if (index >= zones_.size()) {
        return false;
    }
    const Zone& zone = *zones_[index];
    // Authority state only. The grid is deliberately NOT checked here: it
    // also holds non-authoritative ghost entries (see AoiSystem::RebuildInto
    // + GhostSystem::Clear), which RetireZone wipes below. Grid/entity
    // correspondence is enforced by ValidateSpatialIndex on live zones and
    // by the retired Grid().Size()==0 validator check after the wipe.
    return zone.Entities().empty() && zone.Players().empty() && zone.NetBySession().empty() &&
           zone.Commands().Empty();
}

bool ZoneManager::RetireZone(ZoneId zone_id)
{
    const std::size_t index = FindIndexById(zone_id);
    if (index >= zones_.size() || !CanRetire(zone_id)) {
        // Debug builds fail fast: retiring a live zone is always a caller
        // bug (drain first). Production refuses and the transaction aborts.
        assert((index >= zones_.size() || CanRetire(zone_id)) &&
               "RetireZone: zone still holds authority state");
        return false;
    }
    Zone& zone = *zones_[index];
    // Ghost teardown is ownership-gated like every other zone mutation:
    // claim the guard (quiescent supervisor window, so it is always free).
    ZoneWriteGuard guard(zone, "partition retire");
    GhostSystem::Clear(zone);
    // Activity leak-freedom: a retired zone never ticks again, so its last
    // published sources would linger in the field forever. Wipe them here
    // (CanRetire already guaranteed zero players, so this is a no-op in the
    // normal case and a loud backstop otherwise).
    zone.ClearActivitySources();
    zone.RefreshResidentCounts();
    ResetZoneDiagnosticGauges(zone);
    zone.SetSimulationEnabled(false);
    zone.SetPartition(PartitionState::Retired);
    NoteRetired(index);
    if (auto* node = FindNodeInRoots(partition_roots_, zone_id)) {
        node->state = PartitionState::Retired;
        node->simulation_enabled = false;
    }
    graph_.Rebuild(zones_);
    return true;
}

void ZoneManager::ApplyRegionLimits(int max_partition_depth, float min_zone_size_m)
{
    const auto depth =
        static_cast<std::uint8_t>(std::clamp(max_partition_depth, 1, 255));
    for (auto& region : regions_) {
        region.max_partition_depth = depth;
        region.min_zone_size = min_zone_size_m;
    }
}

void ZoneManager::ApplyLoadFieldMapping(const LoadFieldMapping& mapping)
{
    load_field_mapping_ = mapping;
    for (auto& zone : zones_) {
        zone->ConfigureLoadBins(load_field_mapping_);
    }
}

const std::vector<std::size_t>& ZoneManager::NeighborsOf(std::size_t zone_index) const
{
    return graph_.Neighbors(zone_index);
}

bool ZoneManager::AnyTickInProgress() const
{
    for (const auto& zone : zones_) {
        if (zone->TickInProgress().load(std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

bool ZoneManager::AnyCommandsPending() const
{
    // Only zones that can still tick: a command stranded in a retired zone
    // would never drain, and waiting on it would hide exactly the invariant
    // violation the validator reports ("retired zone holds queued commands").
    for (const auto& zone : zones_) {
        if (zone->SimulationEnabled() && !zone->Commands().Empty()) {
            return true;
        }
    }
    return false;
}

bool ZoneManager::CommandsPendingUnder(ZoneId node_id) const
{
    const ZonePartition* node = FindNodeInRoots(partition_roots_, node_id);
    auto pending = [this](ZoneId id) {
        const std::size_t index = FindIndexById(id);
        return index < zones_.size() && !zones_[index]->Commands().Empty();
    };
    if (node == nullptr || node->IsLeaf()) {
        return pending(node_id);
    }
    for (const auto& child : node->children) {
        if (pending(child->zone_id)) {
            return true;
        }
    }
    return false;
}

void ZoneManager::PostCommand(std::size_t zone_index, ZoneCommandQueue::Command command)
{
    if (zone_index >= zones_.size()) {
        return;
    }
    zones_[zone_index]->Commands().Push(std::move(command));
}

} // namespace gs::game
