#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "SpatialTypes.h"
#include "../components/TransformComponents.h"

// Encapsulated uniform-grid spatial index over NetIds, maintained
// INCREMENTALLY:
//   entity created  -> Insert
//   entity destroyed -> Remove
//   entity moves within its cell -> no-op
//   entity crosses into another cell -> Move(old, new)
//   entity changes zone -> source Remove + destination Insert
//
// Classification: DERIVED INDEX. Positions are NOT stored here (only the
// cell key is implied by bucket placement); authority stays in flecs and the
// grid is validated against it by SpatialValidator (debug/test).
namespace gs::game {

class SpatialGrid {
public:
    void Clear();
    bool Empty() const noexcept;
    std::size_t Size() const noexcept;

    void Insert(std::uint32_t net_id, const Position& position, flecs::entity entity);
    void Remove(std::uint32_t net_id, const Position& position);

    // Phase 5D: keeps the entry's stored position in sync in O(1) through the
    // entity's GridSlot component (in-cell move = one write; cell change =
    // swap-erase + insert). Returns true when the cell changed.
    bool Move(flecs::entity entity,
              std::uint32_t net_id,
              std::int64_t old_cell_key,
              const Position& new_position);

    bool Contains(std::uint32_t net_id, const Position& position) const;

    // Debug/test support: every (net, cell) pair currently indexed.
    std::vector<std::pair<std::uint32_t, std::int64_t>> SnapshotEntries() const;

    // Debug/diagnostic: the entry's stored position (false when absent).
    bool DebugStoredPosition(std::uint32_t net_id, float& x, float& y, float& z) const;

    // Validator support (phase 5D): visits every entry with its cell key and
    // slot index so the index audit can verify the stored position and the
    // entity's GridSlot bookkeeping.
    template <typename Visitor>
    void ForEachEntry(Visitor&& visitor) const
    {
        for (const auto& [key, cell] : cells_) {
            for (std::size_t i = 0; i < cell.size(); ++i) {
                visitor(cell[i], key, i);
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

    // Calls visitor(const GridEntry&) for every entry in cells overlapping
    // the radius-disc around center. Distance filtering is the caller's job.
    // `out_cells_visited` (optional) counts the non-empty cells examined --
    // the phase 5D AOI stage audit.
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
                for (const auto& entry : cell_it->second) {
                    visitor(entry);
                }
            }
        }
        if (out_cells_visited != nullptr) {
            *out_cells_visited = cells_visited;
        }
    }

private:
    static bool EraseOne(std::vector<GridEntry>& cell, std::uint32_t net_id)
    {
        const auto it =
            std::find_if(cell.begin(), cell.end(), [net_id](const GridEntry& entry) {
                return entry.net_id == net_id;
            });
        if (it == cell.end()) {
            return false;
        }
        cell.erase(it);
        return true;
    }

    std::unordered_map<std::int64_t, std::vector<GridEntry>> cells_;
    MaintenanceCounters maintenance_;
};

} // namespace gs::game
