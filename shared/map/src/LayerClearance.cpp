#include "map/LayerClearance.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace mx::map {
namespace {

using Point3 = std::array<double, 3>;

// Geometric slack added to every region so float -> double conversions of
// footprints and cell edges never shrink the tested space.
constexpr double kRegionSlack = 1e-6;

struct Triangle {
    std::array<Point3, 3> v{};
    double min_x = 0, max_x = 0, min_y = 0, max_y = 0, min_z = 0, max_z = 0;
};

Triangle MakeTriangle(const Point3& a, const Point3& b, const Point3& c)
{
    Triangle t;
    t.v = {a, b, c};
    t.min_x = std::min({a[0], b[0], c[0]});
    t.max_x = std::max({a[0], b[0], c[0]});
    t.min_y = std::min({a[1], b[1], c[1]});
    t.max_y = std::max({a[1], b[1], c[1]});
    t.min_z = std::min({a[2], b[2], c[2]});
    t.max_z = std::max({a[2], b[2], c[2]});
    return t;
}

// a*x + b*y + c*z + d >= 0 (closed half-space).
struct HalfSpace {
    double a = 0, b = 0, c = 0, d = 0;
    double Eval(const Point3& p) const noexcept { return a * p[0] + b * p[1] + c * p[2] + d; }
};

// Sutherland-Hodgman clipping of the triangle against closed half-spaces.
// Touching counts as intersecting; that only makes the test conservative.
bool ClipNonEmpty(const Triangle& triangle, const HalfSpace* planes, std::size_t plane_count)
{
    std::array<Point3, 24> buffer_a{}, buffer_b{};
    std::size_t count = 3;
    for (std::size_t i = 0; i < 3; ++i) buffer_a[i] = triangle.v[i];
    Point3* in = buffer_a.data();
    Point3* out = buffer_b.data();
    for (std::size_t p = 0; p < plane_count; ++p) {
        std::size_t written = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const Point3& current = in[i];
            const Point3& next = in[(i + 1) % count];
            const double fc = planes[p].Eval(current);
            const double fn = planes[p].Eval(next);
            if (fc >= 0.0) out[written++] = current;
            if ((fc >= 0.0) != (fn >= 0.0)) {
                const double t = fc / (fc - fn);
                out[written++] = {current[0] + t * (next[0] - current[0]),
                                  current[1] + t * (next[1] - current[1]),
                                  current[2] + t * (next[2] - current[2])};
            }
        }
        if (written == 0) return false;
        count = written;
        std::swap(in, out);
    }
    return true;
}

struct Plane {
    double slope_x = 0, slope_y = 0, offset = 0; // z = slope_x*x + slope_y*y + offset
    double sec = 1; // 1 / cos(slope) = sqrt(1 + |grad|^2)
    double Height(double x, double y) const noexcept { return slope_x * x + slope_y * y + offset; }
};

Plane PlaneOf(const LayerSupportPlane& support)
{
    Plane plane;
    plane.slope_x = support.slope_x;
    plane.slope_y = support.slope_y;
    plane.offset = support.anchor_z - support.slope_x * support.anchor_x - support.slope_y * support.anchor_y;
    plane.sec = std::sqrt(1.0 + support.slope_x * support.slope_x + support.slope_y * support.slope_y);
    return plane;
}

HalfSpace AbovePlane(const Plane& plane, double contact)
{
    // z - slope_x*x - slope_y*y - (offset + contact) >= 0
    return {-plane.slope_x, -plane.slope_y, 1.0, -(plane.offset + contact)};
}

struct Rect2 {
    double x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    bool Overlaps(double min_x, double max_x, double min_y, double max_y) const noexcept
    {
        return min_x <= x1 && max_x >= x0 && min_y <= y1 && max_y >= y0;
    }
};

// The obstruction region over a rectangle of capsule centres: centres
// expanded by the radius, above every given plane + floor contact (the upper
// envelope), below the cap. Returns the number of half-spaces written.
std::size_t BuildRegion(const Rect2& centres, double radius, const Plane* planes, std::size_t plane_count,
                        const LayerClearanceProfile& profile, HalfSpace* out)
{
    const double grow = radius + kRegionSlack;
    const Rect2 area{centres.x0 - grow, centres.x1 + grow, centres.y0 - grow, centres.y1 + grow};
    double cap = -std::numeric_limits<double>::infinity();
    double sec = 1.0;
    for (std::size_t p = 0; p < plane_count; ++p) sec = std::max(sec, planes[p].sec);
    for (const double x : {centres.x0, centres.x1}) {
        for (const double y : {centres.y0, centres.y1}) {
            for (std::size_t p = 0; p < plane_count; ++p) cap = std::max(cap, planes[p].Height(x, y));
        }
    }
    // Upright capsule resting on a slope: its top is radius * (sec - 1)
    // higher than on flat ground.
    cap += static_cast<double>(profile.actor_height_m) + radius * (sec - 1.0) + kRegionSlack;
    std::size_t n = 0;
    out[n++] = {1, 0, 0, -area.x0};
    out[n++] = {-1, 0, 0, area.x1};
    out[n++] = {0, 1, 0, -area.y0};
    out[n++] = {0, -1, 0, area.y1};
    out[n++] = {0, 0, -1, cap};
    for (std::size_t p = 0; p < plane_count; ++p) {
        out[n++] = AbovePlane(planes[p], static_cast<double>(profile.floor_contact_m));
    }
    return n;
}

bool EntirelyBelow(const Triangle& triangle, const Plane* planes, std::size_t plane_count, double contact)
{
    // Linear functions attain their maximum over a triangle at a vertex. If
    // every vertex is below ANY one of the planes, the triangle cannot reach
    // the region above the upper envelope. (Below all is implied by one.)
    for (std::size_t p = 0; p < plane_count; ++p) {
        const auto h = AbovePlane(planes[p], contact);
        if (h.Eval(triangle.v[0]) < 0.0 && h.Eval(triangle.v[1]) < 0.0 && h.Eval(triangle.v[2]) < 0.0) {
            return true;
        }
    }
    return false;
}

class ObstructionSet {
public:
    ObstructionSet(const std::vector<LayerObstructionMesh>& meshes, const LayerTerrainObstruction* terrain)
        : terrain_(terrain)
    {
        for (const auto& mesh : meshes) {
            for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
                const auto& a = mesh.vertices[mesh.indices[i]];
                const auto& b = mesh.vertices[mesh.indices[i + 1]];
                const auto& c = mesh.vertices[mesh.indices[i + 2]];
                triangles_.push_back(MakeTriangle({a[0], a[1], a[2]}, {b[0], b[1], b[2]}, {c[0], c[1], c[2]}));
            }
        }
    }

    // Calls visit(triangle) for every mesh triangle and every terrain
    // triangle (both diagonals) whose XY bounds reach `area`.
    template <class Visit>
    void ForEach(const Rect2& area, std::uint64_t& terrain_quads, Visit&& visit) const
    {
        for (const auto& triangle : triangles_) {
            if (area.Overlaps(triangle.min_x, triangle.max_x, triangle.min_y, triangle.max_y)) visit(triangle);
        }
        if (terrain_ == nullptr || terrain_->cells_x == 0 || terrain_->cells_y == 0) return;
        const auto& t = *terrain_;
        const auto index = [&](double value, double origin, std::uint32_t cells) {
            const double raw = std::floor((value - origin) / t.cell_size);
            return static_cast<std::int64_t>(std::clamp(raw, -1.0, static_cast<double>(cells)));
        };
        const auto i0 = std::max<std::int64_t>(0, index(area.x0, t.origin_x, t.cells_x));
        const auto i1 = std::min<std::int64_t>(t.cells_x - 1, index(area.x1, t.origin_x, t.cells_x));
        const auto j0 = std::max<std::int64_t>(0, index(area.y0, t.origin_y, t.cells_y));
        const auto j1 = std::min<std::int64_t>(t.cells_y - 1, index(area.y1, t.origin_y, t.cells_y));
        const std::size_t stride = static_cast<std::size_t>(t.cells_x) + 1;
        for (auto j = j0; j <= j1; ++j) {
            for (auto i = i0; i <= i1; ++i) {
                const auto sample = [&](std::int64_t si, std::int64_t sj) {
                    return Point3{t.origin_x + static_cast<double>(si) * t.cell_size,
                                  t.origin_y + static_cast<double>(sj) * t.cell_size,
                                  static_cast<double>(t.heights[static_cast<std::size_t>(sj) * stride +
                                                               static_cast<std::size_t>(si)])};
                };
                const Point3 p00 = sample(i, j), p10 = sample(i + 1, j), p01 = sample(i, j + 1),
                             p11 = sample(i + 1, j + 1);
                ++terrain_quads;
                visit(MakeTriangle(p00, p10, p11));
                visit(MakeTriangle(p00, p11, p01));
                visit(MakeTriangle(p00, p10, p01));
                visit(MakeTriangle(p10, p11, p01));
            }
        }
    }

    std::size_t MeshTriangles() const noexcept { return triangles_.size(); }

private:
    std::vector<Triangle> triangles_;
    const LayerTerrainObstruction* terrain_;
};

void SetBit(std::vector<std::uint8_t>& bits, std::uint64_t index)
{
    bits[static_cast<std::size_t>(index / 8)] |= static_cast<std::uint8_t>(1u << (index % 8));
}

bool GetBit(const std::vector<std::uint8_t>& bits, std::uint64_t index)
{
    return ((bits[static_cast<std::size_t>(index / 8)] >> (index % 8)) & 1u) != 0;
}

// Centre rectangle of one grid cell; the last row/column ends at the footprint.
Rect2 CellRect(const Rect& bounds, double cell, std::uint32_t i, std::uint32_t j)
{
    const double x0 = static_cast<double>(bounds.min_x) + static_cast<double>(i) * cell;
    const double y0 = static_cast<double>(bounds.min_y) + static_cast<double>(j) * cell;
    return {x0, std::min(x0 + cell, static_cast<double>(bounds.max_x)),
            y0, std::min(y0 + cell, static_cast<double>(bounds.max_y))};
}

void CookVolume(LayerVolume& volume, const ObstructionSet& obstructions, const LayerClearanceProfile& profile,
                LayerClearanceReport& report)
{
    const double cell = static_cast<double>(profile.cell_size_m);
    const double radius = static_cast<double>(profile.actor_radius_m);
    LayerClearanceGrid grid;
    grid.cells_x = LayerClearanceCellCount(volume.bounds.min_x, volume.bounds.max_x, profile.cell_size_m);
    grid.cells_y = LayerClearanceCellCount(volume.bounds.min_y, volume.bounds.max_y, profile.cell_size_m);
    const std::uint64_t cells = static_cast<std::uint64_t>(grid.cells_x) * grid.cells_y;
    grid.blocked.assign(static_cast<std::size_t>((cells + 7) / 8), 0);
    const Plane plane = PlaneOf(*volume.ground_support);
    const Rect2 footprint{volume.bounds.min_x, volume.bounds.max_x, volume.bounds.min_y, volume.bounds.max_y};
    const double grow = radius + kRegionSlack;
    const Rect2 area{footprint.x0 - grow, footprint.x1 + grow, footprint.y0 - grow, footprint.y1 + grow};
    std::array<HalfSpace, 8> region{};
    obstructions.ForEach(area, report.terrain_quads_tested, [&](const Triangle& triangle) {
        if (EntirelyBelow(triangle, &plane, 1, static_cast<double>(profile.floor_contact_m))) return;
        const auto first = [&](double value, double origin, std::uint32_t count) {
            const double raw = std::floor((value - grow - origin) / cell);
            return static_cast<std::uint32_t>(std::clamp(raw, 0.0, static_cast<double>(count - 1)));
        };
        const auto last = [&](double value, double origin, std::uint32_t count) {
            const double raw = std::floor((value + grow - origin) / cell);
            return static_cast<std::uint32_t>(std::clamp(raw, 0.0, static_cast<double>(count - 1)));
        };
        const auto i0 = first(triangle.min_x, footprint.x0, grid.cells_x);
        const auto i1 = last(triangle.max_x, footprint.x0, grid.cells_x);
        const auto j0 = first(triangle.min_y, footprint.y0, grid.cells_y);
        const auto j1 = last(triangle.max_y, footprint.y0, grid.cells_y);
        for (std::uint32_t j = j0; j <= j1; ++j) {
            for (std::uint32_t i = i0; i <= i1; ++i) {
                const std::uint64_t bit = static_cast<std::uint64_t>(j) * grid.cells_x + i;
                if (GetBit(grid.blocked, bit)) continue;
                const Rect2 centres = CellRect(volume.bounds, cell, i, j);
                const std::size_t n = BuildRegion(centres, radius, &plane, 1, profile, region.data());
                if (ClipNonEmpty(triangle, region.data(), n)) SetBit(grid.blocked, bit);
            }
        }
    });
    report.cells_total += cells;
    report.cells_blocked += grid.BlockedCount();
    ++report.volumes_with_clearance;
    volume.clearance = std::move(grid);
}

struct Corridor {
    double across0 = 0, across1 = 0;
};

Corridor CorridorFor(const LayerPortalProof& proof, const LayerVolume& a, const LayerVolume& b,
                     const LayerClearanceProfile& profile)
{
    Corridor corridor;
    LayerPortalCorridorAcross(proof, a, b, profile, corridor.across0, corridor.across1);
    return corridor;
}

bool DerivePortal(const LayerVolume& a, const LayerVolume& b, std::uint8_t axis, const LayerClearanceProfile& profile,
                  LayerPortalProof& proof, LayerClearanceReport& report)
{
    const float a_min = axis == 0 ? a.bounds.min_x : a.bounds.min_y;
    const float a_max = axis == 0 ? a.bounds.max_x : a.bounds.max_y;
    const float b_min = axis == 0 ? b.bounds.min_x : b.bounds.min_y;
    const float b_max = axis == 0 ? b.bounds.max_x : b.bounds.max_y;
    float edge = 0.0f;
    if (a_max == b_min) edge = a_max;
    else if (a_min == b_max) edge = a_min;
    else return false;
    const float ao_min = axis == 0 ? a.bounds.min_y : a.bounds.min_x;
    const float ao_max = axis == 0 ? a.bounds.max_y : a.bounds.max_x;
    const float bo_min = axis == 0 ? b.bounds.min_y : b.bounds.min_x;
    const float bo_max = axis == 0 ? b.bounds.max_y : b.bounds.max_x;
    const float span_min = std::max(ao_min, bo_min);
    const float span_max = std::min(ao_max, bo_max);
    if (!(span_min < span_max)) return false;
    double step = 0.0;
    for (const double along : {static_cast<double>(span_min), static_cast<double>(span_max)}) {
        const double x = axis == 0 ? static_cast<double>(edge) : along;
        const double y = axis == 0 ? along : static_cast<double>(edge);
        step = std::max(step, std::abs(a.ground_support->Height(x, y) - b.ground_support->Height(x, y)));
    }
    if (!std::isfinite(step) || step > static_cast<double>(profile.step_height_m)) {
        ++report.edges_rejected_step;
        return false;
    }
    proof = {};
    proof.axis = axis;
    proof.edge = edge;
    proof.span_min = span_min;
    proof.span_max = span_max;
    proof.max_step_m = step;
    proof.slots = LayerClearanceCellCount(span_min, span_max, profile.cell_size_m);
    return proof.slots != 0;
}

void CookCorridor(LayerPortalProof& proof, const LayerVolume& a, const LayerVolume& b,
                  const ObstructionSet& obstructions, const LayerClearanceProfile& profile,
                  LayerClearanceReport& report)
{
    const double cell = static_cast<double>(profile.cell_size_m);
    const double radius = static_cast<double>(profile.actor_radius_m);
    const Corridor corridor = CorridorFor(proof, a, b, profile);
    proof.across = LayerClearanceCellCount(corridor.across0, corridor.across1, profile.cell_size_m);
    const std::uint64_t cells = static_cast<std::uint64_t>(proof.slots) * proof.across;
    proof.blocked.assign(static_cast<std::size_t>((cells + 7) / 8), 0);
    if (cells == 0) return;
    const std::array<Plane, 2> planes{PlaneOf(*a.ground_support), PlaneOf(*b.ground_support)};
    // Corridor cell (slot, c): along [span_min + slot*cell, ...], across
    // [across0 + c*cell, ...], each clipped to the segment / band end.
    const auto cell_rect = [&](std::uint32_t slot, std::uint32_t c) {
        const double along0 = static_cast<double>(proof.span_min) + static_cast<double>(slot) * cell;
        const double along1 = std::min(along0 + cell, static_cast<double>(proof.span_max));
        const double across0 = corridor.across0 + static_cast<double>(c) * cell;
        const double across1 = std::min(across0 + cell, corridor.across1);
        return proof.axis == 0 ? Rect2{across0, across1, along0, along1} : Rect2{along0, along1, across0, across1};
    };
    const Rect2 all = proof.axis == 0
        ? Rect2{corridor.across0, corridor.across1, proof.span_min, proof.span_max}
        : Rect2{proof.span_min, proof.span_max, corridor.across0, corridor.across1};
    const double grow = radius + kRegionSlack;
    const Rect2 area{all.x0 - grow, all.x1 + grow, all.y0 - grow, all.y1 + grow};
    const auto range = [&](double min, double max, double origin, std::uint32_t count, std::uint32_t& first,
                           std::uint32_t& last) {
        first = static_cast<std::uint32_t>(std::clamp(std::floor((min - grow - origin) / cell), 0.0,
                                                      static_cast<double>(count - 1)));
        last = static_cast<std::uint32_t>(std::clamp(std::floor((max + grow - origin) / cell), 0.0,
                                                     static_cast<double>(count - 1)));
    };
    std::array<HalfSpace, 8> region{};
    obstructions.ForEach(area, report.terrain_quads_tested, [&](const Triangle& triangle) {
        if (EntirelyBelow(triangle, planes.data(), planes.size(), static_cast<double>(profile.floor_contact_m))) return;
        std::uint32_t s0 = 0, s1 = 0, c0 = 0, c1 = 0;
        const double along_min = proof.axis == 0 ? triangle.min_y : triangle.min_x;
        const double along_max = proof.axis == 0 ? triangle.max_y : triangle.max_x;
        const double across_min = proof.axis == 0 ? triangle.min_x : triangle.min_y;
        const double across_max = proof.axis == 0 ? triangle.max_x : triangle.max_y;
        range(along_min, along_max, proof.span_min, proof.slots, s0, s1);
        range(across_min, across_max, corridor.across0, proof.across, c0, c1);
        for (std::uint32_t slot = s0; slot <= s1; ++slot) {
            for (std::uint32_t c = c0; c <= c1; ++c) {
                const std::uint64_t bit = static_cast<std::uint64_t>(slot) * proof.across + c;
                if (GetBit(proof.blocked, bit)) continue;
                const std::size_t n = BuildRegion(cell_rect(slot, c), radius, planes.data(), planes.size(), profile,
                                                  region.data());
                if (ClipNonEmpty(triangle, region.data(), n)) SetBit(proof.blocked, bit);
            }
        }
    });
    report.corridor_slots += cells;
    for (std::uint64_t bit = 0; bit < cells; ++bit) {
        report.corridor_slots_blocked += GetBit(proof.blocked, bit) ? 1u : 0u;
    }
}

Rect Strip(const LayerVolume& volume, const LayerPortalProof& proof, const LayerClearanceProfile& profile)
{
    const float reach = profile.actor_radius_m + profile.cell_size_m;
    Rect strip = volume.bounds;
    const bool low_side = (proof.axis == 0 ? volume.bounds.max_x : volume.bounds.max_y) == proof.edge;
    if (proof.axis == 0) {
        strip.min_y = proof.span_min;
        strip.max_y = proof.span_max;
        if (low_side) strip.min_x = std::max(volume.bounds.min_x, proof.edge - reach);
        else strip.max_x = std::min(volume.bounds.max_x, proof.edge + reach);
    } else {
        strip.min_x = proof.span_min;
        strip.max_x = proof.span_max;
        if (low_side) strip.min_y = std::max(volume.bounds.min_y, proof.edge - reach);
        else strip.max_y = std::min(volume.bounds.max_y, proof.edge + reach);
    }
    return strip;
}

// ---- 3D-5B2 terrain edges ----------------------------------------------------

// Height bounds of every terrain quad a closed XY rectangle touches. False
// when the rectangle reaches outside the terrain grid (no data = no proof).
bool TerrainRange(const LayerTerrainObstruction& t, double x0, double x1, double y0, double y1, double& lo, double& hi)
{
    const double gx1 = t.origin_x + static_cast<double>(t.cells_x) * t.cell_size;
    const double gy1 = t.origin_y + static_cast<double>(t.cells_y) * t.cell_size;
    if (x0 < t.origin_x || y0 < t.origin_y || x1 > gx1 || y1 > gy1) return false;
    const auto index = [&](double value, double origin, std::uint32_t cells) {
        const double raw = std::floor((value - origin) / t.cell_size);
        return static_cast<std::uint32_t>(std::clamp(raw, 0.0, static_cast<double>(cells - 1)));
    };
    // A rectangle edge exactly on a grid line also touches the quad before it.
    const auto i0 = index(x0 - kRegionSlack, t.origin_x, t.cells_x), i1 = index(x1 + kRegionSlack, t.origin_x, t.cells_x);
    const auto j0 = index(y0 - kRegionSlack, t.origin_y, t.cells_y), j1 = index(y1 + kRegionSlack, t.origin_y, t.cells_y);
    const std::size_t stride = static_cast<std::size_t>(t.cells_x) + 1;
    lo = std::numeric_limits<double>::infinity();
    hi = -std::numeric_limits<double>::infinity();
    for (std::uint32_t j = j0; j <= j1 + 1; ++j) {
        for (std::uint32_t i = i0; i <= i1 + 1; ++i) {
            const double h = t.heights[static_cast<std::size_t>(j) * stride + i];
            lo = std::min(lo, h);
            hi = std::max(hi, h);
        }
    }
    return true;
}

// Is the slot [along0, along1] of this volume edge covered by a proven
// volume-to-volume portal (then the outside is another floor, not terrain)?
bool PortalCoversSlot(const LayeredWorld& world, const LayerVolume& volume, std::uint8_t axis, float edge,
                      double along0, double along1)
{
    for (const auto& portal : world.portals) {
        if (!portal.proof || (portal.source_volume != volume.id && portal.target_volume != volume.id)) continue;
        const auto& proof = *portal.proof;
        if (proof.axis == axis && proof.edge == edge && along0 < proof.span_max && proof.span_min < along1) return true;
    }
    return false;
}

// One terrain edge candidate per footprint side. A slot is valid when the
// terrain along the slot segment stays within the step of the plane
// (conservative quad corner bounds) and no proven portal owns it; invalid
// slots keep every corridor cell blocked, so the runtime refuses crossings
// there. Corridor cells of valid slots are free above the upper envelope of
// the plane and the local terrain maximum.
bool CookTerrainEdge(const LayeredWorld& world, const LayerVolume& volume, std::uint8_t axis, std::uint8_t side,
                     const Rect& world_bounds, const LayerTerrainObstruction& terrain, const ObstructionSet& obstructions,
                     const LayerClearanceProfile& profile, LayerTerrainEdge& out, std::vector<Rect2>& support,
                     LayerClearanceReport& report)
{
    const float edge = axis == 0 ? (side == 0 ? volume.bounds.min_x : volume.bounds.max_x)
                                 : (side == 0 ? volume.bounds.min_y : volume.bounds.max_y);
    const float world_min = axis == 0 ? world_bounds.min_x : world_bounds.min_y;
    const float world_max = axis == 0 ? world_bounds.max_x : world_bounds.max_y;
    if (side == 0 ? !(edge > world_min) : !(edge < world_max)) return false;
    LayerTerrainEdge candidate;
    candidate.volume_id = volume.id;
    candidate.axis = axis;
    candidate.terrain_side = side;
    candidate.edge = edge;
    candidate.span_min = axis == 0 ? volume.bounds.min_y : volume.bounds.min_x;
    candidate.span_max = axis == 0 ? volume.bounds.max_y : volume.bounds.max_x;
    candidate.slots = LayerClearanceCellCount(candidate.span_min, candidate.span_max, profile.cell_size_m);
    double across0 = 0.0, across1 = 0.0;
    LayerTerrainEdgeCorridorAcross(candidate, volume, profile, across0, across1);
    candidate.across = LayerClearanceCellCount(across0, across1, profile.cell_size_m);
    const std::uint64_t cells = static_cast<std::uint64_t>(candidate.slots) * candidate.across;
    if (cells == 0 || cells > kMaxLayerClearanceCellsPerVolume) return false;
    candidate.blocked.assign(static_cast<std::size_t>((cells + 7) / 8), 0);

    const double cell = static_cast<double>(profile.cell_size_m);
    const double radius = static_cast<double>(profile.actor_radius_m);
    const double step = static_cast<double>(profile.step_height_m);
    const Plane plane = PlaneOf(*volume.ground_support);
    const auto along_range = [&](std::uint32_t slot, double& a0, double& a1) {
        a0 = static_cast<double>(candidate.span_min) + static_cast<double>(slot) * cell;
        a1 = std::min(a0 + cell, static_cast<double>(candidate.span_max));
    };
    const auto point = [&](double along, double across) {
        return axis == 0 ? std::array<double, 2>{across, along} : std::array<double, 2>{along, across};
    };
    std::vector<bool> valid(candidate.slots, false);
    bool any_valid = false;
    // Terrain beyond each valid slot is ground the actor may rest on (3D-5D
    // open-ledge erosion support); the strip reaches past the radius.
    std::vector<Rect2> valid_strips;
    const double strip_reach = static_cast<double>(profile.actor_radius_m) + static_cast<double>(profile.cell_size_m);
    for (std::uint32_t slot = 0; slot < candidate.slots; ++slot) {
        double a0 = 0, a1 = 0;
        along_range(slot, a0, a1);
        if (PortalCoversSlot(world, volume, axis, edge, a0, a1)) continue;
        const auto p0 = point(a0, edge), p1 = point(a1, edge);
        double t_lo = 0, t_hi = 0;
        if (!TerrainRange(terrain, std::min(p0[0], p1[0]), std::max(p0[0], p1[0]), std::min(p0[1], p1[1]),
                          std::max(p0[1], p1[1]), t_lo, t_hi)) {
            continue;
        }
        const double p_lo = std::min(plane.Height(p0[0], p0[1]), plane.Height(p1[0], p1[1]));
        const double p_hi = std::max(plane.Height(p0[0], p0[1]), plane.Height(p1[0], p1[1]));
        const double bound = std::max(t_hi - p_lo, p_hi - t_lo);
        if (!std::isfinite(bound) || bound > step) {
            ++report.edges_rejected_step;
            continue;
        }
        valid[slot] = true;
        any_valid = true;
        candidate.max_step_m = std::max(candidate.max_step_m, std::max(0.0, bound));
        const double outer = side == 0 ? static_cast<double>(edge) - strip_reach : static_cast<double>(edge) + strip_reach;
        const double t0 = std::min(outer, static_cast<double>(edge)), t1 = std::max(outer, static_cast<double>(edge));
        valid_strips.push_back(axis == 0 ? Rect2{t0, t1, a0, a1} : Rect2{a0, a1, t0, t1});
    }
    if (!any_valid) return false;
    // Past a volume corner the terrain continues: a valid end slot also
    // supports the square beyond the corner when that terrain stays within the
    // step of the plane at the corner (the same quad-corner bound).
    for (const std::uint32_t end : {0u, candidate.slots - 1u}) {
        if (!valid[end]) continue;
        const double corner = end == 0 ? static_cast<double>(candidate.span_min) : static_cast<double>(candidate.span_max);
        const double beyond = end == 0 ? corner - strip_reach : corner + strip_reach;
        const double l0 = std::min(corner, beyond), l1 = std::max(corner, beyond);
        const double outer = side == 0 ? static_cast<double>(edge) - strip_reach : static_cast<double>(edge) + strip_reach;
        const double t0 = std::min(outer, static_cast<double>(edge)), t1 = std::max(outer, static_cast<double>(edge));
        const Rect2 square = axis == 0 ? Rect2{t0, t1, l0, l1} : Rect2{l0, l1, t0, t1};
        double t_lo = 0, t_hi = 0;
        if (!TerrainRange(terrain, square.x0, square.x1, square.y0, square.y1, t_lo, t_hi)) continue;
        const auto p = point(corner, edge);
        const double plane_z = plane.Height(p[0], p[1]);
        if (std::max(t_hi - plane_z, plane_z - t_lo) <= step) valid_strips.push_back(square);
    }

    const double grow = radius + kRegionSlack;
    const auto cell_rect = [&](std::uint32_t slot, std::uint32_t c) {
        double a0 = 0, a1 = 0;
        along_range(slot, a0, a1);
        const double c0 = across0 + static_cast<double>(c) * cell;
        const double c1 = std::min(c0 + cell, across1);
        return axis == 0 ? Rect2{c0, c1, a0, a1} : Rect2{a0, a1, c0, c1};
    };
    // Per cell: the highest terrain the capsule region can stand on (the
    // step-zone floor together with the plane). No terrain data = blocked.
    std::vector<double> ceiling(static_cast<std::size_t>(cells), 0.0);
    for (std::uint32_t slot = 0; slot < candidate.slots; ++slot) {
        for (std::uint32_t c = 0; c < candidate.across; ++c) {
            const std::uint64_t bit = static_cast<std::uint64_t>(slot) * candidate.across + c;
            const Rect2 centres = cell_rect(slot, c);
            double t_lo = 0, t_hi = 0;
            if (!valid[slot] || !TerrainRange(terrain, centres.x0 - grow, centres.x1 + grow, centres.y0 - grow,
                                              centres.y1 + grow, t_lo, t_hi)) {
                SetBit(candidate.blocked, bit);
                continue;
            }
            ceiling[static_cast<std::size_t>(bit)] = t_hi;
        }
    }
    const Rect2 band = axis == 0 ? Rect2{across0, across1, candidate.span_min, candidate.span_max}
                                 : Rect2{candidate.span_min, candidate.span_max, across0, across1};
    const Rect2 area{band.x0 - grow, band.x1 + grow, band.y0 - grow, band.y1 + grow};
    const auto range = [&](double min, double max, double origin, std::uint32_t count, std::uint32_t& first,
                           std::uint32_t& last) {
        first = static_cast<std::uint32_t>(std::clamp(std::floor((min - grow - origin) / cell), 0.0,
                                                      static_cast<double>(count - 1)));
        last = static_cast<std::uint32_t>(std::clamp(std::floor((max + grow - origin) / cell), 0.0,
                                                     static_cast<double>(count - 1)));
    };
    std::array<HalfSpace, 8> region{};
    obstructions.ForEach(area, report.terrain_quads_tested, [&](const Triangle& triangle) {
        // Below the plane is below the upper envelope of every cell.
        if (EntirelyBelow(triangle, &plane, 1, static_cast<double>(profile.floor_contact_m))) return;
        std::uint32_t s0 = 0, s1 = 0, c0 = 0, c1 = 0;
        range(axis == 0 ? triangle.min_y : triangle.min_x, axis == 0 ? triangle.max_y : triangle.max_x,
              candidate.span_min, candidate.slots, s0, s1);
        range(axis == 0 ? triangle.min_x : triangle.min_y, axis == 0 ? triangle.max_x : triangle.max_y, across0,
              candidate.across, c0, c1);
        for (std::uint32_t slot = s0; slot <= s1; ++slot) {
            for (std::uint32_t c = c0; c <= c1; ++c) {
                const std::uint64_t bit = static_cast<std::uint64_t>(slot) * candidate.across + c;
                if (GetBit(candidate.blocked, bit)) continue;
                const std::array<Plane, 2> floors{plane, Plane{0.0, 0.0, ceiling[static_cast<std::size_t>(bit)], 1.0}};
                if (EntirelyBelow(triangle, floors.data(), floors.size(), static_cast<double>(profile.floor_contact_m))) {
                    continue;
                }
                const std::size_t n = BuildRegion(cell_rect(slot, c), radius, floors.data(), floors.size(), profile,
                                                  region.data());
                if (ClipNonEmpty(triangle, region.data(), n)) SetBit(candidate.blocked, bit);
            }
        }
    });
    // A side whose edge-adjacent cells are all blocked (a wall along it) can
    // never be crossed: no record.
    const std::uint32_t adjacent = side == 0 ? 0u : candidate.across - 1u;
    bool crossable = false;
    for (std::uint32_t slot = 0; slot < candidate.slots && !crossable; ++slot) {
        crossable = !GetBit(candidate.blocked, static_cast<std::uint64_t>(slot) * candidate.across + adjacent);
    }
    if (!crossable) return false;
    report.corridor_slots += cells;
    for (std::uint64_t bit = 0; bit < cells; ++bit) {
        report.corridor_slots_blocked += GetBit(candidate.blocked, bit) ? 1u : 0u;
    }
    out = std::move(candidate);
    support = std::move(valid_strips);
    return true;
}

// ---- 3D-5D open-ledge erosion ----------------------------------------------

// Every point of `area` lies in the union of the closed `support` rectangles.
// Exact for axis-aligned rectangles: the area is split at every support edge
// inside it and the midpoint of each piece must be supported.
bool Covered(const Rect2& area, const std::vector<Rect2>& support)
{
    std::vector<double> xs{area.x0, area.x1};
    std::vector<double> ys{area.y0, area.y1};
    for (const auto& r : support) {
        for (const double x : {r.x0, r.x1}) {
            if (x > area.x0 && x < area.x1) xs.push_back(x);
        }
        for (const double y : {r.y0, r.y1}) {
            if (y > area.y0 && y < area.y1) ys.push_back(y);
        }
    }
    std::sort(xs.begin(), xs.end());
    xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
    std::sort(ys.begin(), ys.end());
    ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
    for (std::size_t i = 0; i + 1 < xs.size(); ++i) {
        const double mx = 0.5 * (xs[i] + xs[i + 1]);
        for (std::size_t j = 0; j + 1 < ys.size(); ++j) {
            const double my = 0.5 * (ys[j] + ys[j + 1]);
            const bool inside = std::any_of(support.begin(), support.end(), [&](const Rect2& r) {
                return mx >= r.x0 && mx <= r.x1 && my >= r.y0 && my <= r.y1;
            });
            if (!inside) return false;
        }
    }
    return true;
}

Rect2 FootprintOf(const LayerVolume& volume)
{
    return {volume.bounds.min_x, volume.bounds.max_x, volume.bounds.min_y, volume.bounds.max_y};
}

// Blocks every still-free cell whose capsule footprint square leaves the
// support of the actor (see CookLayerClearance). `terrain_support` holds the
// terrain strips beyond each volume's proven terrain-edge slots.
void ErodeOpenLedges(LayeredWorld& world, const LayerClearanceProfile& profile,
                     const std::vector<std::pair<VolumeId, Rect2>>& terrain_support, LayerClearanceReport& report)
{
    const double cell = static_cast<double>(profile.cell_size_m);
    const double radius = static_cast<double>(profile.actor_radius_m);
    const auto grown = [&](const Rect2& c) { return Rect2{c.x0 - radius, c.x1 + radius, c.y0 - radius, c.y1 + radius}; };
    const auto index_of = [&](VolumeId id) {
        for (std::size_t i = 0; i < world.volumes.size(); ++i) {
            if (world.volumes[i].id == id) return i;
        }
        return world.volumes.size();
    };

    std::vector<std::vector<Rect2>> support(world.volumes.size());
    for (std::size_t i = 0; i < world.volumes.size(); ++i) {
        const auto& volume = world.volumes[i];
        if (!volume.clearance) continue;
        support[i].push_back(FootprintOf(volume));
        for (const auto& portal : world.portals) {
            if (!portal.proof) continue;
            VolumeId other = 0;
            if (portal.source_volume == volume.id) other = portal.target_volume;
            else if (portal.target_volume == volume.id) other = portal.source_volume;
            else continue;
            const std::size_t o = index_of(other);
            if (o < world.volumes.size()) support[i].push_back(FootprintOf(world.volumes[o]));
        }
        for (const auto& [owner, strip] : terrain_support) {
            if (owner == volume.id) support[i].push_back(strip);
        }
    }

    for (std::size_t i = 0; i < world.volumes.size(); ++i) {
        auto& volume = world.volumes[i];
        if (!volume.clearance) continue;
        auto& grid = *volume.clearance;
        for (std::uint32_t j = 0; j < grid.cells_y; ++j) {
            for (std::uint32_t c = 0; c < grid.cells_x; ++c) {
                const std::uint64_t bit = static_cast<std::uint64_t>(j) * grid.cells_x + c;
                if (GetBit(grid.blocked, bit) || Covered(grown(CellRect(volume.bounds, cell, c, j)), support[i])) continue;
                SetBit(grid.blocked, bit);
                ++report.cells_eroded;
                ++report.cells_blocked;
            }
        }
    }

    for (auto& portal : world.portals) {
        if (!portal.proof) continue;
        const std::size_t ia = index_of(portal.source_volume), ib = index_of(portal.target_volume);
        if (ia >= world.volumes.size() || ib >= world.volumes.size()) continue;
        auto& proof = *portal.proof;
        std::vector<Rect2> both = support[ia];
        both.insert(both.end(), support[ib].begin(), support[ib].end());
        double across0 = 0.0, across1 = 0.0;
        LayerPortalCorridorAcross(proof, world.volumes[ia], world.volumes[ib], profile, across0, across1);
        for (std::uint32_t slot = 0; slot < proof.slots; ++slot) {
            const double along0 = static_cast<double>(proof.span_min) + static_cast<double>(slot) * cell;
            const double along1 = std::min(along0 + cell, static_cast<double>(proof.span_max));
            for (std::uint32_t c = 0; c < proof.across; ++c) {
                const std::uint64_t bit = static_cast<std::uint64_t>(slot) * proof.across + c;
                if (GetBit(proof.blocked, bit)) continue;
                const double c0 = across0 + static_cast<double>(c) * cell;
                const double c1 = std::min(c0 + cell, across1);
                const Rect2 rect = proof.axis == 0 ? Rect2{c0, c1, along0, along1} : Rect2{along0, along1, c0, c1};
                if (Covered(grown(rect), both)) continue;
                SetBit(proof.blocked, bit);
                ++report.corridor_slots_eroded;
                ++report.corridor_slots_blocked;
            }
        }
    }

    for (auto& edge : world.terrain_edges) {
        const std::size_t iv = index_of(edge.volume_id);
        if (iv >= world.volumes.size()) continue;
        double across0 = 0.0, across1 = 0.0;
        LayerTerrainEdgeCorridorAcross(edge, world.volumes[iv], profile, across0, across1);
        for (std::uint32_t slot = 0; slot < edge.slots; ++slot) {
            const double along0 = static_cast<double>(edge.span_min) + static_cast<double>(slot) * cell;
            const double along1 = std::min(along0 + cell, static_cast<double>(edge.span_max));
            for (std::uint32_t c = 0; c < edge.across; ++c) {
                const std::uint64_t bit = static_cast<std::uint64_t>(slot) * edge.across + c;
                if (GetBit(edge.blocked, bit)) continue;
                const double c0 = across0 + static_cast<double>(c) * cell;
                const double c1 = std::min(c0 + cell, across1);
                const Rect2 rect = edge.axis == 0 ? Rect2{c0, c1, along0, along1} : Rect2{along0, along1, c0, c1};
                if (Covered(grown(rect), support[iv])) continue;
                SetBit(edge.blocked, bit);
                ++report.corridor_slots_eroded;
                ++report.corridor_slots_blocked;
            }
        }
    }
}

} // namespace

bool CookLayerClearance(LayeredWorld& world,
                        const Rect& world_bounds,
                        const std::vector<LayerObstructionMesh>& obstructions,
                        const LayerTerrainObstruction* terrain,
                        const LayerClearanceProfile& profile,
                        LayerClearanceReport& report)
{
    report = {};
    const auto strip_proofs = [&] {
        for (auto& volume : world.volumes) volume.clearance.reset();
        world.portals.erase(std::remove_if(world.portals.begin(), world.portals.end(),
                                           [](const LayerPortal& portal) { return portal.proof.has_value(); }),
                            world.portals.end());
        world.terrain_edges.clear();
        world.clearance_profile.reset();
    };
    const auto fail = [&](const std::string& message) {
        report.errors.push_back(message);
        strip_proofs();
        return false;
    };
    strip_proofs();
    if (!profile.Valid()) return fail("clearance profile is invalid (cell 0.05..4 m, radius > 0, height >= 2 * radius)");
    std::string error;
    if (!world.Validate(world_bounds, error)) return fail("layered world must validate before clearance cooking: " + error);
    std::size_t triangle_count = 0;
    for (const auto& mesh : obstructions) {
        if (mesh.indices.size() % 3 != 0) return fail("obstruction " + std::to_string(mesh.source_id) + " is not a triangle list");
        for (const auto& vertex : mesh.vertices) {
            if (!std::isfinite(vertex[0]) || !std::isfinite(vertex[1]) || !std::isfinite(vertex[2])) {
                return fail("obstruction " + std::to_string(mesh.source_id) + " has non-finite vertices");
            }
        }
        for (const auto index : mesh.indices) {
            if (index >= mesh.vertices.size()) return fail("obstruction " + std::to_string(mesh.source_id) + " index out of range");
        }
        triangle_count += mesh.indices.size() / 3;
        if (triangle_count > kMaxLayerObstructionTriangles) return fail("obstruction geometry exceeds the bounded triangle limit");
    }
    if (terrain != nullptr) {
        const auto& t = *terrain;
        const std::uint64_t samples = (static_cast<std::uint64_t>(t.cells_x) + 1) * (static_cast<std::uint64_t>(t.cells_y) + 1);
        if (t.cells_x == 0 || t.cells_y == 0 || !std::isfinite(t.cell_size) || t.cell_size <= 0 ||
            !std::isfinite(t.origin_x) || !std::isfinite(t.origin_y) || t.heights.size() != samples ||
            std::any_of(t.heights.begin(), t.heights.end(), [](float h) { return !std::isfinite(h); })) {
            return fail("terrain obstruction needs a complete finite height grid");
        }
    }
    std::uint64_t total = 0;
    for (const auto& volume : world.volumes) {
        if (!volume.ground_support || !volume.HasValidGroundSupport()) continue;
        const std::uint64_t cells =
            static_cast<std::uint64_t>(LayerClearanceCellCount(volume.bounds.min_x, volume.bounds.max_x, profile.cell_size_m)) *
            LayerClearanceCellCount(volume.bounds.min_y, volume.bounds.max_y, profile.cell_size_m);
        if (cells == 0 || cells > kMaxLayerClearanceCellsPerVolume) {
            return fail("volume " + std::to_string(volume.id) + " needs " + std::to_string(cells) +
                        " clearance cells; split the floor or use a larger cell size");
        }
        total += cells;
        if (total > kMaxLayerClearanceTotalCells) return fail("clearance grids exceed the total cell limit");
    }

    const ObstructionSet set(obstructions, terrain);
    report.obstruction_triangles = set.MeshTriangles();
    for (auto& volume : world.volumes) {
        if (!volume.ground_support || !volume.HasValidGroundSupport()) continue;
        CookVolume(volume, set, profile, report);
    }
    if (report.volumes_with_clearance == 0) {
        report.warnings.push_back("no volume has proven ground support; no clearance was cooked");
        return true;
    }

    std::uint32_t next_id = 0;
    for (const auto& portal : world.portals) next_id = std::max(next_id, portal.id);
    std::vector<LayerPortal> derived;
    for (std::size_t i = 0; i < world.volumes.size(); ++i) {
        const auto& a = world.volumes[i];
        if (!a.clearance) continue;
        for (std::size_t j = i + 1; j < world.volumes.size(); ++j) {
            const auto& b = world.volumes[j];
            if (!b.clearance) continue;
            for (std::uint8_t axis = 0; axis < 2; ++axis) {
                LayerPortalProof proof;
                if (!DerivePortal(a, b, axis, profile, proof, report)) continue;
                if (next_id == std::numeric_limits<std::uint32_t>::max()) return fail("portal id space exhausted");
                if (world.portals.size() + derived.size() >= kMaxLayeredWorldPortals) return fail("derived portals exceed the portal limit");
                CookCorridor(proof, a, b, set, profile, report);
                LayerPortal portal;
                portal.id = ++next_id;
                portal.source_volume = a.id;
                portal.target_volume = b.id;
                portal.source_bounds = Strip(a, proof, profile);
                portal.target_bounds = Strip(b, proof, profile);
                portal.source_min_z = a.min_z;
                portal.source_max_z = a.max_z;
                portal.target_min_z = b.min_z;
                portal.target_max_z = b.max_z;
                portal.bidirectional = true;
                portal.proof = std::move(proof);
                derived.push_back(std::move(portal));
            }
        }
    }
    report.portals_derived = derived.size();
    for (auto& portal : derived) world.portals.push_back(std::move(portal));
    std::vector<std::pair<VolumeId, Rect2>> terrain_support;
    if (terrain != nullptr) {
        std::uint32_t edge_id = 0;
        for (const auto& volume : world.volumes) {
            if (!volume.clearance || !volume.AllowsGroundMovement()) continue;
            for (std::uint8_t axis = 0; axis < 2; ++axis) {
                for (std::uint8_t side = 0; side < 2; ++side) {
                    LayerTerrainEdge edge;
                    std::vector<Rect2> strips;
                    if (!CookTerrainEdge(world, volume, axis, side, world_bounds, *terrain, set, profile, edge, strips,
                                         report)) {
                        continue;
                    }
                    if (world.terrain_edges.size() >= kMaxLayerTerrainEdges) return fail("terrain edges exceed the limit");
                    edge.id = ++edge_id;
                    world.terrain_edges.push_back(std::move(edge));
                    for (const auto& strip : strips) terrain_support.emplace_back(volume.id, strip);
                }
            }
        }
        report.terrain_edges_derived = world.terrain_edges.size();
    }
    ErodeOpenLedges(world, profile, terrain_support, report);
    world.clearance_profile = profile;
    if (!world.Validate(world_bounds, error)) return fail("cooked clearance failed validation: " + error);
    if (EncodeLayeredWorld(world).empty()) return fail("cooked clearance does not fit the layered-world sidecar limits");
    return true;
}

} // namespace mx::map
