#include "map/LayeredWorldGeometry.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>
#include <tuple>

namespace mx::map {
namespace {

using Point = std::array<float, 3>;
using Edge = std::pair<std::size_t, std::size_t>;
constexpr double kPlaneTolerance = kLayerSupportCoplanarToleranceMeters;

struct Triangle {
    std::array<std::size_t, 3> vertices{};
    std::array<double, 3> normal{};
    double projected_area = 0;
    bool walkable = false;
};

struct EdgeIncident {
    std::size_t triangle = 0;
    bool forward = false;
};

struct Extracted {
    std::uint32_t mesh_id = 0;
    LayerSourceSurface surface;
};

Edge OrderedEdge(std::size_t lhs, std::size_t rhs)
{
    return std::minmax(lhs, rhs);
}

double PlaneDistance(const Point& point, const Point& origin, const Triangle& plane)
{
    double distance = 0;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        distance += (static_cast<double>(point[axis]) - origin[axis]) * plane.normal[axis];
    }
    return std::abs(distance);
}

bool SamePlane(const Triangle& lhs, const Triangle& rhs, const std::vector<Point>& vertices)
{
    double alignment = 0;
    for (std::size_t axis = 0; axis < 3; ++axis) alignment += lhs.normal[axis] * rhs.normal[axis];
    if (alignment < 1.0 - 1e-10) return false;
    for (const auto vertex : rhs.vertices) {
        if (PlaneDistance(vertices[vertex], vertices[lhs.vertices[0]], lhs) > kPlaneTolerance) return false;
    }
    return true;
}

bool ExtractMesh(const LayerCollisionMesh& mesh,
                 const LayerGeometryOptions& options,
                 std::vector<Extracted>& extracted,
                 LayerGeometryReport& report)
{
    const auto fail = [&](const std::string& message) {
        report.errors.push_back("collision mesh " + std::to_string(mesh.source_id) + " (" + mesh.name + "): " + message);
        return false;
    };
    if (mesh.vertices.empty() || mesh.indices.empty() || mesh.indices.size() % 3 != 0) {
        return fail("expected nonempty indexed triangle collision geometry");
    }
    // Lexicographic welding is independent of source vertex order. It only
    // welds equal positions: nearby edges are never silently stitched.
    std::map<Point, std::size_t> weld;
    for (const auto& position : mesh.vertices) {
        for (const float value : position) {
            if (!std::isfinite(value)) return fail("nonfinite collision vertex");
        }
        weld.emplace(position, 0);
    }
    std::vector<Point> vertices;
    vertices.reserve(weld.size());
    for (auto& entry : weld) {
        entry.second = vertices.size();
        Point canonical = entry.first;
        for (auto& value : canonical) if (value == 0.0f) value = 0.0f;
        vertices.push_back(canonical);
    }
    std::vector<std::size_t> remap;
    remap.reserve(mesh.vertices.size());
    for (const auto& position : mesh.vertices) remap.push_back(weld.at(position));

    const double min_up = std::cos(static_cast<double>(options.max_slope_degrees) * 3.14159265358979323846 / 180.0);
    std::vector<Triangle> triangles;
    triangles.reserve(mesh.indices.size() / 3);
    std::set<std::array<std::size_t, 3>> faces;
    std::map<Edge, std::vector<EdgeIncident>> edges;
    for (std::size_t index = 0; index < mesh.indices.size(); index += 3) {
        Triangle triangle;
        for (std::size_t corner = 0; corner < 3; ++corner) {
            if (mesh.indices[index + corner] >= remap.size()) return fail("triangle index outside vertex array");
            triangle.vertices[corner] = remap[mesh.indices[index + corner]];
        }
        auto canonical_face = triangle.vertices;
        std::sort(canonical_face.begin(), canonical_face.end());
        if (canonical_face[0] == canonical_face[1] || canonical_face[1] == canonical_face[2]) {
            return fail("degenerate triangle after exact coordinate welding");
        }
        if (!faces.insert(canonical_face).second) return fail("duplicate triangle");
        std::rotate(triangle.vertices.begin(),
                    std::min_element(triangle.vertices.begin(), triangle.vertices.end()), triangle.vertices.end());
        std::array<double, 3> a{}, b{};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            a[axis] = static_cast<double>(vertices[triangle.vertices[1]][axis]) - vertices[triangle.vertices[0]][axis];
            b[axis] = static_cast<double>(vertices[triangle.vertices[2]][axis]) - vertices[triangle.vertices[0]][axis];
        }
        triangle.normal = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
        const double length = std::sqrt(triangle.normal[0] * triangle.normal[0] +
                                        triangle.normal[1] * triangle.normal[1] +
                                        triangle.normal[2] * triangle.normal[2]);
        if (!(length > 0) || !std::isfinite(length)) return fail("degenerate or unrepresentable triangle");
        triangle.projected_area = triangle.normal[2] * 0.5;
        for (auto& value : triangle.normal) value /= length;
        triangle.walkable = triangle.normal[2] > 0 && triangle.normal[2] + 1e-12 >= min_up;
        if (triangle.walkable) ++report.triangles_walkable;
        triangles.push_back(triangle);
    }
    std::sort(triangles.begin(), triangles.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.vertices < rhs.vertices;
    });
    for (std::size_t triangle_index = 0; triangle_index < triangles.size(); ++triangle_index) {
        const auto& triangle = triangles[triangle_index];
        for (std::size_t corner = 0; corner < 3; ++corner) {
            const auto lhs = triangle.vertices[corner];
            const auto rhs = triangle.vertices[(corner + 1) % 3];
            auto& incidents = edges[OrderedEdge(lhs, rhs)];
            incidents.push_back({triangle_index, lhs < rhs});
            if (incidents.size() > 2) return fail("non-manifold edge has more than two incident faces");
            if (incidents.size() == 2 && incidents[0].forward == incidents[1].forward) {
                return fail("adjacent collision triangles have inconsistent winding");
            }
        }
    }

    std::vector<std::size_t> parents(triangles.size());
    std::iota(parents.begin(), parents.end(), 0);
    const auto find = [&](std::size_t node) {
        while (parents[node] != node) {
            parents[node] = parents[parents[node]];
            node = parents[node];
        }
        return node;
    };
    for (const auto& entry : edges) {
        if (entry.second.size() != 2) continue;
        const auto lhs = entry.second[0].triangle;
        const auto rhs = entry.second[1].triangle;
        if (triangles[lhs].walkable && triangles[rhs].walkable && SamePlane(triangles[lhs], triangles[rhs], vertices)) {
            const auto lhs_root = find(lhs);
            const auto rhs_root = find(rhs);
            if (lhs_root != rhs_root) parents[rhs_root] = lhs_root;
        }
    }
    std::map<std::size_t, std::vector<std::size_t>> components;
    for (std::size_t index = 0; index < triangles.size(); ++index) {
        if (triangles[index].walkable) components[find(index)].push_back(index);
    }
    if (components.empty()) {
        report.warnings.push_back("collision mesh " + std::to_string(mesh.source_id) + ": no upward walkable faces; skipped");
        return true;
    }
    std::uint32_t component_id = 0;
    for (const auto& component : components) {
        ++component_id;
        std::map<Edge, std::size_t> component_edges;
        std::set<std::size_t> component_vertices;
        double area = 0;
        const auto& plane = triangles[component.second[0]];
        for (const auto index : component.second) {
            const auto& triangle = triangles[index];
            area += triangle.projected_area;
            for (const auto vertex : triangle.vertices) {
                if (PlaneDistance(vertices[vertex], vertices[plane.vertices[0]], plane) > kPlaneTolerance) {
                    return fail("walkable component is not coplanar; split the collision into explicit planar pieces");
                }
                component_vertices.insert(vertex);
            }
            for (std::size_t corner = 0; corner < 3; ++corner) {
                ++component_edges[OrderedEdge(triangle.vertices[corner], triangle.vertices[(corner + 1) % 3])];
            }
        }
        const auto& first = vertices[*component_vertices.begin()];
        Rect bounds{first[0], first[1], first[0], first[1]};
        float floor_min = first[2], floor_max = first[2];
        for (const auto index : component_vertices) {
            const auto& point = vertices[index];
            bounds.min_x = std::min(bounds.min_x, point[0]);
            bounds.min_y = std::min(bounds.min_y, point[1]);
            bounds.max_x = std::max(bounds.max_x, point[0]);
            bounds.max_y = std::max(bounds.max_y, point[1]);
            floor_min = std::min(floor_min, point[2]);
            floor_max = std::max(floor_max, point[2]);
        }
        const double width = static_cast<double>(bounds.max_x) - bounds.min_x;
        const double height = static_cast<double>(bounds.max_y) - bounds.min_y;
        if (!(width > 0 && height > 0)) return fail("walkable component has a degenerate footprint");
        std::map<std::size_t, std::vector<std::size_t>> boundary;
        std::size_t boundary_edges = 0;
        for (const auto& entry : component_edges) {
            if (entry.second != 1) continue;
            ++boundary_edges;
            const auto lhs = entry.first.first, rhs = entry.first.second;
            const auto& a = vertices[lhs];
            const auto& b = vertices[rhs];
            // Exact float coordinates intentionally reject rotated, concave or
            // holed footprints instead of converting their AABB into floor.
            const bool on_rectangle =
                (a[0] == bounds.min_x && b[0] == bounds.min_x) ||
                (a[0] == bounds.max_x && b[0] == bounds.max_x) ||
                (a[1] == bounds.min_y && b[1] == bounds.min_y) ||
                (a[1] == bounds.max_y && b[1] == bounds.max_y);
            if (!on_rectangle) return fail("footprint is not an axis-aligned rectangle (concave outline, hole or rotated edge); split into rectangular collision pieces");
            boundary[lhs].push_back(rhs);
            boundary[rhs].push_back(lhs);
        }
        if (boundary_edges < 4) return fail("walkable component has no rectangular boundary");
        for (const auto& vertex : boundary) {
            if (vertex.second.size() != 2) return fail("non-manifold walkable boundary");
        }
        // A single closed boundary plus the oriented face area proves the
        // rectangle is covered; disconnected rings and duplicate coverage fail.
        const auto start = boundary.begin()->first;
        auto current = start;
        auto previous = static_cast<std::size_t>(-1);
        std::set<std::size_t> visited;
        do {
            if (!visited.insert(current).second) return fail("walkable boundary self-intersects");
            const auto& neighbours = boundary.at(current);
            const auto next = neighbours[0] == previous ? neighbours[1] : neighbours[0];
            previous = current;
            current = next;
        } while (current != start);
        if (visited.size() != boundary.size()) return fail("walkable component has multiple boundary rings");
        const double rectangle_area = width * height;
        if (std::abs(area - rectangle_area) > rectangle_area * 1e-7) {
            return fail("collision triangles do not cover their rectangular footprint exactly");
        }
        if (extracted.size() >= kMaxLayeredWorldVolumes) return fail("cooked surface count exceeds the bounded layer limit");
        LayerSourceSurface surface;
        surface.name = mesh.name;
        surface.bounds = bounds;
        surface.min_z = floor_min - options.foot_tolerance;
        surface.max_z = floor_max + options.clearance_height;
        surface.tags = mesh.tags;
        surface.supports_ground_movement = mesh.supports_ground_movement;
        if (mesh.supports_ground_movement && !HasVolumeTag(mesh.tags, VolumeTagWater) &&
            !HasVolumeTag(mesh.tags, VolumeTagUnderwater)) {
            const auto& anchor = vertices[plane.vertices[0]];
            LayerSupportPlane support;
            support.source_id = mesh.source_id;
            support.component_id = component_id;
            support.anchor_x = anchor[0];
            support.anchor_y = anchor[1];
            support.anchor_z = anchor[2];
            support.slope_x = -plane.normal[0] / plane.normal[2];
            support.slope_y = -plane.normal[1] / plane.normal[2];
            for (const auto index : component_vertices) {
                const auto& position = vertices[index];
                support.max_height_error_m = std::max(support.max_height_error_m,
                    std::abs(support.Height(position[0], position[1]) - position[2]));
            }
            surface.ground_support = support;
        }
        if (!std::isfinite(surface.min_z) || !std::isfinite(surface.max_z) || !(surface.min_z < surface.max_z)) {
            return fail("floor height band is not representable; check scale and clearance");
        }
        extracted.push_back({mesh.source_id, std::move(surface)});
    }
    return true;
}

} // namespace

bool ExtractLayerSourceSurfaces(const std::vector<LayerCollisionMesh>& meshes,
                                const LayerGeometryOptions& options,
                                std::vector<LayerSourceSurface>& surfaces,
                                LayerGeometryReport& report)
{
    surfaces.clear();
    report = {};
    report.meshes_seen = meshes.size();
    if (!std::isfinite(options.clearance_height) || options.clearance_height <= 0 ||
        !std::isfinite(options.foot_tolerance) || options.foot_tolerance < 0 ||
        !std::isfinite(options.max_slope_degrees) || options.max_slope_degrees < 0 || options.max_slope_degrees >= 90) {
        report.errors.push_back("clearance must be positive, foot tolerance nonnegative, and slope in [0, 90) degrees; all options must be finite");
        return false;
    }
    if (meshes.empty() || meshes.size() > kMaxLayeredWorldVolumes) {
        report.errors.push_back("expected between one and 4096 collision mesh sources");
        return false;
    }
    std::size_t vertex_count = 0;
    std::set<std::uint32_t> ids;
    for (const auto& mesh : meshes) {
        if (mesh.source_id == 0 || !ids.insert(mesh.source_id).second ||
            (mesh.tags & ~kKnownVolumeTags) != 0) {
            report.errors.push_back("collision mesh requires a unique nonzero source id and known semantic tags");
            return false;
        }
        if (mesh.vertices.size() > kMaxLayerGeometryVertices - vertex_count ||
            mesh.indices.size() / 3 > kMaxLayerGeometryTriangles - report.triangles_seen) {
            report.errors.push_back("collision input exceeds the bounded vertex or triangle limit");
            return false;
        }
        vertex_count += mesh.vertices.size();
        report.triangles_seen += mesh.indices.size() / 3;
    }
    std::vector<const LayerCollisionMesh*> ordered;
    ordered.reserve(meshes.size());
    for (const auto& mesh : meshes) ordered.push_back(&mesh);
    std::sort(ordered.begin(), ordered.end(), [](const auto* lhs, const auto* rhs) { return lhs->source_id < rhs->source_id; });
    std::vector<Extracted> extracted;
    for (const auto* mesh : ordered) {
        if (!ExtractMesh(*mesh, options, extracted, report)) return false;
    }
    if (extracted.empty()) {
        report.errors.push_back("no upward walkable collision surface was found; render bounds are not floor geometry");
        return false;
    }
    std::sort(extracted.begin(), extracted.end(), [](const Extracted& lhs, const Extracted& rhs) {
        return std::tie(lhs.mesh_id, lhs.surface.min_z, lhs.surface.bounds.min_x, lhs.surface.bounds.min_y,
                        lhs.surface.bounds.max_x, lhs.surface.bounds.max_y, lhs.surface.max_z) <
               std::tie(rhs.mesh_id, rhs.surface.min_z, rhs.surface.bounds.min_x, rhs.surface.bounds.min_y,
                        rhs.surface.bounds.max_x, rhs.surface.bounds.max_y, rhs.surface.max_z);
    });
    surfaces.reserve(extracted.size());
    for (std::size_t index = 0; index < extracted.size(); ++index) {
        auto surface = std::move(extracted[index].surface);
        surface.source_id = static_cast<std::uint32_t>(index + 1);
        surfaces.push_back(std::move(surface));
    }
    report.surfaces_generated = surfaces.size();
    return true;
}

} // namespace mx::map
