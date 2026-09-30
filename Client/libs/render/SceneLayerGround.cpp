#include "SceneLayerGround.h"

SceneLayerGround::SceneLayerGround(const mx::map::LayeredWorld& world,
                                   std::optional<mx::map::LayerActorProfile> actor) noexcept
    : world_(&world), actor_(actor)
{
}

mx::map::LayerGroundResult SceneLayerGround::Commit(mx::map::LayerGroundResult result) noexcept
{
    if (result.Ok())
    {
        state_ = result.state;
        enginePosition_ = {state_.x, state_.z, state_.y};
        hasState_ = true;
    }
    else
        result.state = state_;
    return result;
}

mx::map::LayerGroundResult SceneLayerGround::Place(mx::map::VolumeId volume,
                                                double engineX, double engineZ) noexcept
{
    return Commit(actor_ ? mx::map::ResolveLayerActorPlacement(*world_, *actor_, volume, engineX, engineZ)
                         : mx::map::ResolveLayerGroundPlacement(*world_, volume, engineX, engineZ));
}

mx::map::LayerGroundResult SceneLayerGround::Move(mx::map::VolumeId targetVolume,
                                               double engineX, double engineZ) noexcept
{
    if (!hasState_)
    {
        mx::map::LayerGroundResult result;
        result.status = mx::map::GroundSupportStatus::InvalidState;
        result.state = state_;
        return result;
    }
    return Commit(actor_ ? mx::map::ResolveLayerActorMove(*world_, *actor_, state_, targetVolume, engineX, engineZ)
                         : mx::map::ResolveLayerGroundMove(*world_, state_, targetVolume, engineX, engineZ));
}

bool SceneLayerGround::MatchesWorld(const mx::map::LayeredWorld& candidate) const
{
    const auto boundBytes = mx::map::EncodeLayeredWorld(*world_);
    if (boundBytes.empty()) return false;
    const auto candidateBytes = mx::map::EncodeLayeredWorld(candidate);
    return !candidateBytes.empty() && boundBytes == candidateBytes;
}
