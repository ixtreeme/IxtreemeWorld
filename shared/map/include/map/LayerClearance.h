#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "map/LayeredWorld.h"

namespace mx::map {

// 3D-4B: static obstruction geometry in canonical server metres (X/Y
// horizontal, Z up). Every static collider of the authored scene belongs
// here, whether or not it produced a layer volume: walls, ceilings, beams,
// props, the undersides of floors. Winding is irrelevant (two-sided test).
struct LayerObstructionMesh {
    std::uint32_t source_id = 0;
    std::vector<std::array<float, 3>> vertices;
    std::vector<std::uint32_t> indices;
};

// Terrain heights as corner samples, row-major with y outer, (cells_x + 1) *
// (cells_y + 1) values in metres, sample (i, j) at origin + (i, j) * cell.
// Both triangulations of every quad are tested, so the result is
// conservative for either physical diagonal.
struct LayerTerrainObstruction {
    double origin_x = 0.0;
    double origin_y = 0.0;
    double cell_size = 1.0;
    std::uint32_t cells_x = 0;
    std::uint32_t cells_y = 0;
    std::vector<float> heights;
};

struct LayerClearanceReport {
    std::size_t volumes_with_clearance = 0;
    std::uint64_t cells_total = 0;
    std::uint64_t cells_blocked = 0;
    std::size_t obstruction_triangles = 0;
    std::uint64_t terrain_quads_tested = 0;
    std::size_t portals_derived = 0;
    std::size_t edges_rejected_step = 0;
    std::uint64_t corridor_slots = 0;
    std::uint64_t corridor_slots_blocked = 0;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;

    bool Ok() const noexcept { return errors.empty(); }
};

inline constexpr std::size_t kMaxLayerObstructionTriangles = 1u << 20;

// Cooks a clearance grid for every walkable volume with a valid ground
// support, and derives proven step/ramp portals between exactly touching
// support rectangles. `world` must already validate against `world_bounds`;
// previous clearance data and derived portals are replaced, authored portals
// are kept unproven. On failure `world` is left without any 3D-4B data.
//
// Soundness contract (see LayerClearanceGrid / LayerPortalProof): a cell is
// blocked when any obstruction triangle intersects the conservative region
//   { (q, z) : q in cell expanded by the radius (square),
//              plane(q) + floor_contact <= z <= max plane over cell + height
//              + radius * (1/cos(slope) - 1) }.
// The capsule standing on the plane is contained in that region.
bool CookLayerClearance(LayeredWorld& world,
                        const Rect& world_bounds,
                        const std::vector<LayerObstructionMesh>& obstructions,
                        const LayerTerrainObstruction* terrain,
                        const LayerClearanceProfile& profile,
                        LayerClearanceReport& report);

} // namespace mx::map
