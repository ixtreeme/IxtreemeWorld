#pragma once

#include <cstdint>
#include <vector>

#include "map/MapData.h"
#include "map/ServerTerrain.h"

// Server water (MAP-3): what the package DECLARES about water, and the
// minimal surface / depth / land-water query for that capability. Format
// contract: docs/map-data-format.md (manifest `water`, waterBodies layer).
//
// Undeclared water is UNKNOWN (UnsupportedLayer) -- never silently "land".
// Water data is small and always resident; only the ground height under a
// point can be missing (NotResident) while terrain streams.
namespace mx::map {

enum class WaterModel : std::uint8_t {
    Undeclared = 0, // no declaration: queries answer UnsupportedLayer
    None = 1,       // the world has no water
    SeaLevel = 2,   // one global surface at sea_level_m
    Bodies = 3,     // local axis-aligned bodies, each with its own surface
};
const char* ToString(WaterModel model) noexcept;

struct WaterBodyRect {
    std::uint32_t id = 0;
    Rect bounds;          // half-open, inside the world, disjoint
    float surface_m = 0.0f;
};

enum class WaterStatus : std::uint8_t {
    Water,            // surface above the ground: depth > 0
    NoWater,          // declared dry here (no body, or ground above the surface)
    OutsideWorld,
    NotResident,      // the ground height is not loaded yet
    UnsupportedLayer, // the package declares no water capability
    InvalidData,
};
const char* ToString(WaterStatus status) noexcept;

struct WaterSample {
    WaterStatus status = WaterStatus::UnsupportedLayer;
    float surface_m = 0.0f; // valid for Water, and for NoWater under a surface (dry ground)
    float depth_m = 0.0f;   // surface - ground (> 0 for Water)
    bool IsWater() const noexcept
    {
        return status == WaterStatus::Water;
    }
};

struct ServerWater {
    WaterModel model = WaterModel::Undeclared;
    double sea_level_m = 0.0;
    std::vector<WaterBodyRect> bodies;

    // `ground` = the terrain height at (x, y) (the caller's query).
    WaterSample Query(double x, double y, const GridGeometry& geometry, const HeightSample& ground) const noexcept;
};

} // namespace mx::map
