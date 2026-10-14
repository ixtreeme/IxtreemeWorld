#pragma once

#include <cstddef>
#include <cstdint>

#include "map/LayeredWorld.h"

namespace mx::map {

// 3D-3 spatial identity. A horizontal cell is not globally unique once two
// navigable volumes may share its XY footprint; the volume id is therefore a
// mandatory part of the derived AOI/index key. The legacy key is represented
// by volume_id = kLegacyLayerId and remains distinct from every explicit
// volume.
struct LayeredSpatialCellKey {
    VolumeId volume_id = kLegacyLayerId;
    std::int32_t cell_x = 0;
    std::int32_t cell_y = 0;

    friend constexpr bool operator==(const LayeredSpatialCellKey& lhs,
                                     const LayeredSpatialCellKey& rhs) noexcept
    {
        return lhs.volume_id == rhs.volume_id && lhs.cell_x == rhs.cell_x && lhs.cell_y == rhs.cell_y;
    }
};

struct LayeredSpatialCellKeyHash {
    std::size_t operator()(const LayeredSpatialCellKey& key) const noexcept
    {
        std::uint64_t value = static_cast<std::uint64_t>(key.volume_id) * 0x9e3779b185ebca87ull;
        value ^= static_cast<std::uint32_t>(key.cell_x) + 0x9e3779b9ull + (value << 6) + (value >> 2);
        value ^= static_cast<std::uint32_t>(key.cell_y) + 0x9e3779b9ull + (value << 6) + (value >> 2);
        return static_cast<std::size_t>(value ^ (value >> 32));
    }
};

} // namespace mx::map
