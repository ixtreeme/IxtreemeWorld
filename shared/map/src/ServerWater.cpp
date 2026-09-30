#include "map/ServerWater.h"

namespace mx::map {

const char* ToString(WaterModel model) noexcept
{
    switch (model) {
    case WaterModel::Undeclared:
        return "undeclared";
    case WaterModel::None:
        return "none";
    case WaterModel::SeaLevel:
        return "sea-level";
    case WaterModel::Bodies:
        return "bodies";
    }
    return "?";
}

const char* ToString(WaterStatus status) noexcept
{
    switch (status) {
    case WaterStatus::Water:
        return "water";
    case WaterStatus::NoWater:
        return "no-water";
    case WaterStatus::OutsideWorld:
        return "outside-world";
    case WaterStatus::NotResident:
        return "not-resident";
    case WaterStatus::UnsupportedLayer:
        return "unsupported-layer";
    case WaterStatus::InvalidData:
        return "invalid-data";
    }
    return "?";
}

WaterSample ServerWater::Query(double x, double y, const GridGeometry& geometry, const HeightSample& ground) const noexcept
{
    WaterSample out;
    if (!geometry.Contains(x, y)) {
        out.status = WaterStatus::OutsideWorld;
        return out;
    }
    double surface = 0.0;
    switch (model) {
    case WaterModel::Undeclared:
        out.status = WaterStatus::UnsupportedLayer;
        return out;
    case WaterModel::None:
        out.status = WaterStatus::NoWater;
        return out;
    case WaterModel::SeaLevel:
        surface = sea_level_m;
        break;
    case WaterModel::Bodies: {
        const WaterBodyRect* body = nullptr;
        for (const auto& candidate : bodies) {
            if (candidate.bounds.ContainsHalfOpen(static_cast<float>(x), static_cast<float>(y))) {
                body = &candidate;
                break;
            }
        }
        if (body == nullptr) {
            out.status = WaterStatus::NoWater;
            return out;
        }
        surface = body->surface_m;
        break;
    }
    }
    switch (ground.status) {
    case TerrainStatus::Ok:
        break;
    case TerrainStatus::NotResident:
        out.status = WaterStatus::NotResident;
        return out;
    case TerrainStatus::OutsideWorld:
        out.status = WaterStatus::OutsideWorld;
        return out;
    case TerrainStatus::InvalidData:
        out.status = WaterStatus::InvalidData;
        return out;
    }
    out.surface_m = static_cast<float>(surface);
    out.depth_m = static_cast<float>(surface - static_cast<double>(ground.meters));
    out.status = out.depth_m > 0.0f ? WaterStatus::Water : WaterStatus::NoWater;
    return out;
}

} // namespace mx::map
