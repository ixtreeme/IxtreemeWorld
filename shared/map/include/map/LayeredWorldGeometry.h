#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "map/LayeredWorldGenerator.h"

namespace mx::map {

// Collision geometry in canonical server metres: X/Y horizontal, Z up.
// Upward floors have counterclockwise winding when viewed from above. Engine
// adapters must apply world transforms and correct winding after axis swaps.
struct LayerCollisionMesh {
    std::uint32_t source_id = 0;
    std::string name;
    std::uint32_t tags = VolumeTagNone;
    bool supports_ground_movement = true;
    std::vector<std::array<float, 3>> vertices;
    std::vector<std::uint32_t> indices;
};

struct LayerGeometryOptions {
    float clearance_height = 2.0f;
    float foot_tolerance = 0.02f;
    float max_slope_degrees = 45.0f;
};

struct LayerGeometryReport {
    std::size_t meshes_seen = 0;
    std::size_t triangles_seen = 0;
    std::size_t triangles_walkable = 0;
    std::size_t surfaces_generated = 0;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;

    bool Ok() const noexcept { return errors.empty(); }
};

inline constexpr std::size_t kMaxLayerGeometryTriangles = 131072;
inline constexpr std::size_t kMaxLayerGeometryVertices = 262144;

// Extracts only connected coplanar upward collision faces with a proven
// axis-aligned rectangular horizontal footprint. Walls/downward/steep faces
// are skipped; holes, concave outlines and unsupported geometry fail closed.
// Exact duplicate coordinates are welded (including split material vertices).
// This generates height bands, not collision shapes or inferred portals.
// Output is stable under mesh/triangle/vertex reorder and empty on failure.
bool ExtractLayerSourceSurfaces(const std::vector<LayerCollisionMesh>& meshes,
                                const LayerGeometryOptions& options,
                                std::vector<LayerSourceSurface>& surfaces,
                                LayerGeometryReport& report);

} // namespace mx::map
