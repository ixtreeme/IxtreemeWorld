#include "map/LayerActorMovement.h"

#include "map/LayerClearance.h"

#include <algorithm>
#include <cmath>

namespace mx::map {
namespace {

// Cells are matched with a tiny slack so a point or path on a cell boundary
// must be clear in every cell it touches (never "between" two cells).
constexpr double kCellSlack = 1e-7;

struct CellRect {
    double x0 = 0, x1 = 0, y0 = 0, y1 = 0;
};

struct GridView {
    const LayerVolume* volume = nullptr;
    double cell = 1.0;
    std::uint32_t nx = 0, ny = 0;

    CellRect Cell(std::uint32_t i, std::uint32_t j) const noexcept
    {
        const double x0 = static_cast<double>(volume->bounds.min_x) + static_cast<double>(i) * cell;
        const double y0 = static_cast<double>(volume->bounds.min_y) + static_cast<double>(j) * cell;
        return {x0, std::min(x0 + cell, static_cast<double>(volume->bounds.max_x)),
                y0, std::min(y0 + cell, static_cast<double>(volume->bounds.max_y))};
    }
};

GridView ViewOf(const LayerVolume& volume, const LayerClearanceProfile& profile)
{
    GridView view;
    view.volume = &volume;
    view.cell = static_cast<double>(profile.cell_size_m);
    view.nx = volume.clearance->cells_x;
    view.ny = volume.clearance->cells_y;
    return view;
}

// Liang-Barsky: does the closed segment touch the closed rectangle?
bool SegmentTouchesRect(double ax, double ay, double bx, double by, const CellRect& rect)
{
    double t0 = 0.0, t1 = 1.0;
    const double dx = bx - ax, dy = by - ay;
    const auto clip = [&](double p, double q) {
        if (p == 0.0) return q >= 0.0;
        const double r = q / p;
        if (p < 0.0) {
            if (r > t1) return false;
            t0 = std::max(t0, r);
        } else {
            if (r < t0) return false;
            t1 = std::min(t1, r);
        }
        return true;
    };
    return clip(-dx, ax - rect.x0) && clip(dx, rect.x1 - ax) && clip(-dy, ay - rect.y0) && clip(dy, rect.y1 - ay);
}

// Calls visit(i, j, rect) for every cell the segment touches; stops and
// returns false as soon as visit does.
template <class Visit>
bool ForEachSegmentCell(const GridView& view, double ax, double ay, double bx, double by, Visit&& visit)
{
    const auto index = [&](double value, double origin, std::uint32_t count, double shift) {
        const double raw = std::floor((value - origin) / view.cell) + shift;
        return static_cast<std::uint32_t>(std::clamp(raw, 0.0, static_cast<double>(count - 1)));
    };
    const double ox = view.volume->bounds.min_x, oy = view.volume->bounds.min_y;
    const auto i0 = index(std::min(ax, bx), ox, view.nx, -1.0);
    const auto i1 = index(std::max(ax, bx), ox, view.nx, 1.0);
    const auto j0 = index(std::min(ay, by), oy, view.ny, -1.0);
    const auto j1 = index(std::max(ay, by), oy, view.ny, 1.0);
    for (std::uint32_t j = j0; j <= j1; ++j) {
        for (std::uint32_t i = i0; i <= i1; ++i) {
            const CellRect rect = view.Cell(i, j);
            const CellRect touch{rect.x0 - kCellSlack, rect.x1 + kCellSlack, rect.y0 - kCellSlack, rect.y1 + kCellSlack};
            if (!SegmentTouchesRect(ax, ay, bx, by, touch)) continue;
            if (!visit(i, j, rect)) return false;
        }
    }
    return true;
}

const LayerVolume* FindVolume(const LayeredWorld& world, VolumeId id)
{
    for (const auto& volume : world.volumes) {
        if (volume.id == id) return &volume;
    }
    return nullptr;
}

// A clearance cell is covered by a proven portal's step zone when it lies in
// the corridor band and every corridor cell it overlaps is proven free above
// the two planes' upper envelope.
bool CorridorCovers(const LayerPortalProof& proof, const LayerVolume& a, const LayerVolume& b,
                    const LayerClearanceProfile& profile, const CellRect& cell)
{
    if (proof.slots == 0 || proof.across == 0) return false;
    double across0 = 0.0, across1 = 0.0;
    LayerPortalCorridorAcross(proof, a, b, profile, across0, across1);
    const double c0 = proof.axis == 0 ? cell.x0 : cell.y0;
    const double c1 = proof.axis == 0 ? cell.x1 : cell.y1;
    const double l0 = proof.axis == 0 ? cell.y0 : cell.x0;
    const double l1 = proof.axis == 0 ? cell.y1 : cell.x1;
    const double span0 = proof.span_min, span1 = proof.span_max;
    if (c0 < across0 - kCellSlack || c1 > across1 + kCellSlack || l0 < span0 - kCellSlack || l1 > span1 + kCellSlack) {
        return false;
    }
    const double size = static_cast<double>(profile.cell_size_m);
    const auto index = [&](double value, double origin, double end, std::uint32_t count) {
        const double raw = std::floor((std::clamp(value, origin, end) - origin) / size);
        return static_cast<std::uint32_t>(std::clamp(raw, 0.0, static_cast<double>(count - 1)));
    };
    const auto s0 = index(l0 - kCellSlack, span0, span1, proof.slots);
    const auto s1 = index(l1 + kCellSlack, span0, span1, proof.slots);
    const auto a0 = index(c0 - kCellSlack, across0, across1, proof.across);
    const auto a1 = index(c1 + kCellSlack, across0, across1, proof.across);
    for (std::uint32_t s = s0; s <= s1; ++s) {
        for (std::uint32_t c = a0; c <= a1; ++c) {
            if (proof.CellBlocked(s, c)) return false;
        }
    }
    return true;
}

// Clear in the volume's own grid, or inside the proven step zone of any
// proven portal of that volume.
bool Passable(const LayeredWorld& world, const LayerVolume& volume, const LayerClearanceProfile& profile,
              std::uint32_t i, std::uint32_t j, const CellRect& rect)
{
    if (!volume.clearance->Blocked(i, j)) return true;
    for (const auto& portal : world.portals) {
        if (!portal.proof || (portal.source_volume != volume.id && portal.target_volume != volume.id)) continue;
        const LayerVolume* a = nullptr;
        const LayerVolume* b = nullptr;
        for (const auto& candidate : world.volumes) {
            if (candidate.id == portal.source_volume) a = &candidate;
            if (candidate.id == portal.target_volume) b = &candidate;
        }
        if (a != nullptr && b != nullptr && CorridorCovers(*portal.proof, *a, *b, profile, rect)) return true;
    }
    return false;
}

bool ActorFinite(const LayerActorProfile& actor)
{
    return std::isfinite(actor.radius_m) && actor.radius_m > 0.0f && std::isfinite(actor.height_m) &&
           actor.height_m >= 2.0f * actor.radius_m;
}

} // namespace

LayerGroundResult ResolveLayerActorPlacement(const LayeredWorld& world,
    const LayerActorProfile& actor, VolumeId volume_id, double x, double y) noexcept
{
    LayerGroundResult result;
    auto fail = [&](GroundSupportStatus status) {
        LayerGroundResult failed;
        failed.status = status;
        return failed;
    };
    if (!ActorFinite(actor)) return fail(GroundSupportStatus::InvalidState);
    if (!world.clearance_profile) return fail(GroundSupportStatus::NoClearanceProof);
    const auto& profile = *world.clearance_profile;
    if (actor.radius_m > profile.actor_radius_m || actor.height_m > profile.actor_height_m) {
        return fail(GroundSupportStatus::ActorNotCovered);
    }
    result = ResolveLayerGroundPlacement(world, volume_id, x, y);
    if (!result.Ok()) return result;
    const LayerVolume* volume = FindVolume(world, volume_id);
    if (volume == nullptr || !volume->clearance) return fail(GroundSupportStatus::NoClearanceProof);
    const auto view = ViewOf(*volume, profile);
    const bool clear = ForEachSegmentCell(view, x, y, x, y, [&](std::uint32_t i, std::uint32_t j, const CellRect& rect) {
        return Passable(world, *volume, profile, i, j, rect);
    });
    if (!clear) return fail(GroundSupportStatus::Blocked);
    return result;
}

LayerGroundResult ResolveLayerActorMove(const LayeredWorld& world,
    const LayerActorProfile& actor, const LayerGroundState& current,
    VolumeId target_volume, double x, double y) noexcept
{
    auto fail = [&](GroundSupportStatus status) {
        LayerGroundResult failed;
        failed.status = status;
        failed.state = current;
        return failed;
    };
    if (!ActorFinite(actor)) return fail(GroundSupportStatus::InvalidState);
    if (!world.clearance_profile) return fail(GroundSupportStatus::NoClearanceProof);
    const auto& profile = *world.clearance_profile;
    if (actor.radius_m > profile.actor_radius_m || actor.height_m > profile.actor_height_m) {
        return fail(GroundSupportStatus::ActorNotCovered);
    }
    // The current pose must be the authoritative grounded state (3D-4A
    // identity/residual rules) AND a proven passable cell.
    const auto here = ResolveLayerGroundMove(world, current, current.volume_id, current.x, current.y);
    if (!here.Ok()) return fail(here.status);
    const auto start = ResolveLayerActorPlacement(world, actor, current.volume_id, current.x, current.y);
    if (!start.Ok()) {
        return fail(start.status == GroundSupportStatus::Blocked ? GroundSupportStatus::InvalidState : start.status);
    }
    const auto end = ResolveLayerActorPlacement(world, actor, target_volume, x, y);
    if (!end.Ok()) return fail(end.status);
    const LayerVolume* source = FindVolume(world, current.volume_id);
    const LayerVolume* target = FindVolume(world, target_volume);
    if (source == nullptr || target == nullptr || !source->clearance || !target->clearance) {
        return fail(GroundSupportStatus::NoClearanceProof);
    }

    if (target_volume == current.volume_id) {
        const auto view = ViewOf(*source, profile);
        const bool clear = ForEachSegmentCell(view, current.x, current.y, x, y,
            [&](std::uint32_t i, std::uint32_t j, const CellRect& rect) {
                return Passable(world, *source, profile, i, j, rect);
            });
        return clear ? end : fail(GroundSupportStatus::Blocked);
    }

    for (const auto& portal : world.portals) {
        if (!portal.proof) continue;
        const bool forward = portal.source_volume == current.volume_id && portal.target_volume == target_volume;
        const bool backward = portal.bidirectional && portal.source_volume == target_volume &&
                              portal.target_volume == current.volume_id;
        if (!forward && !backward) continue;
        const auto& proof = *portal.proof;
        const double edge = proof.edge;
        const double from_across = proof.axis == 0 ? current.x : current.y;
        const double to_across = proof.axis == 0 ? x : y;
        const double from_along = proof.axis == 0 ? current.y : current.x;
        const double to_along = proof.axis == 0 ? y : x;
        if (from_across == to_across) continue;
        const double t = (edge - from_across) / (to_across - from_across);
        if (!(t >= 0.0 && t <= 1.0)) continue;
        const double along = from_along + t * (to_along - from_along);
        if (!(along >= proof.span_min && along <= proof.span_max)) continue;
        const double ex = proof.axis == 0 ? edge : along;
        const double ey = proof.axis == 0 ? along : edge;
        const bool clear =
            ForEachSegmentCell(ViewOf(*source, profile), current.x, current.y, ex, ey,
                [&](std::uint32_t i, std::uint32_t j, const CellRect& rect) {
                    return Passable(world, *source, profile, i, j, rect);
                }) &&
            ForEachSegmentCell(ViewOf(*target, profile), ex, ey, x, y,
                [&](std::uint32_t i, std::uint32_t j, const CellRect& rect) {
                    return Passable(world, *target, profile, i, j, rect);
                });
        if (!clear) return fail(GroundSupportStatus::Blocked);
        auto moved = end;
        moved.portal_id = portal.id;
        return moved;
    }
    // Different volume without a proven portal on this segment: an explicit
    // (lift, door, teleport) or missing transition.
    return fail(GroundSupportStatus::TransitionRequired);
}

} // namespace mx::map
