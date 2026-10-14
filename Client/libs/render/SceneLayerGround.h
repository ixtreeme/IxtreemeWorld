#pragma once

#include "map/LayerActorMovement.h"
#include "map/LayerGroundSupport.h"

#include <array>
#include <optional>

// Offline adapter for one immutable authored scene/package world instance.
// The referenced world must outlive this object and must not be mutated while
// bound. Re-loading/re-baking a scene requires a new adapter and explicit Place;
// support/volume ids are never carried across different world snapshots.
// Without an actor this resolves support metadata only (3D-4A). With an actor
// (3D-4B) Place/Move additionally require the world's cooked clearance: the
// actor's capsule must fit, a move may not pass through obstruction, and a
// volume change is allowed only across a proven step/ramp portal. It is still
// not admission, packet prediction or a CharacterController activation.
class SceneLayerGround
{
public:
    explicit SceneLayerGround(const mx::map::LayeredWorld& world,
        std::optional<mx::map::LayerActorProfile> actor = std::nullopt) noexcept;
    SceneLayerGround(mx::map::LayeredWorld&&, std::optional<mx::map::LayerActorProfile> = std::nullopt) = delete;
    SceneLayerGround(const mx::map::LayeredWorld&&, std::optional<mx::map::LayerActorProfile> = std::nullopt) = delete;

    // Authored engine coordinates are X/Z horizontal, Y up, in metres.
    // Canonical server X/Y receives engine X/Z without a sign change.
    // Only successful shared queries commit state; failure preserves the
    // previous canonical state and engine position transactionally.
    mx::map::LayerGroundResult Place(mx::map::VolumeId volume,
                                    double engineX, double engineZ) noexcept;
    mx::map::LayerGroundResult Move(mx::map::VolumeId targetVolume,
                                   double engineX, double engineZ) noexcept;

    // A failed/empty codec result is never a world identity, including when
    // both snapshots fail encoding. Does not rebind or modify grounded state.
    bool MatchesWorld(const mx::map::LayeredWorld& candidate) const;

    bool HasState() const noexcept { return hasState_; }
    bool UsesActor() const noexcept { return actor_.has_value(); }
    const mx::map::LayerGroundState& State() const noexcept { return state_; }
    const std::array<double, 3>& EnginePosition() const noexcept { return enginePosition_; }

private:
    mx::map::LayerGroundResult Commit(mx::map::LayerGroundResult result) noexcept;

    const mx::map::LayeredWorld* world_;
    std::optional<mx::map::LayerActorProfile> actor_;
    mx::map::LayerGroundState state_{};
    std::array<double, 3> enginePosition_{}; // X, Y-up, Z
    bool hasState_ = false;
};
