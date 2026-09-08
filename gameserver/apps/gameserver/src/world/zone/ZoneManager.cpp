#include "ZoneManager.h"

#include <algorithm>
#include <cassert>

#include "common/Logging.h"

#include "../visibility/GhostSystem.h"
#include "ZoneOwnership.h"

namespace gs::game {
namespace {

// Region assignment by CENTER: a map zone straddling a quadrant border
// belongs to exactly one region (no double ownership), chosen by where its
// center falls.
const RegionDefinition* RegionForBounds(const std::vector<RegionDefinition>& regions,
                                        const mx::map::Rect& bounds)
{
    const float cx = (bounds.min_x + bounds.max_x) * 0.5f;
    const float cy = (bounds.min_y + bounds.max_y) * 0.5f;
    return FindRegionContaining(regions, cx, cy);
}

ZonePartition* FindNodeInRoots(const std::vector<std::unique_ptr<ZonePartition>>& roots, ZoneId id)
{
    for (const auto& root : roots) {
        if (auto* found = FindPartitionNode(root.get(), id)) {
            return found;
        }
    }
    return nullptr;
}

} // namespace

void ZoneManager::BuildFromWorldLogic(const mx::map::WorldLogic& logic, float fallback_extent)
{
    zones_.clear();
    partition_roots_.clear();
    regions_ = DefaultRegions();

    if (!logic.zones.empty()) {
        zones_.reserve(logic.zones.size());
        for (const auto& logic_zone : logic.zones) {
            zones_.push_back(std::make_unique<Zone>(logic_zone.id, logic_zone.name, logic_zone.bounds));
        }
    } else {
        zones_.push_back(std::make_unique<Zone>(1,
                                                "fallback",
                                                mx::map::Rect{0.0f, 0.0f, fallback_extent, fallback_extent}));
    }

    ZoneId max_id = 0;
    for (const auto& zone : zones_) {
        max_id = std::max(max_id, zone->Id());
    }
    next_zone_id_ = max_id + 1;

    // One partition root per region; every map zone becomes a depth-1 leaf
    // in its center's region. Regions with no map zones keep a bare root
    // (no coverage there, same as before: lookup falls back to linear).
    partition_roots_.reserve(regions_.size());
    for (const auto& region : regions_) {
        auto root = std::make_unique<ZonePartition>(0, region.id, region.bounds, 0);
        for (auto& zone : zones_) {
            const RegionDefinition* home = RegionForBounds(regions_, zone->Bounds());
            if (home != nullptr && home->id == region.id) {
                zone->SetRegion(region.id);
                auto leaf = std::make_unique<ZonePartition>(zone->Id(), region.id, zone->Bounds(), 1);
                leaf->parent = root.get();
                root->children.push_back(std::move(leaf));
            }
        }
        // A region with no map zones simulates nothing: disable the bare
        // root so it never appears as an active leaf (no phantom zone 0 in
        // scheduling, directory or validation).
        if (root->children.empty()) {
            root->simulation_enabled = false;
        }
        partition_roots_.push_back(std::move(root));
    }

    graph_.Rebuild(zones_);

    const auto now = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < zones_.size(); ++i) {
        zones_[i]->NextTick() = now;
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
    graph_.Rebuild(zones_);
}

ZoneId ZoneManager::AllocateZoneId()
{
    return next_zone_id_++;
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
    // O(depth) tree descent over active leaves first.
    for (const auto& root : partition_roots_) {
        if (!root->bounds.Contains(world_x, world_y)) {
            continue;
        }
        if (auto* leaf = FindLeaf(root.get(), world_x, world_y)) {
            const std::size_t index = FindIndexById(leaf->zone_id);
            if (index < zones_.size()) {
                return index;
            }
        }
    }
    // Linear fallback: positions outside the forest (unmapped area) keep
    // the old semantics over simulating zones.
    for (std::size_t i = 0; i < zones_.size(); ++i) {
        if (zones_[i]->SimulationEnabled() && zones_[i]->Bounds().Contains(world_x, world_y)) {
            return i;
        }
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

bool ZoneManager::PlanSplit(ZoneId zone_id, SplitPlan& out_plan) const
{
    out_plan = SplitPlan{};
    const std::size_t zone_index = FindIndexById(zone_id);
    if (zone_index >= zones_.size()) {
        return false;
    }
    const Zone& zone = *zones_[zone_index];
    if (!zone.SimulationEnabled() || zone.Partition() != PartitionState::Leaf) {
        return false;
    }
    if (!zone.Commands().Empty()) {
        // Racing commands would strand in the frozen parent: retry next
        // cycle instead of stranding work.
        return false;
    }

    const RegionDefinition* region = nullptr;
    for (const auto& r : regions_) {
        if (r.id == zone.Region()) {
            region = &r;
            break;
        }
    }
    if (region == nullptr) {
        return false;
    }

    const ZonePartition* leaf = FindNodeInRoots(partition_roots_, zone_id);
    if (leaf == nullptr || !leaf->IsLeaf()) {
        return false;
    }
    if (leaf->depth >= region->max_partition_depth) {
        return false;
    }

    const mx::map::Rect bounds = zone.Bounds();
    const float half_w = (bounds.max_x - bounds.min_x) * 0.5f;
    const float half_h = (bounds.max_y - bounds.min_y) * 0.5f;
    if (half_w < region->min_zone_size || half_h < region->min_zone_size) {
        return false;
    }

    // Quadtree children tile the parent exactly (shared float midpoint, no
    // gaps by construction). Half-open ownership is enforced by FindLeaf.
    const float mid_x = bounds.min_x + half_w;
    const float mid_y = bounds.min_y + half_h;
    out_plan.parent_id = zone_id;
    out_plan.parent_index = zone_index;
    out_plan.region_id = zone.Region();
    out_plan.child_bounds[0] = {bounds.min_x, mid_y, mid_x, bounds.max_y}; // NW
    out_plan.child_bounds[1] = {mid_x, mid_y, bounds.max_x, bounds.max_y}; // NE
    out_plan.child_bounds[2] = {bounds.min_x, bounds.min_y, mid_x, mid_y}; // SW
    out_plan.child_bounds[3] = {mid_x, bounds.min_y, bounds.max_x, mid_y}; // SE
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
            zones_.push_back(std::move(child));
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
        child.SetSimulationEnabled(false);
        child.RefreshResidentCounts();
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
        zones_.push_back(std::move(merged));
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
        merged.SetSimulationEnabled(false);
        merged.RefreshResidentCounts();
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
    zone.SetSimulationEnabled(false);
    zone.SetPartition(PartitionState::Retired);
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

void ZoneManager::PostCommand(std::size_t zone_index, ZoneCommandQueue::Command command)
{
    if (zone_index >= zones_.size()) {
        return;
    }
    zones_[zone_index]->Commands().Push(std::move(command));
}

} // namespace gs::game
