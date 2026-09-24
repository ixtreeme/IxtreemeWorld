#include "TerrainService.h"

#include <stdexcept>

namespace gs::game {

TerrainService::TerrainService(mx::map::HeightField field)
    : field_(std::move(field))
{
    if (!field_.IsValid()) {
        throw std::invalid_argument("TerrainService: invalid height field (world package not validated?)");
    }
}

TerrainService::TerrainService(float flat_extent_meters)
    : flat_extent_m_(flat_extent_meters > 0.0f ? flat_extent_meters : 1000.0f)
{
}

float TerrainService::SampleGroundHeight(float world_x, float world_y) const noexcept
{
    if (!field_.IsValid()) {
        return 0.0f;
    }
    return field_.SampleHeightMeters(world_x, world_y);
}

bool TerrainService::IsWalkable(float world_x, float world_y) const noexcept
{
    if (!field_.IsValid()) {
        return true;
    }
    return field_.IsWalkable(world_x, world_y);
}

float TerrainService::WorldExtentMeters() const noexcept
{
    if (!field_.IsValid()) {
        return flat_extent_m_;
    }
    return static_cast<float>(field_.manifest.world_size_cells) * field_.manifest.cell_size_meters;
}

std::size_t TerrainService::ResidentBytes() const noexcept
{
    return field_.heights_cm.size() * sizeof(std::int16_t) + field_.attributes.size() * sizeof(std::uint16_t) +
           field_.splat_a_rgba8.size() + field_.splat_b_rgba8.size();
}

} // namespace gs::game
