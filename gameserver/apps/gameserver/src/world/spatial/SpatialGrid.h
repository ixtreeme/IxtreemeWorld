#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "SpatialTypes.h"
#include "../components/TransformComponents.h"

// Encapsulated uniform-grid spatial index over NetIds.
// Rebuilt from authoritative flecs state every tick (behavior parity with the
// old RebuildSpatialGrid; incremental updates are a documented TODO).
// Positions are NOT stored here; they are resolved from flecs at query time
// so the grid can never disagree with the authoritative state.
namespace gs::game {

class SpatialGrid {
public:
    void Clear();
    bool Empty() const noexcept;

    void Insert(std::uint32_t net_id, const Position& position);

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
    std::unordered_map<std::int64_t, std::vector<GridEntry>> cells_;
};

} // namespace gs::game
