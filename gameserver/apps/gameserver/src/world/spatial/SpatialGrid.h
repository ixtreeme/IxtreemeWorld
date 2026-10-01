#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "SpatialTypes.h"
#include "../components/TransformComponents.h"
#include "map/LayeredSpatialKey.h"

// Encapsulated uniform-grid spatial index over NetIds, maintained
// INCREMENTALLY:
//   entity created  -> Insert
//   entity destroyed -> Remove
//   entity moves within its cell -> no-op
//   entity crosses into another cell -> Move(old, new)
//   entity changes zone -> source Remove + destination Insert
//
// 3D-5A: the bucket identity is the layered key (volume, cell_x, cell_y)
// (mx::map::LayeredSpatialCellKey); volume 0 is the legacy terrain volume.
// Physically an XY column holds one bucket per volume present in it, so
// stacked floors never share a bucket while a radius scan still reads the
// whole column with one hash lookup. Insert/Move take the entity's volume
// explicitly (there is no default); Remove/Contains search the XY column.
//
// Classification: DERIVED INDEX. Authority stays in flecs and the grid is
// validated against it by SpatialValidator (debug/test).
namespace gs::game {

class SpatialGrid {
public:
    void Clear();
    bool Empty() const noexcept;
    std::size_t Size() const noexcept;

    void Insert(std::uint32_t net_id, const Position& position, flecs::entity entity,
                std::uint32_t volume_id);
    void Remove(std::uint32_t net_id, const Position& position);

    // Phase 5D: keeps the entry's stored position in sync in O(1) through the
    // entity's GridSlot component (in-bucket move = one write; cell or volume
    // change = swap-erase + insert). Returns true when the bucket changed.
    bool Move(flecs::entity entity,
              std::uint32_t net_id,
              std::int64_t old_cell_key,
              const Position& new_position,
              std::uint32_t new_volume_id);

    // Any volume of the XY cell derived from `position`.
    bool Contains(std::uint32_t net_id, const Position& position) const;
    // Exactly the (volume, cell) bucket derived from position + volume.
    bool ContainsIn(std::uint32_t net_id, const Position& position, std::uint32_t volume_id) const;

    // Debug/test support: every (net, layered bucket key) pair indexed.
    std::vector<std::pair<std::uint32_t, mx::map::LayeredSpatialCellKey>> SnapshotEntries() const;

    // Debug/diagnostic: the entry's stored position (false when absent).
    bool DebugStoredPosition(std::uint32_t net_id, float& x, float& y, float& z) const;

    // Validator support (phase 5D): visits every entry with its XY cell key
    // and slot index inside its volume bucket (the entry carries the volume).
    template <typename Visitor>
    void ForEachEntry(Visitor&& visitor) const
    {
        for (const auto& [key, column] : cells_) {
            for (const auto& bucket : column) {
                for (std::size_t i = 0; i < bucket.entries.size(); ++i) {
                    visitor(bucket.entries[i], key, i);
                }
            }
        }
    }

    // Phase 5D maintenance audit: cumulative operation counts (the zone
    // snapshots deltas into its diagnostics once per tick).
    struct MaintenanceCounters {
        std::uint64_t inserts = 0;
        std::uint64_t removes = 0;
        std::uint64_t moves_in_cell = 0;
        std::uint64_t moves_cell = 0;
    };
    const MaintenanceCounters& Maintenance() const noexcept
    {
        return maintenance_;
    }

    // Calls visitor(const GridEntry&) for every entry of every volume in
    // cells overlapping the radius-disc around center. Distance filtering is
    // the caller's job. `out_cells_visited` (optional) counts the non-empty
    // XY cells examined -- the phase 5D AOI stage audit.
    template <typename Visitor>
    void ForEachInRadius(const Position& center,
                         float radius,
                         Visitor&& visitor,
                         std::uint64_t* out_cells_visited = nullptr) const
    {
        const int center_x = SpatialCellCoord(center.x);
        const int center_y = SpatialCellCoord(center.y);
        const int search_radius =
            static_cast<int>(std::ceil(radius / kSpatialCellSizeMeters));

        std::uint64_t cells_visited = 0;
        for (int y = center_y - search_radius; y <= center_y + search_radius; ++y) {
            for (int x = center_x - search_radius; x <= center_x + search_radius; ++x) {
                const auto cell_it = cells_.find(SpatialCellKey(x, y));
                if (cell_it == cells_.end()) {
                    continue;
                }
                ++cells_visited;
                for (const auto& bucket : cell_it->second) {
                    for (const auto& entry : bucket.entries) {
                        visitor(entry);
                    }
                }
            }
        }
        if (out_cells_visited != nullptr) {
            *out_cells_visited = cells_visited;
        }
    }

private:
    struct Bucket {
        std::uint32_t volume_id = 0;
        std::vector<GridEntry> entries;
    };
    using Column = std::vector<Bucket>;

    static Bucket* FindBucket(Column& column, std::uint32_t volume_id) noexcept
    {
        for (auto& bucket : column) {
            if (bucket.volume_id == volume_id) {
                return &bucket;
            }
        }
        return nullptr;
    }
    // Swap-erases entry `index` of `bucket` (fixing the moved entry's slot)
    // and drops the bucket / column when they become empty. `column_it`
    // is invalidated when the column is erased.
    void EraseAt(std::unordered_map<std::int64_t, Column>::iterator column_it,
                 std::size_t bucket_index,
                 std::size_t index);
    void Append(std::int64_t cell_key, std::uint32_t volume_id, const GridEntry& entry);

    std::unordered_map<std::int64_t, Column> cells_;
    MaintenanceCounters maintenance_;
};

} // namespace gs::game
