#include "map/LayerGroundSupport.h"

#include <cmath>

namespace mx::map {
namespace {

bool InsideXY(const LayerVolume& volume, double x, double y) noexcept
{
    return x >= static_cast<double>(volume.bounds.min_x) &&
           x < static_cast<double>(volume.bounds.max_x) &&
           y >= static_cast<double>(volume.bounds.min_y) &&
           y < static_cast<double>(volume.bounds.max_y);
}

bool Contains(const LayerVolume& volume, double x, double y, double z) noexcept
{
    return InsideXY(volume, x, y) && z >= static_cast<double>(volume.min_z) &&
           z < static_cast<double>(volume.max_z);
}

} // namespace

const char* ToString(GroundSupportStatus status) noexcept
{
    switch (status) {
    case GroundSupportStatus::Ok: return "ok";
    case GroundSupportStatus::NotAvailable: return "not_available";
    case GroundSupportStatus::UnknownVolume: return "unknown_volume";
    case GroundSupportStatus::OutsideVolume: return "outside_volume";
    case GroundSupportStatus::UnsupportedMovement: return "unsupported_movement";
    case GroundSupportStatus::InvalidState: return "invalid_state";
    case GroundSupportStatus::TransitionRequired: return "transition_required";
    case GroundSupportStatus::Blocked: return "blocked";
    case GroundSupportStatus::NoClearanceProof: return "no_clearance_proof";
    case GroundSupportStatus::ActorNotCovered: return "actor_not_covered";
    }
    return "unknown";
}

LayerGroundResult ResolveLayerGroundPlacement(const LayeredWorld& world,
    VolumeId volume_id, double x, double y) noexcept
{
    LayerGroundResult result;
    auto fail = [&](GroundSupportStatus status) { result.status = status; return result; };
    if (!std::isfinite(x) || !std::isfinite(y) || world.volumes.size() > kMaxLayeredWorldVolumes) {
        return fail(GroundSupportStatus::InvalidState);
    }
    if (world.volumes.empty()) return fail(GroundSupportStatus::NotAvailable);
    const LayerVolume* selected = nullptr;
    for (const auto& volume : world.volumes) {
        if (volume.id != volume_id) continue;
        if (selected != nullptr) return fail(GroundSupportStatus::InvalidState);
        selected = &volume;
    }
    if (selected == nullptr || volume_id == 0) return fail(GroundSupportStatus::UnknownVolume);
    if (!selected->AllowsGroundMovement()) return fail(GroundSupportStatus::UnsupportedMovement);
    if (!selected->ground_support) return fail(GroundSupportStatus::NotAvailable);
    if (!selected->HasValidGroundSupport()) return fail(GroundSupportStatus::InvalidState);
    if (!InsideXY(*selected, x, y)) return fail(GroundSupportStatus::OutsideVolume);
    const double height = selected->ground_support->Height(x, y);
    if (!std::isfinite(height) || !Contains(*selected, x, y, height)) {
        return fail(GroundSupportStatus::InvalidState);
    }
    // A validated contract cannot have another volume at this point. Do not
    // silently select one if a caller supplied unchecked/ambiguous metadata.
    for (const auto& volume : world.volumes) {
        if (&volume != selected && Contains(volume, x, y, height)) {
            return fail(GroundSupportStatus::InvalidState);
        }
    }
    result.status = GroundSupportStatus::Ok;
    result.state = {selected->id, selected->layer_id, x, y, height};
    result.max_height_error_m = selected->ground_support->max_height_error_m;
    return result;
}

LayerGroundResult ResolveLayerGroundMove(const LayeredWorld& world,
    const LayerGroundState& current, VolumeId target_volume, double x, double y) noexcept
{
    LayerGroundResult result;
    result.state = current;
    auto fail = [&](GroundSupportStatus status) { result.status = status; return result; };
    if (!std::isfinite(current.x) || !std::isfinite(current.y) || !std::isfinite(current.z)) {
        return fail(GroundSupportStatus::InvalidState);
    }
    const auto source = ResolveLayerGroundPlacement(world, current.volume_id, current.x, current.y);
    if (!source.Ok()) return fail(source.status);
    result.max_height_error_m = source.max_height_error_m;
    if (current.layer_id != source.state.layer_id ||
        std::abs(current.z - source.state.z) > source.max_height_error_m) {
        return fail(GroundSupportStatus::InvalidState);
    }
    // The permitted residual is solely the measured collision-to-plane
    // deviation; no arbitrary snapping/settling tolerance is introduced.
    for (const auto& volume : world.volumes) {
        if (volume.id == current.volume_id && !Contains(volume, current.x, current.y, current.z)) {
            return fail(GroundSupportStatus::InvalidState);
        }
        if (volume.id != current.volume_id && Contains(volume, current.x, current.y, current.z)) {
            return fail(GroundSupportStatus::InvalidState);
        }
    }
    if (target_volume != current.volume_id) return fail(GroundSupportStatus::TransitionRequired);
    const auto target = ResolveLayerGroundPlacement(world, target_volume, x, y);
    if (!target.Ok()) return fail(target.status);
    return target;
}

} // namespace mx::map
