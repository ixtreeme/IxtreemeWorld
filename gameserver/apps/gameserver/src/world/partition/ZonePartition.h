#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

#include "map/MapData.h"

#include "PartitionTypes.h"

// Dynamic simulation-ownership tree. ONE canonical header: it depends only
// on map/MapData.h + PartitionTypes.h, never on Zone.h, so zone code can
// include it without a cycle. A region owns a forest of ZonePartition
// roots; only IsActiveLeaf() nodes may hold authoritative entities.
namespace gs::game {

struct ZonePartition {
    ZoneId zone_id = 0;
    RegionId region_id = 0;
    PartitionState state = PartitionState::Leaf;
    mx::map::Rect bounds;
    std::uint8_t depth = 0;

    ZonePartition* parent = nullptr;
    std::vector<std::unique_ptr<ZonePartition>> children;

    // Load-control bookkeeping (written by ZoneLoadMonitor, read by the
    // scheduler predicates). Plain data; supervisor thread only.
    float load_score = 0.0f;
    std::chrono::steady_clock::time_point sustained_breach_since{};
    std::chrono::steady_clock::time_point last_split_time{};
    std::chrono::steady_clock::time_point last_merge_time{};
    bool simulation_enabled = true;

    ZonePartition() = default;
    ZonePartition(ZoneId id, RegionId region, const mx::map::Rect& b, std::uint8_t d)
        : zone_id(id)
        , region_id(region)
        , bounds(b)
        , depth(d)
    {
    }

    bool IsLeaf() const noexcept
    {
        return children.empty();
    }

    bool IsActiveLeaf() const noexcept
    {
        return IsLeaf() && state == PartitionState::Leaf && simulation_enabled;
    }
};

// Tree descent with half-open bounds [min, max): a point on a shared edge
// belongs to exactly one child (strict < goes west/south), so siblings
// never overlap and never gap. O(depth). Returns null when no ACTIVE leaf
// covers the point (caller falls back as appropriate).
inline ZonePartition* FindLeaf(ZonePartition* root, float world_x, float world_y)
{
    auto contains = [](const mx::map::Rect& r, float x, float y) {
        return x >= r.min_x && x < r.max_x && y >= r.min_y && y < r.max_y;
    };

    ZonePartition* current = root;
    while (current != nullptr && !current->IsLeaf()) {
        ZonePartition* next = nullptr;
        for (auto& child : current->children) {
            if (contains(child->bounds, world_x, world_y)) {
                next = child.get();
                break;
            }
        }
        if (next == nullptr) {
            break;
        }
        current = next;
    }
    return (current != nullptr && current->IsActiveLeaf()) ? current : nullptr;
}

inline void CollectActiveLeaves(ZonePartition* root, std::vector<ZonePartition*>& out)
{
    if (root == nullptr) {
        return;
    }
    if (root->IsActiveLeaf()) {
        out.push_back(root);
        return;
    }
    for (auto& child : root->children) {
        CollectActiveLeaves(child.get(), out);
    }
}

// Depth-first search for the metadata node carrying a ZoneId (leaf or
// internal). Linear in tree size; used on the control plane only.
inline ZonePartition* FindPartitionNode(ZonePartition* root, ZoneId zone_id)
{
    if (root == nullptr) {
        return nullptr;
    }
    if (root->zone_id == zone_id) {
        return root;
    }
    for (auto& child : root->children) {
        if (auto* found = FindPartitionNode(child.get(), zone_id)) {
            return found;
        }
    }
    return nullptr;
}

} // namespace gs::game
