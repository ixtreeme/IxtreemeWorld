// 3D-4B: cooked clearance grids and proven step/ramp portals.
//
// Fixtures are analytic boxes / wedges in canonical metres (Z up). The
// soundness oracle is independent of the cooker: for sampled capsule centres
// in every PASSABLE cell it clips each obstruction triangle to the vertical
// band the capsule may use and measures the exact 2D distance from the
// centre to the clipped polygon (a disk, not the cooker's square region).
#include "map/LayerActorMovement.h"
#include "map/LayerClearance.h"
#include "map/LayerGroundSupport.h"
#include "map/LayeredWorldGeometry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace {
using namespace mx::map;

int failures = 0;
int checks = 0;

void Check(const std::string& name, bool condition, const std::string& detail = {})
{
    ++checks;
    std::cout << "LAYER CLEARANCE " << name << ": " << (condition ? "PASS" : "FAIL");
    if (!detail.empty()) std::cout << " (" << detail << ")";
    std::cout << '\n';
    if (!condition) ++failures;
}

using V3 = std::array<float, 3>;

LayerCollisionMesh Mesh(std::uint32_t id, std::vector<V3> vertices, std::vector<std::uint32_t> indices,
                        std::uint32_t tags = VolumeTagGround)
{
    LayerCollisionMesh mesh;
    mesh.source_id = id;
    mesh.name = "mesh_" + std::to_string(id);
    mesh.tags = tags;
    mesh.supports_ground_movement = true;
    mesh.vertices = std::move(vertices);
    mesh.indices = std::move(indices);
    return mesh;
}

// Closed box, outward winding (top face +Z).
LayerCollisionMesh Box(std::uint32_t id, float x0, float y0, float z0, float x1, float y1, float z1,
                       std::uint32_t tags = VolumeTagGround)
{
    return Mesh(id,
                {{x0, y0, z0}, {x1, y0, z0}, {x0, y1, z0}, {x1, y1, z0},
                 {x0, y0, z1}, {x1, y0, z1}, {x0, y1, z1}, {x1, y1, z1}},
                {4, 5, 7, 4, 7, 6, 0, 2, 3, 0, 3, 1, 0, 1, 5, 0, 5, 4,
                 2, 6, 7, 2, 7, 3, 0, 4, 6, 0, 6, 2, 1, 3, 7, 1, 7, 5},
                tags);
}

// Solid ramp rising along +X from (x0, z=base) to (x1, z=top) over [y0, y1).
LayerCollisionMesh Wedge(std::uint32_t id, float x0, float x1, float y0, float y1, float base, float top,
                         std::uint32_t tags = VolumeTagGround)
{
    // A B C D bottom; E F top back edge.
    return Mesh(id,
                {{x0, y0, base}, {x1, y0, base}, {x1, y1, base}, {x0, y1, base}, {x1, y0, top}, {x1, y1, top}},
                {0, 4, 5, 0, 5, 3, 0, 3, 2, 0, 2, 1, 1, 2, 5, 1, 5, 4, 0, 1, 4, 3, 5, 2},
                tags);
}

LayerObstructionMesh Obstruct(const LayerCollisionMesh& mesh)
{
    return {mesh.source_id, mesh.vertices, mesh.indices};
}

struct Scene {
    std::vector<LayerCollisionMesh> layers;   // opted-in: produce volumes
    std::vector<LayerCollisionMesh> blockers; // physical only: never volumes
    Rect bounds{0, 0, 64, 64};
    std::optional<LayerTerrainObstruction> terrain;
};

struct Built {
    bool ok = false;
    LayeredWorld world;
    LayerClearanceReport report;
    std::string error;
    std::vector<LayerObstructionMesh> obstructions;
};

Built Build(const Scene& scene, const LayerClearanceProfile& profile = {}, bool cook = true)
{
    Built built;
    std::vector<LayerSourceSurface> surfaces;
    LayerGeometryReport geometry;
    if (!ExtractLayerSourceSurfaces(scene.layers, {}, surfaces, geometry)) {
        built.error = geometry.errors.empty() ? "extract" : geometry.errors.front();
        return built;
    }
    LayerGenerationOptions options;
    options.world_bounds = scene.bounds;
    options.require_exact_footprints = true;
    LayerGenerationReport generation;
    if (!GenerateLayeredWorld(surfaces, options, built.world, generation)) {
        built.error = generation.errors.empty() ? "generate" : generation.errors.front();
        return built;
    }
    for (const auto& mesh : scene.layers) built.obstructions.push_back(Obstruct(mesh));
    for (const auto& mesh : scene.blockers) built.obstructions.push_back(Obstruct(mesh));
    if (!cook) {
        built.ok = true;
        return built;
    }
    built.ok = CookLayerClearance(built.world, scene.bounds, built.obstructions,
                                  scene.terrain ? &*scene.terrain : nullptr, profile, built.report);
    if (!built.ok) built.error = built.report.errors.empty() ? "cook" : built.report.errors.front();
    return built;
}

const LayerVolume* VolumeAt(const LayeredWorld& world, double x, double y, double z)
{
    for (const auto& volume : world.volumes) {
        if (volume.Contains(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z))) return &volume;
    }
    return nullptr;
}

// Walkable volume whose half-open footprint contains (x, y), evaluated in
// double (the fixtures' walkable footprints do not overlap in XY).
const LayerVolume* FootprintAt(const LayeredWorld& world, double x, double y)
{
    for (const auto& volume : world.volumes) {
        if (volume.ground_support && x >= volume.bounds.min_x && x < volume.bounds.max_x &&
            y >= volume.bounds.min_y && y < volume.bounds.max_y) {
            return &volume;
        }
    }
    return nullptr;
}

double CellDistanceToRect(double cx0, double cx1, double cy0, double cy1, double rx0, double rx1, double ry0, double ry1)
{
    const double dx = std::max({0.0, rx0 - cx1, cx0 - rx1});
    const double dy = std::max({0.0, ry0 - cy1, cy0 - ry1});
    return std::hypot(dx, dy);
}

// ---------------- independent oracle ----------------
using P3 = std::array<double, 3>;

struct OracleTri {
    std::array<P3, 3> v;
};

std::vector<OracleTri> Triangles(const std::vector<LayerObstructionMesh>& meshes, const LayerTerrainObstruction* terrain)
{
    std::vector<OracleTri> out;
    for (const auto& mesh : meshes) {
        for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            OracleTri t;
            for (int k = 0; k < 3; ++k) {
                const auto& p = mesh.vertices[mesh.indices[i + k]];
                t.v[k] = {p[0], p[1], p[2]};
            }
            out.push_back(t);
        }
    }
    if (terrain != nullptr) {
        const auto& t = *terrain;
        const std::size_t stride = t.cells_x + 1;
        for (std::uint32_t j = 0; j < t.cells_y; ++j) {
            for (std::uint32_t i = 0; i < t.cells_x; ++i) {
                auto s = [&](std::uint32_t si, std::uint32_t sj) {
                    return P3{t.origin_x + si * t.cell_size, t.origin_y + sj * t.cell_size, t.heights[sj * stride + si]};
                };
                // The oracle uses ONE physical diagonal; the cooker tests both.
                out.push_back({{s(i, j), s(i + 1, j), s(i + 1, j + 1)}});
                out.push_back({{s(i, j), s(i + 1, j + 1), s(i, j + 1)}});
            }
        }
    }
    return out;
}

// Keep the part of polygon `poly` with f(p) >= 0, f linear.
template <class F>
std::vector<P3> KeepAbove(const std::vector<P3>& poly, F f)
{
    std::vector<P3> out;
    for (std::size_t i = 0; i < poly.size(); ++i) {
        const P3& a = poly[i];
        const P3& b = poly[(i + 1) % poly.size()];
        const double fa = f(a), fb = f(b);
        if (fa >= 0) out.push_back(a);
        if ((fa >= 0) != (fb >= 0)) {
            const double t = fa / (fa - fb);
            out.push_back({a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1]), a[2] + t * (b[2] - a[2])});
        }
    }
    return out;
}

double SegmentDistance2D(double px, double py, const P3& a, const P3& b)
{
    const double dx = b[0] - a[0], dy = b[1] - a[1];
    const double len = dx * dx + dy * dy;
    double t = len > 0 ? ((px - a[0]) * dx + (py - a[1]) * dy) / len : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    return std::hypot(px - (a[0] + t * dx), py - (a[1] + t * dy));
}

double PolygonDistance2D(double px, double py, const std::vector<P3>& poly)
{
    if (poly.empty()) return 1e300;
    if (poly.size() >= 3) {
        // Point inside a convex polygon: all cross products share one sign.
        int sign = 0;
        bool inside = true;
        for (std::size_t i = 0; i < poly.size(); ++i) {
            const P3& a = poly[i];
            const P3& b = poly[(i + 1) % poly.size()];
            const double cross = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
            if (std::abs(cross) < 1e-15) continue;
            const int s = cross > 0 ? 1 : -1;
            if (sign == 0) sign = s;
            else if (s != sign) { inside = false; break; }
        }
        if (inside && sign != 0) return 0.0;
    }
    double best = 1e300;
    for (std::size_t i = 0; i < poly.size(); ++i) {
        best = std::min(best, SegmentDistance2D(px, py, poly[i], poly[(i + 1) % poly.size()]));
    }
    return best;
}

// Envelope planes: the volume's own plane, or (inside a step zone) the upper
// envelope of the portal's two planes.
struct OraclePlane {
    double sx, sy, k, sec;
    double H(double x, double y) const { return sx * x + sy * y + k; }
};

OraclePlane PlaneOf(const LayerSupportPlane& s)
{
    return {s.slope_x, s.slope_y, s.anchor_z - s.slope_x * s.anchor_x - s.slope_y * s.anchor_y,
            std::sqrt(1 + s.slope_x * s.slope_x + s.slope_y * s.slope_y)};
}

bool OracleFree(const std::vector<OraclePlane>& planes, const LayerClearanceProfile& profile, double px, double py,
                const std::vector<OracleTri>& tris, double& worst)
{
    double top = -1e300, sec = 1;
    for (const auto& plane : planes) {
        top = std::max(top, plane.H(px, py));
        sec = std::max(sec, plane.sec);
    }
    top += profile.actor_height_m + profile.actor_radius_m * (sec - 1.0);
    for (const auto& tri : tris) {
        std::vector<P3> poly(tri.v.begin(), tri.v.end());
        for (const auto& plane : planes) {
            poly = KeepAbove(poly, [&](const P3& p) { return p[2] - plane.H(p[0], p[1]) - profile.floor_contact_m; });
            if (poly.empty()) break;
        }
        if (poly.empty()) continue;
        poly = KeepAbove(poly, [&](const P3& p) { return top - p[2]; });
        if (poly.empty()) continue;
        const double d = PolygonDistance2D(px, py, poly);
        worst = std::min(worst, d);
        if (d <= profile.actor_radius_m) return false;
    }
    return true;
}

// Samples 5 x 5 centres of every clear cell of every volume, and of every
// clear corridor cell (step zone, upper envelope), against the oracle.
struct OracleResult {
    std::uint64_t samples = 0;
    std::uint64_t violations = 0;
    double closest = 1e300;
};

OracleResult RunOracle(const Built& built, const LayerTerrainObstruction* terrain,
                        const LayerClearanceProfile* actor_override = nullptr)
{
    OracleResult result;
    const auto& profile = actor_override ? *actor_override : *built.world.clearance_profile;
    const auto tris = Triangles(built.obstructions, terrain);
    const double g = profile.cell_size_m;
    for (const auto& volume : built.world.volumes) {
        if (!volume.clearance) continue;
        const auto plane = PlaneOf(*volume.ground_support);
        for (std::uint32_t j = 0; j < volume.clearance->cells_y; ++j) {
            for (std::uint32_t i = 0; i < volume.clearance->cells_x; ++i) {
                if (volume.clearance->Blocked(i, j)) continue;
                const double x0 = volume.bounds.min_x + i * g, y0 = volume.bounds.min_y + j * g;
                const double x1 = std::min(x0 + g, static_cast<double>(volume.bounds.max_x));
                const double y1 = std::min(y0 + g, static_cast<double>(volume.bounds.max_y));
                for (int a = 0; a <= 4; ++a) {
                    for (int b = 0; b <= 4; ++b) {
                        ++result.samples;
                        if (!OracleFree({plane}, profile, x0 + (x1 - x0) * a / 4, y0 + (y1 - y0) * b / 4, tris,
                                        result.closest)) {
                            ++result.violations;
                        }
                    }
                }
            }
        }
    }
    for (const auto& portal : built.world.portals) {
        if (!portal.proof) continue;
        const auto& proof = *portal.proof;
        const LayerVolume* a = nullptr;
        const LayerVolume* b = nullptr;
        for (const auto& v : built.world.volumes) {
            if (v.id == portal.source_volume) a = &v;
            if (v.id == portal.target_volume) b = &v;
        }
        double across0 = 0, across1 = 0;
        LayerPortalCorridorAcross(proof, *a, *b, profile, across0, across1);
        const std::vector<OraclePlane> planes{PlaneOf(*a->ground_support), PlaneOf(*b->ground_support)};
        for (std::uint32_t s = 0; s < proof.slots; ++s) {
            for (std::uint32_t c = 0; c < proof.across; ++c) {
                if (proof.CellBlocked(s, c)) continue;
                const double l0 = proof.span_min + s * g, l1 = std::min(l0 + g, static_cast<double>(proof.span_max));
                const double c0 = across0 + c * g, c1 = std::min(c0 + g, across1);
                for (int u = 0; u <= 4; ++u) {
                    for (int w = 0; w <= 4; ++w) {
                        const double along = l0 + (l1 - l0) * u / 4, acr = c0 + (c1 - c0) * w / 4;
                        ++result.samples;
                        if (!OracleFree(planes, profile, proof.axis == 0 ? acr : along, proof.axis == 0 ? along : acr,
                                        tris, result.closest)) {
                            ++result.violations;
                        }
                    }
                }
            }
        }
    }
    return result;
}

std::uint64_t Bits(double value)
{
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

LayerGroundState StateOf(const LayerGroundResult& result)
{
    return result.state;
}

std::uint32_t Version(const std::vector<std::uint8_t>& bytes)
{
    return bytes.size() < 8 ? 0 : static_cast<std::uint32_t>(bytes[4] | (bytes[5] << 8) | (bytes[6] << 16) | (bytes[7] << 24));
}

} // namespace

int main()
{
    const LayerClearanceProfile profile; // 0.25 m cells, r 0.35, H 1.8, step 0.35, contact 0.02
    const LayerActorProfile actor;       // same capsule
    const double r = profile.actor_radius_m;
    std::uint64_t oracle_samples = 0, oracle_violations = 0;
    double oracle_closest = 1e300;
    auto oracle = [&](const std::string& fixture, const Built& built, const LayerTerrainObstruction* terrain = nullptr) {
        const auto result = RunOracle(built, terrain);
        oracle_samples += result.samples;
        oracle_violations += result.violations;
        oracle_closest = std::min(oracle_closest, result.closest);
        Check("oracle-" + fixture, result.samples > 0 && result.violations == 0,
              std::to_string(result.samples) + " centres, violations=" + std::to_string(result.violations));
    };

    // ---------- 1. flat floor ----------
    {
        Scene scene;
        scene.layers.push_back(Box(1, 0, 0, -1, 16, 16, 0));
        const auto built = Build(scene);
        const auto* floor = built.ok ? VolumeAt(built.world, 8, 8, 0) : nullptr;
        Check("flat-floor-all-clear", floor && floor->clearance && floor->clearance->cells_x == 64 &&
              floor->clearance->cells_y == 64 && floor->clearance->BlockedCount() == 0, built.error);
        Check("flat-floor-no-portals", built.ok && built.world.portals.empty());
    }

    // ---------- 2. wall, beam, ceilings, floor contact ----------
    Scene room;
    room.layers.push_back(Box(1, 0, 0, -1, 16, 16, 0));
    room.blockers.push_back(Box(10, 8.0f, 0, 0, 8.2f, 6, 3));          // wall, not opted in
    room.blockers.push_back(Box(11, 2, 8, 1.2f, 4, 16, 1.5f));          // beam under head height
    room.blockers.push_back(Box(12, 10, 8, 2.5f, 14, 12, 2.8f));        // high ceiling slab
    room.blockers.push_back(Box(13, 10, 12.5f, 1.79f, 14, 16, 2.0f));   // ceiling just inside height
    room.blockers.push_back(Box(14, 5, 8, 0, 6, 9, 0.01f));             // rug (floor contact)
    room.blockers.push_back(Box(15, 5, 11, 0, 6, 12, 0.05f));           // 5 cm box
    const auto roomBuilt = Build(room);
    const LayerVolume* floor = roomBuilt.ok ? VolumeAt(roomBuilt.world, 1, 1, 0) : nullptr;
    Check("room-cooks", roomBuilt.ok && floor && floor->clearance, roomBuilt.error);
    if (floor && floor->clearance) {
        const auto& grid = *floor->clearance;
        const double g = profile.cell_size_m;
        std::size_t wrong_clear = 0, wrong_blocked = 0, near_checked = 0;
        for (std::uint32_t j = 0; j < grid.cells_y; ++j) {
            for (std::uint32_t i = 0; i < grid.cells_x; ++i) {
                const double x0 = i * g, x1 = x0 + g, y0 = j * g, y1 = y0 + g;
                if (y1 > 7.5) continue; // away from the beam, rug and boxes (y >= 8)
                // Full-height wall: blocked exactly when the cell expanded by
                // the radius (a square, the cooker's region) reaches it; the
                // Euclidean distance bounds it from below.
                const double gx = std::max({0.0, 8.0 - x1, x0 - 8.2});
                const double gy = std::max({0.0, 0.0 - y1, y0 - 6.0});
                const double chebyshev = std::max(gx, gy);
                const double euclid = CellDistanceToRect(x0, x1, y0, y1, 8.0, 8.2, 0, 6);
                if (euclid < r - 1e-4 && !grid.Blocked(i, j)) ++wrong_clear;
                if (chebyshev > r + 1e-4 && grid.Blocked(i, j)) ++wrong_blocked;
                ++near_checked;
            }
        }
        Check("wall-cells-within-radius-blocked", wrong_clear == 0, "cells=" + std::to_string(near_checked));
        Check("wall-cells-beyond-square-reach-clear", wrong_blocked == 0, "wrongly blocked=" + std::to_string(wrong_blocked));
        auto cell = [&](double x, double y) {
            return grid.Blocked(static_cast<std::uint32_t>(x / g), static_cast<std::uint32_t>(y / g));
        };
        Check("beam-under-head-height-blocks", cell(3, 12) && cell(1.75, 12) && !cell(1.25, 12));
        Check("ceiling-above-head-height-clear", !cell(12, 10));
        Check("ceiling-inside-head-height-blocks", cell(12, 14));
        Check("floor-contact-rug-ignored", !cell(5.5, 8.5));
        Check("ankle-box-blocks", cell(5.5, 11.5));
        oracle("room", roomBuilt);

        const auto place = [&](double x, double y) { return ResolveLayerActorPlacement(roomBuilt.world, actor, floor->id, x, y); };
        const auto start = place(4, 3);
        Check("placement-clear-ok", start.Ok() && start.state.z == 0.0);
        Check("placement-inside-wall-margin-blocked", place(8.3, 3).status == GroundSupportStatus::Blocked &&
              place(7.8, 3).status == GroundSupportStatus::Blocked);
        const auto across = ResolveLayerActorMove(roomBuilt.world, actor, start.state, floor->id, 12, 3);
        Check("move-across-wall-blocked-no-tunnelling", across.status == GroundSupportStatus::Blocked &&
              across.state.x == 4.0 && across.state.y == 3.0);
        const auto around = ResolveLayerActorMove(roomBuilt.world, actor, start.state, floor->id, 4, 7);
        const auto past = around.Ok() ? ResolveLayerActorMove(roomBuilt.world, actor, around.state, floor->id, 12, 7) : around;
        Check("move-around-wall-end-ok", around.Ok() && past.Ok() && past.state.x == 12.0);
        LayerActorProfile big = actor;
        big.radius_m = 0.4f;
        LayerActorProfile tall = actor;
        tall.height_m = 2.0f;
        LayerActorProfile small = actor;
        small.radius_m = 0.3f;
        small.height_m = 1.6f;
        Check("actor-larger-than-profile-not-covered",
              ResolveLayerActorPlacement(roomBuilt.world, big, floor->id, 4, 3).status == GroundSupportStatus::ActorNotCovered &&
              ResolveLayerActorPlacement(roomBuilt.world, tall, floor->id, 4, 3).status == GroundSupportStatus::ActorNotCovered);
        Check("actor-smaller-than-profile-covered", ResolveLayerActorPlacement(roomBuilt.world, small, floor->id, 4, 3).Ok());
    }

    // ---------- 3. support-only world (3D-4A) is unchanged ----------
    {
        const auto built = Build(room, profile, false);
        const auto* v = built.ok ? VolumeAt(built.world, 1, 1, 0) : nullptr;
        const auto bytes = v ? EncodeLayeredWorld(built.world) : std::vector<std::uint8_t>{};
        Check("support-only-world-no-clearance-proof", v &&
              ResolveLayerActorPlacement(built.world, actor, v->id, 4, 3).status == GroundSupportStatus::NoClearanceProof);
        Check("support-only-3d4a-api-unchanged", v && ResolveLayerGroundPlacement(built.world, v->id, 8.1, 3).Ok());
        Check("support-only-world-encodes-mx3d-v3", Version(bytes) == 3);
    }

    // ---------- 4. stairs: floor -> three 0.25 m steps -> landing ----------
    Scene stairs;
    stairs.layers.push_back(Box(1, 0, 0, -1, 16, 4, 0));               // floor
    stairs.layers.push_back(Box(2, 16, 0, -1, 16.75f, 4, 0.25f));      // step 1
    stairs.layers.push_back(Box(3, 16.75f, 0, -1, 17.5f, 4, 0.5f));    // step 2
    stairs.layers.push_back(Box(4, 17.5f, 0, -1, 26, 4, 0.75f));       // landing
    const auto stairsBuilt = Build(stairs);
    {
        std::size_t proven = 0;
        double max_step = 0;
        for (const auto& portal : stairsBuilt.world.portals) {
            if (portal.proof) {
                ++proven;
                max_step = std::max(max_step, portal.proof->max_step_m);
            }
        }
        Check("stairs-derive-three-proven-portals", stairsBuilt.ok && proven == 3 && std::abs(max_step - 0.25) < 1e-9,
              stairsBuilt.error);
        oracle("stairs", stairsBuilt);
        const auto* f = VolumeAt(stairsBuilt.world, 8, 2, 0);
        const auto* landing = VolumeAt(stairsBuilt.world, 20, 2, 0.75);
        auto state = f ? ResolveLayerActorPlacement(stairsBuilt.world, actor, f->id, 14, 2) : LayerGroundResult{};
        std::size_t moves = 0, crossings = 0;
        bool walked = state.Ok();
        for (int k = 71; walked && k <= 110; ++k) {
            const double x = k * 0.2;
            const auto* target = FootprintAt(stairsBuilt.world, x, 2);
            const auto next = target ? ResolveLayerActorMove(stairsBuilt.world, actor, state.state, target->id, x, 2)
                                     : LayerGroundResult{};
            walked = next.Ok();
            if (!walked) {
                std::cout << "  stairs-up stopped at x=" << x << " target=" << (target ? target->id : 0)
                          << " from=" << state.state.volume_id << " status=" << ToString(next.status) << '\n';
            }
            crossings += next.portal_id != 0 ? 1 : 0;
            ++moves;
            if (walked) state = next;
        }
        Check("stairs-walk-up-in-0.2m-ticks", walked && landing && state.state.volume_id == landing->id &&
              crossings == 3 && std::abs(state.state.z - 0.75) < 1e-9,
              "moves=" + std::to_string(moves) + " crossings=" + std::to_string(crossings));
        // Walk back down the same way.
        std::size_t down_crossings = 0;
        for (int k = 109; walked && k >= 70; --k) {
            const double x = k * 0.2;
            const auto* target = FootprintAt(stairsBuilt.world, x, 2);
            const auto next = target ? ResolveLayerActorMove(stairsBuilt.world, actor, state.state, target->id, x, 2)
                                     : LayerGroundResult{};
            walked = next.Ok();
            down_crossings += next.portal_id != 0 ? 1 : 0;
            if (walked) state = next;
        }
        Check("stairs-walk-down-in-0.2m-ticks", walked && f && state.state.volume_id == f->id && down_crossings == 3 &&
              state.state.z == 0.0);
        const auto* s2 = VolumeAt(stairsBuilt.world, 17.2, 2, 0.5);
        const auto at_floor = f ? ResolveLayerActorPlacement(stairsBuilt.world, actor, f->id, 15.5, 2) : LayerGroundResult{};
        Check("stairs-two-edges-in-one-move-transition-required", s2 && at_floor.Ok() &&
              ResolveLayerActorMove(stairsBuilt.world, actor, at_floor.state, s2->id, 17.2, 2).status ==
                  GroundSupportStatus::TransitionRequired);
        const auto direct = landing && at_floor.Ok()
            ? ResolveLayerActorMove(stairsBuilt.world, actor, at_floor.state, landing->id, 20, 2) : LayerGroundResult{};
        Check("stairs-floor-to-landing-no-portal-transition-required",
              direct.status == GroundSupportStatus::TransitionRequired && direct.state.x == 15.5);
    }

    // ---------- 5. a step higher than the profile step ----------
    {
        Scene high;
        high.layers.push_back(Box(1, 0, 0, -1, 16, 4, 0));
        high.layers.push_back(Box(2, 16, 0, -1, 24, 4, 0.5f));
        const auto built = Build(high);
        const auto* f = VolumeAt(built.world, 8, 2, 0);
        const auto* up = VolumeAt(built.world, 20, 2, 0.5);
        const auto start = f ? ResolveLayerActorPlacement(built.world, actor, f->id, 15, 2) : LayerGroundResult{};
        Check("step-above-profile-no-portal", built.ok && built.world.portals.empty() && built.report.edges_rejected_step == 1);
        Check("step-above-profile-transition-required", up && start.Ok() &&
              ResolveLayerActorMove(built.world, actor, start.state, up->id, 17, 2).status ==
                  GroundSupportStatus::TransitionRequired);
    }

    // ---------- 6. wall standing on part of a shared step edge ----------
    {
        Scene walled = stairs;
        walled.bounds = {0, 0, 64, 64};
        walled.layers.resize(2); // floor + step 1 only, step 1 extended to be a platform
        walled.layers[1] = Box(2, 16, 0, -1, 24, 4, 0.25f);
        walled.blockers.push_back(Box(20, 15.9f, 0, 0, 16.1f, 2, 3)); // wall across y [0, 2)
        const auto built = Build(walled);
        const auto* f = VolumeAt(built.world, 8, 2, 0);
        const auto* p = VolumeAt(built.world, 20, 2, 0.25);
        const auto a = f ? ResolveLayerActorPlacement(built.world, actor, f->id, 15, 1) : LayerGroundResult{};
        const auto b = f ? ResolveLayerActorPlacement(built.world, actor, f->id, 15, 3.4) : LayerGroundResult{};
        const auto through = p && a.Ok() ? ResolveLayerActorMove(built.world, actor, a.state, p->id, 17, 1) : LayerGroundResult{};
        const auto beside = p && b.Ok() ? ResolveLayerActorMove(built.world, actor, b.state, p->id, 17, 3.4) : LayerGroundResult{};
        Check("wall-on-edge-blocks-crossing-behind-it", built.ok && through.status == GroundSupportStatus::Blocked,
              std::string(ToString(through.status)));
        Check("edge-beside-wall-still-crossable", beside.Ok() && beside.portal_id != 0 && std::abs(beside.state.z - 0.25) < 1e-9,
              std::string(ToString(beside.status)));
        oracle("walled-edge", built);
    }

    // ---------- 7. ramp (valley transition) to an upper floor ----------
    Scene ramp;
    ramp.layers.push_back(Box(1, 0, 0, -1, 16, 4, 0));
    ramp.layers.push_back(Wedge(2, 16, 22, 0, 4, 0, 3));
    ramp.layers.push_back(Box(3, 22, 0, 2, 30, 4, 3));
    const auto rampBuilt = Build(ramp);
    {
        std::size_t proven = 0;
        for (const auto& portal : rampBuilt.world.portals) proven += portal.proof ? 1 : 0;
        Check("ramp-derives-two-proven-portals", rampBuilt.ok && proven == 2, rampBuilt.error);
        const auto* f = VolumeAt(rampBuilt.world, 8, 2, 0);
        const auto* w = VolumeAt(rampBuilt.world, 19, 2, 1.5);
        std::uint64_t ramp_blocked_middle = 0;
        if (w && w->clearance) {
            // Cells at least radius + cell away from both ramp ends: only the
            // ramp's own side faces are near, and they lie below its plane.
            for (std::uint32_t j = 0; j < w->clearance->cells_y; ++j) {
                for (std::uint32_t i = 0; i < w->clearance->cells_x; ++i) {
                    const double x0 = 16 + i * 0.25;
                    if (x0 >= 16.75 && x0 + 0.25 <= 21.25) ramp_blocked_middle += w->clearance->Blocked(i, j) ? 1 : 0;
                }
            }
        }
        Check("ramp-own-side-faces-do-not-block", w && w->clearance && ramp_blocked_middle == 0);
        oracle("ramp", rampBuilt);
        auto state = f ? ResolveLayerActorPlacement(rampBuilt.world, actor, f->id, 14, 2) : LayerGroundResult{};
        bool walked = state.Ok();
        std::size_t crossings = 0;
        for (int k = 71; walked && k <= 130; ++k) {
            const double x = k * 0.2;
            const LayerVolume* target = FootprintAt(rampBuilt.world, x, 2);
            const auto next = target ? ResolveLayerActorMove(rampBuilt.world, actor, state.state, target->id, x, 2)
                                     : LayerGroundResult{};
            walked = next.Ok();
            crossings += next.portal_id != 0 ? 1 : 0;
            if (walked) state = next;
        }
        Check("ramp-walk-floor-to-upper-floor", walked && crossings == 2 && std::abs(state.state.z - 3.0) < 1e-9,
              "z=" + std::to_string(state.state.z));
    }

    // ---------- 8. authored (unproven) portal stays metadata ----------
    {
        Scene stacked;
        stacked.layers.push_back(Box(1, 0, 0, -1, 16, 16, 0));
        stacked.layers.push_back(Box(2, 0, 0, 5, 16, 16, 6, VolumeTagBuilding));
        auto built = Build(stacked, profile, false);
        const auto* ground = VolumeAt(built.world, 8, 8, 0);
        const auto* upper = VolumeAt(built.world, 8, 8, 6);
        if (ground && upper) {
            LayerPortal lift;
            lift.id = 7;
            lift.source_volume = ground->id;
            lift.target_volume = upper->id;
            lift.source_bounds = {7, 7, 9, 9};
            lift.target_bounds = {7, 7, 9, 9};
            lift.source_min_z = ground->min_z;
            lift.source_max_z = ground->max_z;
            lift.target_min_z = upper->min_z;
            lift.target_max_z = upper->max_z;
            built.world.portals.push_back(lift);
        }
        LayerClearanceReport report;
        const bool cooked = ground && upper &&
            CookLayerClearance(built.world, stacked.bounds, built.obstructions, nullptr, profile, report);
        const auto kept = std::find_if(built.world.portals.begin(), built.world.portals.end(),
                                       [](const LayerPortal& p) { return p.id == 7; });
        const auto start = cooked ? ResolveLayerActorPlacement(built.world, actor, ground->id, 8, 8) : LayerGroundResult{};
        Check("authored-portal-kept-unproven", cooked && kept != built.world.portals.end() && !kept->proof);
        Check("authored-portal-transition-required", start.Ok() &&
              ResolveLayerActorMove(built.world, actor, start.state, upper->id, 8, 8).status ==
                  GroundSupportStatus::TransitionRequired);
        // The upper slab (5..6 m) is above the 1.8 m capsule on the ground.
        Check("stacked-floor-above-head-height-clear", cooked && ground->clearance &&
              built.world.volumes[0].clearance && built.world.volumes[0].clearance->BlockedCount() == 0);
    }

    // ---------- 9. terrain ----------
    {
        Scene terrainScene;
        terrainScene.layers.push_back(Box(1, 0, 0, -1, 16, 16, 0));
        LayerTerrainObstruction t;
        t.origin_x = -4;
        t.origin_y = -4;
        t.cell_size = 2;
        t.cells_x = 12;
        t.cells_y = 12;
        t.heights.assign(13 * 13, -0.5f);  // below the floor everywhere...
        t.heights[(6) * 13 + 7] = 0.6f;    // ...except one hill sample at (10, 8)
        terrainScene.terrain = t;
        const auto built = Build(terrainScene);
        const auto* v = built.ok ? VolumeAt(built.world, 1, 1, 0) : nullptr;
        const auto blocked = [&](double x, double y) {
            return v->clearance->Blocked(static_cast<std::uint32_t>(x / 0.25), static_cast<std::uint32_t>(y / 0.25));
        };
        Check("terrain-hill-through-floor-blocks", v && v->clearance && blocked(10, 8), built.error);
        Check("terrain-below-floor-clear", v && v->clearance && !blocked(2, 2) && !blocked(14, 14));
        Check("terrain-quads-tested", built.report.terrain_quads_tested > 0);
        if (built.ok) oracle("terrain", built, &*terrainScene.terrain);
    }

    // ---------- 10. codec, validation, determinism ----------
    {
        const auto bytes = EncodeLayeredWorld(stairsBuilt.world);
        LayeredWorld decoded;
        std::string error;
        const bool decoded_ok = DecodeLayeredWorld(bytes, decoded, error) && decoded.Validate(stairs.bounds, error);
        Check("v4-roundtrip-bit-exact", Version(bytes) == 4 && decoded_ok && EncodeLayeredWorld(decoded) == bytes, error);
        auto trailing = bytes;
        trailing.push_back(0);
        LayeredWorld scratch;
        Check("v4-trailing-byte-rejected", !DecodeLayeredWorld(trailing, scratch, error));
        auto truncated = bytes;
        truncated.pop_back();
        Check("v4-truncated-rejected", !DecodeLayeredWorld(truncated, scratch, error));
        auto tamper = [&](auto mutate) {
            LayeredWorld copy = decoded;
            mutate(copy);
            std::string why;
            return !copy.Validate(stairs.bounds, why);
        };
        Check("grid-size-mismatch-rejected", tamper([](LayeredWorld& w) {
            for (auto& v : w.volumes) if (v.clearance) { ++v.clearance->cells_x; break; }
        }));
        {
            // 65 x 17 = 1105 cells: the last bitset byte has 7 padding bits.
            Scene odd;
            odd.layers.push_back(Box(1, 0, 0, -1, 16.1f, 4.1f, 0));
            auto oddBuilt = Build(odd);
            const auto* v = oddBuilt.ok ? VolumeAt(oddBuilt.world, 1, 1, 0) : nullptr;
            const bool shaped = v && v->clearance && v->clearance->cells_x == 65 && v->clearance->cells_y == 17;
            std::string why;
            const bool valid_before = shaped && oddBuilt.world.Validate(odd.bounds, why);
            if (shaped) oddBuilt.world.volumes[0].clearance->blocked.back() |= 0x80;
            Check("grid-padding-bit-rejected", valid_before && !oddBuilt.world.Validate(odd.bounds, why) &&
                  EncodeLayeredWorld(oddBuilt.world).empty());
        }
        Check("portal-step-tamper-rejected", tamper([](LayeredWorld& w) {
            for (auto& p : w.portals) if (p.proof) { p.proof->max_step_m = 0.2; break; }
        }));
        Check("portal-step-above-profile-rejected", tamper([](LayeredWorld& w) {
            for (auto& p : w.portals) if (p.proof) { p.proof->max_step_m = 0.4; break; }
        }));
        Check("portal-edge-tamper-rejected", tamper([](LayeredWorld& w) {
            for (auto& p : w.portals) if (p.proof) { p.proof->edge += 0.25f; break; }
        }));
        Check("portal-corridor-size-tamper-rejected", tamper([](LayeredWorld& w) {
            for (auto& p : w.portals) if (p.proof) { ++p.proof->across; break; }
        }));
        Check("profile-without-data-rejected", tamper([](LayeredWorld& w) {
            for (auto& v : w.volumes) v.clearance.reset();
            for (auto& p : w.portals) p.proof.reset();
        }));
        Check("data-without-profile-rejected", tamper([](LayeredWorld& w) { w.clearance_profile.reset(); }));
        Check("clearance-without-support-rejected", tamper([](LayeredWorld& w) {
            for (auto& v : w.volumes) if (v.clearance) { v.ground_support.reset(); break; }
        }));
        auto bad_profile = bytes;
        const float nan = std::numeric_limits<float>::quiet_NaN();
        std::memcpy(bad_profile.data() + 16, &nan, sizeof(nan)); // cell size
        Check("v4-invalid-profile-rejected-on-decode", !DecodeLayeredWorld(bad_profile, scratch, error));

        // Same inputs -> same bytes; obstruction order does not matter.
        const auto again = Build(stairs);
        Scene reordered = stairs;
        std::reverse(reordered.layers.begin(), reordered.layers.end());
        const auto reorderedBuilt = Build(reordered);
        Check("cook-deterministic", again.ok && EncodeLayeredWorld(again.world) == bytes);
        Check("cook-input-order-independent", reorderedBuilt.ok && EncodeLayeredWorld(reorderedBuilt.world) == bytes);
    }

    // ---------- 11. limits fail closed ----------
    {
        Scene huge;
        huge.bounds = {0, 0, 1024, 1024};
        huge.layers.push_back(Box(1, 0, 0, -1, 1024, 1024, 0));
        LayerClearanceProfile fine = profile;
        fine.cell_size_m = 0.25f; // 4096 x 4096 = 16.7M cells > 4.2M per volume
        const auto built = Build(huge, fine);
        Check("oversized-grid-fails-closed", !built.ok && !built.world.clearance_profile &&
              std::none_of(built.world.volumes.begin(), built.world.volumes.end(),
                           [](const LayerVolume& v) { return v.clearance.has_value(); }), built.error);
        LayerClearanceProfile broken = profile;
        broken.actor_height_m = 0.5f; // shorter than the capsule diameter
        Check("invalid-profile-fails-closed", !Build(stairs, broken).ok);
    }

    // ---------- 12. negative control: the oracle detects an undersized bake ----------
    {
        LayerClearanceProfile thin = profile;
        thin.actor_radius_m = 0.1f; // cooked for a much thinner actor...
        const auto built = Build(room, thin);
        const auto sensitive = built.ok ? RunOracle(built, nullptr, &profile) // ...checked with the real capsule
                                        : OracleResult{};
        Check("negative-control-oracle-detects-undersized-radius", built.ok && sensitive.violations > 0,
              std::to_string(sensitive.violations) + " of " + std::to_string(sensitive.samples) + " centres touch geometry");
        LayerClearanceProfile shortProfile = profile;
        shortProfile.actor_height_m = 1.0f; // head below the 1.2 m beam...
        const auto shortBuilt = Build(room, shortProfile);
        const auto tall = shortBuilt.ok ? RunOracle(shortBuilt, nullptr, &profile) : OracleResult{};
        Check("negative-control-oracle-detects-undersized-height", shortBuilt.ok && tall.violations > 0,
              std::to_string(tall.violations) + " violations");
    }

    Check("oracle-total", oracle_samples > 0 && oracle_violations == 0,
          std::to_string(oracle_samples) + " centres, closest obstruction " + std::to_string(oracle_closest) + " m");
    std::cout << "LAYER CLEARANCE summary: checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
