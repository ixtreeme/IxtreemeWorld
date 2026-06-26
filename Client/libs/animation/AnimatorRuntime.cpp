#include "AnimatorRuntime.h"

#include <algorithm>
#include <cmath>

#include <ozz/animation/runtime/blending_job.h>

namespace ixanim
{
namespace
{

const AnimatorState* FindState(const AnimatorController& c, std::uint32_t id)
{
    for (const AnimatorState& s : c.states)
        if (s.id == id)
            return &s;
    return nullptr;
}

// Lazily loads + retarget-binds the clip for a state onto this entity's skeleton. Returns nullptr
// for a clipless state or a clip that fails to load (a !ready entry is cached to stop retrying).
ClipPlayback* EnsurePlayback(AnimatorRuntime& rt, const std::string& clipId, const ClipPathResolver& resolveClipPath)
{
    if (clipId.empty())
        return nullptr;
    auto it = rt.clipPlaybacks.find(clipId);
    if (it != rt.clipPlaybacks.end())
        return it->second.ready ? &it->second : nullptr;

    ClipPlayback playback;
    const std::string path = resolveClipPath ? resolveClipPath(clipId) : std::string();
    std::shared_ptr<ClipAsset> clip = path.empty() ? nullptr : LoadClipAsset(path);
    bool ok = false;
    if (clip)
        ok = BindClip(playback, clip, rt.targetJointNames, rt.targetNumJoints, rt.targetNumSoa);

    auto& stored = rt.clipPlaybacks[clipId];
    stored = std::move(playback);  // ready==false if !ok → cached as a negative result
    return (ok && stored.ready) ? &stored : nullptr;
}

float ParamValue(const AnimatorRuntime& rt, const std::string& name)
{
    auto it = rt.paramIndex.find(name);
    return it != rt.paramIndex.end() ? rt.paramValues[it->second] : 0.0f;
}

bool ConditionPasses(const AnimatorRuntime& rt, const AnimatorCondition& c)
{
    auto it = rt.paramIndex.find(c.param);
    if (it == rt.paramIndex.end())
        return false;
    const float v = rt.paramValues[it->second];
    switch (c.op)
    {
    case ConditionOp::Greater: return v > c.value;
    case ConditionOp::Less: return v < c.value;
    case ConditionOp::Equals: return v == c.value;
    case ConditionOp::NotEquals: return v != c.value;
    case ConditionOp::If: return v != 0.0f;
    case ConditionOp::IfNot: return v == 0.0f;
    }
    return false;
}

bool AllConditionsPass(const AnimatorRuntime& rt, const AnimatorTransition& t)
{
    for (const AnimatorCondition& c : t.conditions)
        if (!ConditionPasses(rt, c))
            return false;
    return true;
}

void ConsumeTriggers(AnimatorRuntime& rt, const AnimatorTransition& t)
{
    if (!rt.controller)
        return;
    for (const AnimatorCondition& c : t.conditions)
    {
        auto it = rt.paramIndex.find(c.param);
        if (it != rt.paramIndex.end() && rt.controller->parameters[it->second].type == ParamType::Trigger)
            rt.paramValues[it->second] = 0.0f;
    }
}

float AdvancePhase(float normTime, float dt, float speed, float duration, bool loop)
{
    if (duration <= 0.0f)
        return normTime;
    normTime += dt * speed / duration;
    if (loop)
        normTime -= std::floor(normTime);
    else
        normTime = std::clamp(normTime, 0.0f, 1.0f);
    return normTime;
}

} // namespace

void BindAnimator(AnimatorRuntime& rt,
                  const AnimatorController* controller,
                  const std::vector<std::string>& targetJointNames,
                  int targetNumJoints,
                  int targetNumSoa)
{
    rt = AnimatorRuntime{};
    rt.controller = controller;
    rt.targetJointNames = targetJointNames;
    rt.targetNumJoints = targetNumJoints;
    rt.targetNumSoa = targetNumSoa;
    rt.blendedLocals.assign(static_cast<std::size_t>(std::max(0, targetNumSoa)), ozz::math::SoaTransform());
    if (!controller || targetNumJoints <= 0 || targetNumSoa <= 0)
        return;

    rt.paramValues.resize(controller->parameters.size());
    for (std::size_t i = 0; i < controller->parameters.size(); ++i)
    {
        rt.paramValues[i] = controller->parameters[i].defaultValue;
        rt.paramIndex[controller->parameters[i].name] = i;
    }
    rt.currentStateId = controller->defaultStateId;
    rt.currentNormTime = 0.0f;
    rt.bound = true;
}

void SetFloat(AnimatorRuntime& rt, const std::string& name, float v)
{
    auto it = rt.paramIndex.find(name);
    if (it != rt.paramIndex.end())
        rt.paramValues[it->second] = v;
}

void SetBool(AnimatorRuntime& rt, const std::string& name, bool v)
{
    SetFloat(rt, name, v ? 1.0f : 0.0f);
}

void SetInt(AnimatorRuntime& rt, const std::string& name, int v)
{
    SetFloat(rt, name, static_cast<float>(v));
}

void SetTrigger(AnimatorRuntime& rt, const std::string& name)
{
    SetFloat(rt, name, 1.0f);  // consumed (reset to 0) by the next transition that uses it
}

ozz::span<const ozz::math::SoaTransform> EvaluateAnimator(
    AnimatorRuntime& rt,
    float dtSeconds,
    ozz::span<const ozz::math::SoaTransform> targetRest,
    const ClipPathResolver& resolveClipPath)
{
    if (!rt.controller || !rt.bound)
        return {};
    if (targetRest.size() < static_cast<std::size_t>(rt.targetNumSoa))
        return {};

    const AnimatorController& c = *rt.controller;

    const AnimatorState* cur = FindState(c, rt.currentStateId);
    if (!cur)
    {
        rt.currentStateId = c.defaultStateId;
        rt.currentNormTime = 0.0f;
        rt.inTransition = false;
        cur = FindState(c, rt.currentStateId);
        if (!cur)
            return {};
    }

    // Advance the current state's phase.
    ClipPlayback* curPb = EnsurePlayback(rt, cur->clipId, resolveClipPath);
    {
        const float dur = (curPb && curPb->clip) ? curPb->clip->duration : 0.0f;
        const float speed = cur->speed * (cur->speedParam.empty() ? 1.0f : ParamValue(rt, cur->speedParam));
        rt.currentNormTime = AdvancePhase(rt.currentNormTime, dtSeconds, speed, dur, cur->loop);
    }

    // Advance + possibly complete an active transition.
    bool justCompletedTransition = false;
    if (rt.inTransition)
    {
        rt.transitionElapsed += dtSeconds;
        if (const AnimatorState* dst = FindState(c, rt.transitionToStateId))
        {
            ClipPlayback* dstPb = EnsurePlayback(rt, dst->clipId, resolveClipPath);
            const float dur = (dstPb && dstPb->clip) ? dstPb->clip->duration : 0.0f;
            const float speed = dst->speed * (dst->speedParam.empty() ? 1.0f : ParamValue(rt, dst->speedParam));
            rt.transitionToNormTime = AdvancePhase(rt.transitionToNormTime, dtSeconds, speed, dur, dst->loop);
        }
        if (rt.transitionElapsed >= rt.transitionDuration)
        {
            rt.currentStateId = rt.transitionToStateId;
            rt.currentNormTime = rt.transitionToNormTime;
            rt.inTransition = false;
            justCompletedTransition = true;  // don't start a NEW transition the same frame
            cur = FindState(c, rt.currentStateId);
            if (!cur)
                return {};
            curPb = EnsurePlayback(rt, cur->clipId, resolveClipPath);
        }
    }

    // Evaluate transitions (Any-State first = highest priority, then the current state's outgoing).
    // Skip on the frame a transition just completed, so at most one transition starts per frame.
    if (!rt.inTransition && !justCompletedTransition)
    {
        const AnimatorTransition* taken = nullptr;
        for (int pass = 0; pass < 2 && !taken; ++pass)
        {
            for (const AnimatorTransition& t : c.transitions)
            {
                const bool isAny = (t.fromStateId == kAnyStateId);
                if (pass == 0 && !isAny) continue;
                if (pass == 1 && (isAny || t.fromStateId != rt.currentStateId)) continue;
                if (t.toStateId == rt.currentStateId && !t.canTransitionToSelf) continue;
                if (t.hasExitTime && rt.currentNormTime < t.exitTime) continue;
                if (!AllConditionsPass(rt, t)) continue;
                taken = &t;
                break;
            }
        }
        if (taken)
        {
            ConsumeTriggers(rt, *taken);
            rt.inTransition = true;
            rt.transitionToStateId = taken->toStateId;
            rt.transitionElapsed = 0.0f;
            rt.transitionDuration = std::max(0.0001f, taken->duration);
            rt.transitionToNormTime = 0.0f;
        }
    }

    // Sample + (during a transition) crossfade.
    const ozz::span<const ozz::math::SoaTransform> curPose =
        curPb ? SampleAndRetargetAt(*curPb, targetRest, rt.currentNormTime)
              : ozz::span<const ozz::math::SoaTransform>{};

    if (!rt.inTransition)
        return curPose.size() != 0 ? curPose : targetRest;  // clipless state → rest pose

    const AnimatorState* dst = FindState(c, rt.transitionToStateId);
    ClipPlayback* dstPb = dst ? EnsurePlayback(rt, dst->clipId, resolveClipPath) : nullptr;
    // Same underlying clip on both sides: curPose and dstPose would alias one targetLocals buffer
    // (the second sample overwrites the first), so skip the crossfade and just keep the source.
    if (dstPb == curPb)
        return curPose.size() != 0 ? curPose : targetRest;
    const ozz::span<const ozz::math::SoaTransform> dstPose =
        dstPb ? SampleAndRetargetAt(*dstPb, targetRest, rt.transitionToNormTime)
              : ozz::span<const ozz::math::SoaTransform>{};

    const float t = std::clamp(rt.transitionElapsed / rt.transitionDuration, 0.0f, 1.0f);

    // If either side is clipless, fall back to the other (or rest) without a blend.
    if (curPose.size() == 0 && dstPose.size() == 0)
        return targetRest;
    if (curPose.size() == 0)
        return dstPose;
    if (dstPose.size() == 0)
        return curPose;

    ozz::animation::BlendingJob::Layer layers[2];
    layers[0].transform = curPose;
    layers[0].weight = 1.0f - t;
    layers[1].transform = dstPose;
    layers[1].weight = t;

    ozz::animation::BlendingJob blend;
    blend.threshold = 0.01f;
    blend.layers = ozz::span<const ozz::animation::BlendingJob::Layer>(layers, 2);
    blend.rest_pose = targetRest;
    blend.output = ozz::make_span(rt.blendedLocals);
    if (!blend.Run())
        return curPose;  // graceful: show the source pose if the blend can't run
    return ozz::make_span(rt.blendedLocals);
}

} // namespace ixanim
