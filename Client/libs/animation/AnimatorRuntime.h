#pragma once

// Stage 4: the per-entity AnimatorController state-machine evaluator. Reads parameters, evaluates
// transitions, advances the active state's phase, samples its clip (Stage-3 retarget) and, during
// a transition, crossfades two clips via an ozz BlendingJob — producing one final target-ordered
// local pose for SkinnedMeshRenderer::SkinInstanceFromPose. v1: single flat machine, clip-per-state.

#include "AnimationRuntime.h"
#include "AnimatorController.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <ozz/base/maths/soa_transform.h>
#include <ozz/base/span.h>

namespace ixanim
{

// Resolves an AnimationClip asset id to an absolute .ixclip path (supplied by the caller; keeps
// this module free of any AssetLibrary dependency).
using ClipPathResolver = std::function<std::string(const std::string&)>;

struct AnimatorRuntime
{
    const AnimatorController* controller = nullptr;  // not owned (lives in a controller cache)
    std::vector<float> paramValues;                  // parallel to controller->parameters
    std::unordered_map<std::string, std::size_t> paramIndex;

    std::uint32_t currentStateId = 0;
    float currentNormTime = 0.0f;

    bool inTransition = false;
    std::uint32_t transitionToStateId = 0;
    float transitionElapsed = 0.0f;
    float transitionDuration = 0.0f;
    float transitionToNormTime = 0.0f;

    // One ClipPlayback per state's clip id, bound to THIS entity's skeleton (lazy; a default-
    // constructed/!ready entry is cached for clips that fail to load, to avoid per-frame retries).
    std::unordered_map<std::string, ClipPlayback> clipPlaybacks;
    std::vector<ozz::math::SoaTransform> blendedLocals;  // crossfade output buffer

    int targetNumJoints = 0;
    int targetNumSoa = 0;
    std::vector<std::string> targetJointNames;  // cached for lazy clip binding
    bool bound = false;
};

// (Re)binds the runtime to a controller + this entity's target skeleton; resets to the default state.
void BindAnimator(AnimatorRuntime& rt,
                  const AnimatorController* controller,
                  const std::vector<std::string>& targetJointNames,
                  int targetNumJoints,
                  int targetNumSoa);

// Parameter writes (no-op if the controller declares no such parameter).
void SetFloat(AnimatorRuntime& rt, const std::string& name, float v);
void SetBool(AnimatorRuntime& rt, const std::string& name, bool v);
void SetInt(AnimatorRuntime& rt, const std::string& name, int v);
void SetTrigger(AnimatorRuntime& rt, const std::string& name);

// Ticks the FSM by dt and returns the final target-ordered local pose (empty span on failure or
// while the active state has no loadable clip — caller then keeps the rest/MotionState pose).
ozz::span<const ozz::math::SoaTransform> EvaluateAnimator(
    AnimatorRuntime& rt,
    float dtSeconds,
    ozz::span<const ozz::math::SoaTransform> targetRest,
    const ClipPathResolver& resolveClipPath);

} // namespace ixanim
