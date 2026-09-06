#pragma once

#include <cmath>
#include <cstdint>

#include "../WorldConstants.h"
#include "../components/TransformComponents.h"
#include "map/MapData.h"

// Pure geometry helpers shared by the zone graph, migration, visibility and
// spatial grid. Free functions on concrete types; no service, no state.
namespace gs::game {

struct GridEntry {
    std::uint32_t net_id = 0;
};

inline int SpatialCellCoord(float value)
{
    return static_cast<int>(std::floor(value / kSpatialCellSizeMeters));
}

inline std::int64_t SpatialCellKey(int x, int y)
{
    const auto packed = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) |
                        static_cast<std::uint32_t>(y);
    return static_cast<std::int64_t>(packed);
}

inline bool RectsTouchOrOverlap(const mx::map::Rect& lhs, const mx::map::Rect& rhs)
{
    constexpr float kEpsilon = 0.001f;
    return lhs.min_x <= rhs.max_x + kEpsilon && lhs.max_x + kEpsilon >= rhs.min_x &&
           lhs.min_y <= rhs.max_y + kEpsilon && lhs.max_y + kEpsilon >= rhs.min_y;
}

inline float DistanceOutsideRect(const mx::map::Rect& rect, const Position& position)
{
    float distance = 0.0f;
    if (position.x < rect.min_x) {
        distance = distance > rect.min_x - position.x ? distance : rect.min_x - position.x;
    } else if (position.x > rect.max_x) {
        distance = distance > position.x - rect.max_x ? distance : position.x - rect.max_x;
    }

    if (position.y < rect.min_y) {
        distance = distance > rect.min_y - position.y ? distance : rect.min_y - position.y;
    } else if (position.y > rect.max_y) {
        distance = distance > position.y - rect.max_y ? distance : position.y - rect.max_y;
    }
    return distance;
}

inline bool IsInBorderBand(const mx::map::Rect& bounds, const Position& position, float band)
{
    return position.x - bounds.min_x <= band || bounds.max_x - position.x <= band ||
           position.y - bounds.min_y <= band || bounds.max_y - position.y <= band;
}

} // namespace gs::game
