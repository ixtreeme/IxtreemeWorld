#include "ZoneManager.h"

#include <algorithm>

#include "common/Logging.h"

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

bool ZoneManager::SplitZone(ZoneId zone_id, std::vector<ZoneId>& out_new_zone_ids)
{
    out_new_zone_ids.clear();
    const std::size_t zone_index = FindIndexById(zone_id);
    if (zone_index >= zones_.size()) {
        return false;
    }
    Zone& zone = *zones_[zone_index];
    if (!zone.SimulationEnabled() || zone.Partition() != PartitionState::Leaf) {
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

    ZonePartition* leaf = FindNodeInRoots(partition_roots_, zone_id);
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
    const mx::map::Rect child_bounds[4] = {
        {bounds.min_x, mid_y, mid_x, bounds.max_y}, // NW
        {mid_x, mid_y, bounds.max_x, bounds.max_y}, // NE
        {bounds.min_x, bounds.min_y, mid_x, mid_y}, // SW
        {mid_x, bounds.min_y, bounds.max_x, mid_y}, // SE
    };
    const char* child_suffix[4] = {"_nw", "_ne", "_sw", "_se"};

    leaf->state = PartitionState::SplitPending;
    zone.SetPartition(PartitionState::SplitPending);
    zone.SetSimulationEnabled(false);

    const auto now = std::chrono::steady_clock::now();
    for (int i = 0; i < 4; ++i) {
        const ZoneId child_id = AllocateZoneId();
        auto child = std::make_unique<Zone>(child_id, zone.Name() + child_suffix[i], child_bounds[i]);
        child->SetRegion(zone.Region());
        child->SetSimulationEnabled(true);
        child->NextTick() = now;
        zones_.push_back(std::move(child));

        auto node = std::make_unique<ZonePartition>(child_id, zone.Region(), child_bounds[i],
                                                    static_cast<std::uint8_t>(leaf->depth + 1));
        node->parent = leaf;
        leaf->children.push_back(std::move(node));
        out_new_zone_ids.push_back(child_id);
    }
    leaf->last_split_time = std::chrono::steady_clock::now();

    graph_.Rebuild(zones_);
    return true;
}

bool ZoneManager::MergeZones(const std::vector<ZoneId>& zone_ids, ZoneId& out_merged_zone_id)
{
    if (zone_ids.size() < 2) {
        return false;
    }

    // All ids must be leaves under ONE common parent (sibling set).
    ZonePartition* parent = nullptr;
    for (ZoneId id : zone_ids) {
        ZonePartition* node = FindNodeInRoots(partition_roots_, id);
        if (node == nullptr || !node->IsLeaf() || node->parent == nullptr) {
            return false;
        }
        if (parent == nullptr) {
            parent = node->parent;
        } else if (parent != node->parent) {
            return false;
        }
        const std::size_t index = FindIndexById(id);
        if (index >= zones_.size()) {
            return false;
        }
        const Zone& zone = *zones_[index];
        if (!zone.SimulationEnabled() || zone.Partition() != PartitionState::Leaf) {
            return false;
        }
    }
    // Exact sibling set: no partial collapse (keeps tiling exact).
    if (parent->children.size() != zone_ids.size()) {
        return false;
    }

    const ZoneId merged_id = AllocateZoneId();
    auto merged = std::make_unique<Zone>(merged_id, "merged", parent->bounds);
    merged->SetRegion(parent->region_id);
    merged->SetSimulationEnabled(true);
    merged->NextTick() = std::chrono::steady_clock::now();
    zones_.push_back(std::move(merged));
    out_merged_zone_id = merged_id;

    // Collapse the parent back to a leaf carrying the merged zone. Child
    // metadata is dropped; the child Zone OBJECTS stay in their slots and
    // are retired by the caller after the entity transfer.
    parent->children.clear();
    parent->zone_id = merged_id;
    parent->state = PartitionState::Leaf;
    parent->simulation_enabled = true;
    parent->last_merge_time = std::chrono::steady_clock::now();

    graph_.Rebuild(zones_);
    return true;
}

void ZoneManager::RetireZone(ZoneId zone_id)
{
    const std::size_t index = FindIndexById(zone_id);
    if (index < zones_.size()) {
        zones_[index]->SetSimulationEnabled(false);
        zones_[index]->SetPartition(PartitionState::Retired);
    }
    if (auto* node = FindNodeInRoots(partition_roots_, zone_id)) {
        node->state = PartitionState::Retired;
        node->simulation_enabled = false;
    }
    graph_.Rebuild(zones_);
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
