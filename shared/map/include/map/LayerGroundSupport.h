#pragma once

#include "map/LayeredWorld.h"

namespace mx::map {

enum class GroundSupportStatus : std::uint8_t {
    Ok = 0,
    NotAvailable,
    UnknownVolume,
    OutsideVolume,
    UnsupportedMovement,
    InvalidState,
    TransitionRequired,
    // 3D-4B actor queries (LayerActorMovement.h):
    Blocked,          // the point or the path is not proven free for the actor
    NoClearanceProof, // the world/volume carries no cooked clearance
    ActorNotCovered,  // the actor is larger than the cooked clearance profile
};

const char* ToString(GroundSupportStatus status) noexcept;

struct LayerGroundState {
    VolumeId volume_id = 0;
    LayerId layer_id = kLegacyLayerId;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct LayerGroundResult {
    GroundSupportStatus status = GroundSupportStatus::NotAvailable;
    LayerGroundState state;
    double max_height_error_m = 0.0;
    // 3D-4B: the proven portal a successful actor move stepped across (0: none).
    std::uint32_t portal_id = 0;

    bool Ok() const noexcept { return status == GroundSupportStatus::Ok; }
};

// Read-only queries over an already validated immutable world. Explicit
// volume identity is required: there is no nearest-floor selection, legacy
// terrain fallback, capsule clearance or portal traversal.
LayerGroundResult ResolveLayerGroundPlacement(const LayeredWorld& world,
    VolumeId volume_id, double x, double y) noexcept;

// Verifies the current authoritative grounded pose before querying the new
// point. Every failure retains current verbatim; a different target volume
// always requires a separate explicit transition operation.
LayerGroundResult ResolveLayerGroundMove(const LayeredWorld& world,
    const LayerGroundState& current, VolumeId target_volume,
    double x, double y) noexcept;

} // namespace mx::map
