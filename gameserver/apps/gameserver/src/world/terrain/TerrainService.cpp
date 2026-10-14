#include "TerrainService.h"

#include "TerrainStreamer.h"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace gs::game {

TerrainService::TerrainService(mx::map::ServerTerrain terrain)
    : terrain_(std::move(terrain))
{
    if (terrain_.Empty()) {
        throw std::invalid_argument("TerrainService: empty terrain (world package not loaded?)");
    }
    bounds_ = BoundsOf(terrain_);
}

WorldBounds TerrainService::BoundsOf(const mx::map::ServerTerrain& terrain) noexcept
{
    // Runtime positions are f32: round the bounds INWARD so every f32 point
    // the service calls inside is inside the (double) terrain geometry too.
    const auto& g = terrain.Geometry();
    auto inward_min = [](double v) {
        float f = static_cast<float>(v);
        return static_cast<double>(f) < v ? std::nextafter(f, std::numeric_limits<float>::infinity()) : f;
    };
    auto inward_max = [](double v) {
        float f = static_cast<float>(v);
        return static_cast<double>(f) > v ? std::nextafter(f, -std::numeric_limits<float>::infinity()) : f;
    };
    return WorldBounds{inward_min(g.MinX()), inward_min(g.MinY()), inward_max(g.MaxX()), inward_max(g.MaxY())};
}

TerrainService TerrainService::Flat(const WorldBounds& bounds)
{
    if (!bounds.IsValid()) {
        throw std::invalid_argument("TerrainService::Flat: invalid bounds");
    }
    TerrainService service;
    service.bounds_ = bounds;
    service.flat_ = true;
    return service;
}

bool TerrainService::Contains(float world_x, float world_y) const noexcept
{
    // Half-open [min, max); NaN compares false and is outside.
    return world_x >= bounds_.min_x && world_x < bounds_.max_x && world_y >= bounds_.min_y &&
           world_y < bounds_.max_y;
}

mx::map::HeightSample TerrainService::Height(float world_x, float world_y) const noexcept
{
    if (flat_) {
        return Contains(world_x, world_y) ? mx::map::HeightSample{mx::map::TerrainStatus::Ok, 0.0f}
                                          : mx::map::HeightSample{mx::map::TerrainStatus::OutsideWorld, 0.0f};
    }
    if (!Contains(world_x, world_y)) {
        return {mx::map::TerrainStatus::OutsideWorld, 0.0f};
    }
    return terrain_.Height(world_x, world_y);
}

mx::map::CellSample TerrainService::Cell(float world_x, float world_y) const noexcept
{
    if (flat_) {
        return Contains(world_x, world_y) ? mx::map::CellSample{mx::map::TerrainStatus::Ok, 0}
                                          : mx::map::CellSample{mx::map::TerrainStatus::OutsideWorld, 0};
    }
    if (!Contains(world_x, world_y)) {
        return {mx::map::TerrainStatus::OutsideWorld, 0};
    }
    return terrain_.Cell(world_x, world_y);
}

std::size_t TerrainService::ResidentBytes() const noexcept
{
    return flat_ ? 0 : terrain_.ResidentBytes();
}

const char* ToString(StepResult result) noexcept
{
    switch (result) {
    case StepResult::Clear:
        return "clear";
    case StepResult::Blocked:
        return "blocked";
    case StepResult::TooSteep:
        return "too-steep";
    case StepResult::DeepWater:
        return "deep-water";
    case StepResult::OutsideWorld:
        return "outside-world";
    case StepResult::NotResident:
        return "not-resident";
    case StepResult::InvalidData:
        return "invalid-data";
    }
    return "?";
}

mx::map::WaterSample TerrainService::Water(float world_x, float world_y) const noexcept
{
    mx::map::WaterSample out;
    if (!Contains(world_x, world_y)) {
        out.status = mx::map::WaterStatus::OutsideWorld;
        return out;
    }
    if (flat_ || terrain_.Empty()) {
        out.status = mx::map::WaterStatus::UnsupportedLayer; // synthetic worlds declare no water
        return out;
    }
    return water_.Query(world_x, world_y, terrain_.Geometry(), terrain_.Height(world_x, world_y));
}

StepCheck TerrainService::CheckStep(float from_x, float from_y, float to_x, float to_y) const noexcept
{
    StepCheck out;
    if (!Contains(to_x, to_y)) {
        out.result = StepResult::OutsideWorld;
        return out;
    }
    if (flat_) {
        return out;
    }
    auto path = terrain_.Segment(from_x, from_y, to_x, to_y);
    if (path.status == mx::map::TerrainStatus::Ok && path.cells == 0) {
        // The step stays inside the start cell, which the path check skips
        // (an entity may always leave the cell it stands in). The step still
        // needs that cell's DATA: its height becomes the entity's z. Without
        // data (chunk evicted while the entity was dormant / its zone slept,
        // or published invalid) the step waits or is refused -- never taken
        // with a stale z.
        const auto here = terrain_.Cell(to_x, to_y);
        if (!here.Ok()) {
            path.status = here.status;
            path.chunk = terrain_.ChunkIndexOf(to_x, to_y);
        }
    }
    switch (path.status) {
    case mx::map::TerrainStatus::Ok:
        break;
    case mx::map::TerrainStatus::OutsideWorld:
        out.result = StepResult::OutsideWorld;
        return out;
    case mx::map::TerrainStatus::NotResident:
        out.result = StepResult::NotResident;
        out.chunk = path.chunk;
        return out;
    case mx::map::TerrainStatus::InvalidData:
        out.result = StepResult::InvalidData;
        return out;
    }
    if (path.blocked) {
        out.result = StepResult::Blocked;
        return out;
    }
    if (rules_.max_slope > 0.0f) {
        const auto h0 = terrain_.Height(from_x, from_y);
        const auto h1 = terrain_.Height(to_x, to_y);
        const float dist = std::hypot(to_x - from_x, to_y - from_y);
        if (h0.Ok() && h1.Ok() && dist > 1e-4f && (h1.meters - h0.meters) / dist > rules_.max_slope) {
            out.result = StepResult::TooSteep;
            return out;
        }
    }
    if (rules_.max_water_depth_m >= 0.0f) {
        const auto water = Water(to_x, to_y);
        if (water.status == mx::map::WaterStatus::NotResident) {
            out.result = StepResult::NotResident;
            out.chunk = terrain_.ChunkIndexOf(to_x, to_y);
            return out;
        }
        if (water.IsWater() && water.depth_m > rules_.max_water_depth_m) {
            out.result = StepResult::DeepWater;
            return out;
        }
    }
    return out;
}

float TerrainService::LookaheadSeconds() const noexcept
{
    return streamer_ != nullptr ? streamer_->LookaheadSeconds() : 0.0f;
}

} // namespace gs::game
