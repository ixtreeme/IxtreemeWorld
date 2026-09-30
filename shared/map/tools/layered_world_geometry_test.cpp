#include "map/LayeredWorldGeometry.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>

namespace {

using namespace mx::map;
int failures = 0;

void Check(const char* name, bool condition)
{
    std::cout << "LAYERED3D GEOMETRY " << name << ": " << (condition ? "PASS" : "FAIL") << '\n';
    if (!condition) ++failures;
}

void Quad(LayerCollisionMesh& mesh, float x, float y, float width, float depth, float z)
{
    const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
    mesh.vertices.insert(mesh.vertices.end(), {{x, y, z}, {x + width, y, z},
                                              {x + width, y + depth, z}, {x, y + depth, z}});
    for (const auto index : {0u, 1u, 2u, 0u, 2u, 3u}) mesh.indices.push_back(base + index);
}

LayerCollisionMesh Floor(std::uint32_t id, float z, std::uint32_t tags = VolumeTagGround)
{
    LayerCollisionMesh mesh;
    mesh.source_id = id;
    mesh.name = "floor_" + std::to_string(id);
    mesh.tags = tags;
    Quad(mesh, 0, 0, 4, 4, z);
    return mesh;
}

bool Same(const std::vector<LayerSourceSurface>& lhs, const std::vector<LayerSourceSurface>& rhs)
{
    if (lhs.size() != rhs.size()) return false;
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        const auto& a = lhs[index];
        const auto& b = rhs[index];
        if (a.source_id != b.source_id || a.name != b.name || a.tags != b.tags ||
            a.supports_ground_movement != b.supports_ground_movement || a.min_z != b.min_z || a.max_z != b.max_z ||
            a.bounds.min_x != b.bounds.min_x || a.bounds.min_y != b.bounds.min_y ||
            a.bounds.max_x != b.bounds.max_x || a.bounds.max_y != b.bounds.max_y) return false;
    }
    return true;
}

} // namespace

int main()
{
    using namespace mx::map;
    const LayerGeometryOptions options;
    LayerGeometryReport report;
    std::vector<LayerSourceSurface> surfaces;
    std::vector<LayerCollisionMesh> stack{Floor(20, 4, VolumeTagBridge | VolumeTagConnector), Floor(10, 0)};
    Check("stacked-floors-extracted", ExtractLayerSourceSurfaces(stack, options, surfaces, report) &&
                                          report.Ok() && surfaces.size() == 2 && report.triangles_seen == 4 &&
                                          report.triangles_walkable == 4 && report.surfaces_generated == 2);
    Check("height-derived-from-floor", surfaces.size() == 2 && surfaces[0].min_z == -0.02f &&
                                          surfaces[0].max_z == 2 && surfaces[1].min_z == 3.98f && surfaces[1].max_z == 6);
    Check("semantic-tags-preserved", surfaces.size() == 2 &&
                                         surfaces[1].tags == (VolumeTagBridge | VolumeTagConnector));
    const auto reference = surfaces;
    std::reverse(stack.begin(), stack.end());
    // Permute vertices, triangle order and cyclic corner order independently.
    for (auto& mesh : stack) {
        std::reverse(mesh.vertices.begin(), mesh.vertices.end());
        for (auto& index : mesh.indices) index = static_cast<std::uint32_t>(mesh.vertices.size() - 1) - index;
        for (std::size_t index = 0; index < mesh.indices.size(); index += 3) {
            std::rotate(mesh.indices.begin() + index, mesh.indices.begin() + index + 1, mesh.indices.begin() + index + 3);
        }
        std::swap_ranges(mesh.indices.begin(), mesh.indices.begin() + 3, mesh.indices.begin() + 3);
    }
    Check("input-reorder-deterministic", ExtractLayerSourceSurfaces(stack, options, surfaces, report) && Same(reference, surfaces));
    Check("repeat-deterministic", ExtractLayerSourceSurfaces(stack, options, surfaces, report) && Same(reference, surfaces));
    LayeredWorld world;
    LayerGenerationReport generation;
    LayerGenerationOptions generation_options{Rect{-16, -16, 16, 16}};
    generation_options.require_exact_footprints = true;
    Check("shared-generator-accepts-stack", GenerateLayeredWorld(surfaces, generation_options, world, generation) &&
                                                world.volumes.size() == 2 && world.portals.empty());
    Check("height-selects-distinct-layers", world.FindVolume(2, 2, 0) && world.FindVolume(2, 2, 4) &&
                                                world.FindVolume(2, 2, 0) != world.FindVolume(2, 2, 4) &&
                                                !world.FindVolume(2, 2, 3));

    auto split = Floor(1, 0);
    split.vertices = {{0, 0, 0}, {4, 0, 0}, {4, 4, 0}, {0, 0, 0}, {4, 4, 0}, {0, 4, 0}};
    split.indices = {0, 1, 2, 3, 4, 5};
    Check("duplicate-gltf-vertices-welded", ExtractLayerSourceSurfaces({split}, options, surfaces, report) && surfaces.size() == 1);
    auto box = Floor(2, 0);
    box.vertices = {{0, 0, 0}, {4, 0, 0}, {4, 4, 0}, {0, 4, 0},
                    {0, 0, 3}, {4, 0, 3}, {4, 4, 3}, {0, 4, 3}};
    box.indices = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7,
                   0, 1, 5, 0, 5, 4, 1, 2, 6, 1, 6, 5,
                   2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7};
    Check("box-only-top-is-floor", ExtractLayerSourceSurfaces({box}, options, surfaces, report) &&
                                      surfaces.size() == 1 && report.triangles_seen == 12 && report.triangles_walkable == 2 &&
                                      surfaces[0].min_z == 2.98f && surfaces[0].max_z == 5);
    auto wall = Floor(30, 0);
    wall.vertices = {{0, 0, 0}, {0, 4, 0}, {0, 4, 3}, {0, 0, 3}};
    Check("wall-skipped-with-floor", ExtractLayerSourceSurfaces({wall, Floor(2, 0)}, options, surfaces, report) &&
                                          surfaces.size() == 1 && !report.warnings.empty());
    Check("walls-alone-not-floor", !ExtractLayerSourceSurfaces({wall}, options, surfaces, report) && surfaces.empty());
    auto underside = Floor(4, 0);
    for (std::size_t index = 0; index < underside.indices.size(); index += 3) {
        std::swap(underside.indices[index + 1], underside.indices[index + 2]);
    }
    Check("downward-alone-not-floor", !ExtractLayerSourceSurfaces({underside}, options, surfaces, report));
    auto ramp = Floor(5, 0);
    for (auto& position : ramp.vertices) position[2] = position[0] / 4;
    Check("planar-ramp-height-band", ExtractLayerSourceSurfaces({ramp}, options, surfaces, report) &&
                                         surfaces.size() == 1 && surfaces[0].min_z == -0.02f && surfaces[0].max_z == 3);
    auto steep = ramp;
    for (auto& position : steep.vertices) position[2] = position[0] * 2;
    Check("steep-face-not-walkable", !ExtractLayerSourceSurfaces({steep}, options, surfaces, report));
    auto transformed = Floor(6, 0);
    for (auto& position : transformed.vertices) {
        position[0] = position[0] * 2 - 10;
        position[1] = position[1] * 3 - 8;
        position[2] = -5;
    }
    Check("canonical-transformed-negative-depth", ExtractLayerSourceSurfaces({transformed}, options, surfaces, report) &&
                                                     surfaces[0].bounds.min_x == -10 && surfaces[0].bounds.max_x == -2 &&
                                                     surfaces[0].bounds.min_y == -8 && surfaces[0].bounds.max_y == 4 &&
                                                     surfaces[0].min_z == -5.02f && surfaces[0].max_z == -3);
    auto water = Floor(7, 1, VolumeTagWater | VolumeTagDock);
    water.supports_ground_movement = false;
    Check("water-movement-policy-preserved", ExtractLayerSourceSurfaces({water}, options, surfaces, report) &&
                                                !surfaces[0].supports_ground_movement && surfaces[0].tags == water.tags);

    auto malformed = Floor(8, 0);
    malformed.indices.pop_back();
    Check("incomplete-triangle-rejected", !ExtractLayerSourceSurfaces({malformed}, options, surfaces, report) && surfaces.empty());
    malformed = Floor(8, 0);
    malformed.indices[0] = 999;
    Check("invalid-index-rejected", !ExtractLayerSourceSurfaces({malformed}, options, surfaces, report));
    malformed = Floor(8, 0);
    malformed.vertices[0][0] = std::numeric_limits<float>::quiet_NaN();
    Check("nan-vertex-rejected", !ExtractLayerSourceSurfaces({malformed}, options, surfaces, report));
    malformed.vertices[0][0] = std::numeric_limits<float>::infinity();
    Check("infinite-vertex-rejected", !ExtractLayerSourceSurfaces({malformed}, options, surfaces, report));
    malformed = Floor(8, 0);
    malformed.tags = 1u << 31;
    Check("unknown-semantic-tag-rejected", !ExtractLayerSourceSurfaces({malformed}, options, surfaces, report));
    Check("zero-source-id-rejected", !ExtractLayerSourceSurfaces({Floor(0, 0)}, options, surfaces, report));
    Check("duplicate-source-id-rejected", !ExtractLayerSourceSurfaces({Floor(8, 0), Floor(8, 4)}, options, surfaces, report));
    Check("empty-input-rejected", !ExtractLayerSourceSurfaces({}, options, surfaces, report));
    malformed = Floor(8, 0);
    malformed.vertices.clear();
    Check("missing-geometry-rejected", !ExtractLayerSourceSurfaces({malformed}, options, surfaces, report));
    malformed = Floor(8, 0);
    malformed.indices[2] = 1;
    Check("degenerate-triangle-rejected", !ExtractLayerSourceSurfaces({malformed}, options, surfaces, report));
    malformed = Floor(8, 0);
    malformed.indices.insert(malformed.indices.end(), {0, 1, 2});
    Check("duplicate-triangle-rejected", !ExtractLayerSourceSurfaces({malformed}, options, surfaces, report));
    malformed = Floor(8, 0);
    malformed.vertices.push_back({2, -2, 0});
    malformed.indices.insert(malformed.indices.end(), {0, 2, 4});
    Check("non-manifold-edge-rejected", !ExtractLayerSourceSurfaces({malformed}, options, surfaces, report));
    malformed = Floor(8, 0);
    std::swap(malformed.indices[4], malformed.indices[5]);
    Check("inconsistent-winding-rejected", !ExtractLayerSourceSurfaces({malformed}, options, surfaces, report));
    malformed = Floor(8, 0);
    malformed.vertices[2][2] = 0.1f;
    Check("nonplanar-quad-rejected", !ExtractLayerSourceSurfaces({malformed}, options, surfaces, report));
    malformed = Floor(8, 0);
    for (auto& position : malformed.vertices) {
        const float x = position[0], y = position[1];
        position[0] = x - y;
        position[1] = x + y;
    }
    Check("rotated-rectangle-does-not-invent-floor", !ExtractLayerSourceSurfaces({malformed}, options, surfaces, report));
    LayerCollisionMesh concave;
    concave.source_id = 9;
    Quad(concave, 0, 0, 1, 1, 0);
    Quad(concave, 1, 0, 1, 1, 0);
    Quad(concave, 0, 1, 1, 1, 0);
    Check("l-footprint-rejected", !ExtractLayerSourceSurfaces({concave}, options, surfaces, report));
    LayerCollisionMesh ring;
    ring.source_id = 10;
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 3; ++x) {
            if (x != 1 || y != 1) Quad(ring, static_cast<float>(x), static_cast<float>(y), 1, 1, 0);
        }
    }
    Check("hole-footprint-rejected", !ExtractLayerSourceSurfaces({ring}, options, surfaces, report));
    LayerCollisionMesh islands;
    islands.source_id = 11;
    Quad(islands, 0, 0, 1, 1, 0);
    Quad(islands, 2, 0, 1, 1, 0);
    Check("disconnected-rectangles-remain-separate", ExtractLayerSourceSurfaces({islands}, options, surfaces, report) &&
                                                       surfaces.size() == 2 &&
                                                       GenerateLayeredWorld(surfaces, generation_options, world, generation) &&
                                                       world.volumes.size() == 2 && !world.FindVolume(1.5f, 0.5f, 0));

    std::vector<LayerSourceSurface> l_surfaces{
        {1, "a", Rect{0, 0, 2, 1}, 0, 2, VolumeTagGround, true},
        {2, "b", Rect{0, 1, 1, 2}, 0, 2, VolumeTagGround, true}};
    Check("exact-mode-retains-l-hole", GenerateLayeredWorld(l_surfaces, generation_options, world, generation) &&
                                          world.volumes.size() == 2 && !world.FindVolume(1.5f, 1.5f, 1));
    Check("exact-mode-preserves-source-names", world.volumes.size() == 2 &&
                                                   world.volumes[0].name == "a" && world.volumes[1].name == "b");
    auto legacy_options = generation_options;
    legacy_options.require_exact_footprints = false;
    Check("legacy-merge-mode-unchanged", GenerateLayeredWorld(l_surfaces, legacy_options, world, generation) &&
                                             world.volumes.size() == 1 && world.FindVolume(1.5f, 1.5f, 1));
    Check("legacy-generated-name-unchanged", world.volumes.size() == 1 && world.volumes[0].name == "generated_1");
    l_surfaces[1].bounds = Rect{0, 0, 2, 1};
    Check("overlapping-sources-fail-validation", !GenerateLayeredWorld(l_surfaces, generation_options, world, generation));
    auto invalid_options = options;
    invalid_options.clearance_height = 0;
    Check("zero-clearance-rejected", !ExtractLayerSourceSurfaces({Floor(12, 0)}, invalid_options, surfaces, report));
    invalid_options = options;
    invalid_options.foot_tolerance = -1;
    Check("negative-foot-tolerance-rejected", !ExtractLayerSourceSurfaces({Floor(12, 0)}, invalid_options, surfaces, report));
    invalid_options = options;
    invalid_options.max_slope_degrees = 90;
    Check("vertical-slope-option-rejected", !ExtractLayerSourceSurfaces({Floor(12, 0)}, invalid_options, surfaces, report));
    invalid_options.max_slope_degrees = std::numeric_limits<float>::infinity();
    Check("infinite-option-rejected", !ExtractLayerSourceSurfaces({Floor(12, 0)}, invalid_options, surfaces, report));
    invalid_options = options;
    invalid_options.max_slope_degrees = 0;
    Check("zero-slope-accepts-horizontal-floor", ExtractLayerSourceSurfaces({Floor(12, 0)}, invalid_options, surfaces, report));
    malformed = Floor(13, 0);
    malformed.vertices.resize(kMaxLayerGeometryVertices + 1);
    Check("bounded-vertex-input", !ExtractLayerSourceSurfaces({malformed}, options, surfaces, report));
    malformed = Floor(13, 0);
    malformed.indices.resize((kMaxLayerGeometryTriangles + 1) * 3);
    Check("bounded-triangle-input", !ExtractLayerSourceSurfaces({malformed}, options, surfaces, report));
    std::cout << "LAYERED3D GEOMETRY summary: failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
