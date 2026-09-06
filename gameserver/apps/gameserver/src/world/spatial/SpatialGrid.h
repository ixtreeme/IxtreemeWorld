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

    void Insert(std::uint32_t net_id, const Position& position);
    void Remove(std::uint32_t net_id, const Position& position);

    // Moves the entry if the cells differ, inserts if it was missing
    // (self-healing), no-op otherwise. Returns true when the cell changed.
    bool Move(std::uint32_t net_id, std::int64_t old_cell_key, const Position& new_position);

    bool Contains(std::uint32_t net_id, const Position& position) const;

    // Debug/test support: every (net, cell) pair currently indexed.
    std::vector<std::pair<std::uint32_t, std::int64_t>> SnapshotEntries() const;

    // Calls visitor(net_id) for every entry in cells overlapping the
    // radius-disc around center. Distance filtering is the caller's job.
    template <typename Visitor>
    void ForEachInRadius(const Position& center, float radius, Visitor&& visitor) const
    {
        const int center_x = SpatialCellCoord(center.x);
        const int center_y = SpatialCellCoord(center.y);
        const int search_radius =
            static_cast<int>(std::ceil(radius / kSpatialCellSizeMeters));

        for (int y = center_y - search_radius; y <= center_y + search_radius; ++y) {
            for (int x = center_x - search_radius; x <= center_x + search_radius; ++x) {
                const auto cell_it = cells_.find(SpatialCellKey(x, y));
                if (cell_it == cells_.end()) {
                    continue;
                }
                for (const auto& entry : cell_it->second) {
                    visitor(entry.net_id);
                }
            }
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
};

} // namespace gs::game
