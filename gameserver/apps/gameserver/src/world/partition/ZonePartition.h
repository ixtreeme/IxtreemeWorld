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
    float load_score = 0.0f;        // combined gate score (legacy + field)
    float legacy_load_score = 0.0f; // avg/p99 tick + resident pressure
    float field_load_score = 0.0f;  // load field mean/peak composite (fast)
    float field_peak_score = 0.0f;  // max cell composite in the zone
    float p95_tick_us = 0.0f;
    float p99_tick_us = 0.0f;
    std::chrono::steady_clock::time_point sustained_breach_since{};
    // Leaf-level sustained-low state: how long this leaf's combined score has
    // stayed below the merge threshold (written by ZoneLoadMonitor).
    std::chrono::steady_clock::time_point field_low_since{};
    // Sibling-GROUP sustained-low state (internal nodes only): how long the
    // whole group -- every child AND the parent-area aggregate on both the
    // fast and slow field timescales -- has stayed below the merge threshold.
    // One low child is never enough (phase-3 §8): the timer resets when any
    // child or the aggregate rises. Written by ZoneLoadMonitor, read by
    // ZoneScheduler::EvaluateMergeGate.
    std::chrono::steady_clock::time_point group_low_since{};
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

// The cut point of a split (world coordinates). The transactional executor
// tiles its four children from this point; the adaptive scorer produces it.
struct SplitCenter {
    float x = 0.0f;
    float y = 0.0f;
};

// Structured reason a split plan was refused (why-not diagnostics, §31).
enum class SplitRejectReason : std::uint8_t {
    None = 0,
    UnknownZone,
    NotSimulating,
    CommandsPending,
    NoRegion,
    NotLeaf,
    MaxDepth,
    TooSmall,
};

inline const char* SplitRejectReasonName(SplitRejectReason reason) noexcept
{
    switch (reason) {
    case SplitRejectReason::UnknownZone:
        return "unknown-zone";
    case SplitRejectReason::NotSimulating:
        return "not-simulating";
    case SplitRejectReason::CommandsPending:
        return "commands-pending";
    case SplitRejectReason::NoRegion:
        return "no-region";
    case SplitRejectReason::NotLeaf:
        return "not-leaf";
    case SplitRejectReason::MaxDepth:
        return "max-depth";
    case SplitRejectReason::TooSmall:
        return "min-size";
    case SplitRejectReason::None:
    default:
        return "none";
    }
}

// The canonical quadtree tiling shared by the transactional split executor
// and the adaptive scorer: four children from one center point, half-open
// ownership, order NW, NE, SW, SE (matches ZoneManager::SplitPlan and the
// commit-time child locations). One geometry, one source of truth.
inline void BuildQuadtreeChildBounds(const mx::map::Rect& parent,
                                     float center_x,
                                     float center_y,
                                     mx::map::Rect out_child_bounds[4]) noexcept
{
    out_child_bounds[0] = {parent.min_x, center_y, center_x, parent.max_y}; // NW
    out_child_bounds[1] = {center_x, center_y, parent.max_x, parent.max_y}; // NE
    out_child_bounds[2] = {parent.min_x, parent.min_y, center_x, center_y}; // SW
    out_child_bounds[3] = {center_x, parent.min_y, parent.max_x, center_y}; // SE
}

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

// Deterministic depth-first enumeration of every INTERNAL node (potential
// merge parent). Order is tree/child-vector order -- never hash order -- so
// the merge candidate list is reproducible for identical trees.
inline void CollectInternalNodes(ZonePartition* root, std::vector<ZonePartition*>& out)
{
    if (root == nullptr || root->IsLeaf()) {
        return;
    }
    out.push_back(root);
    for (auto& child : root->children) {
        CollectInternalNodes(child.get(), out);
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
