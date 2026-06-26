#include "AnimatorRuntime.h"

#include <algorithm>
#include <cmath>
#include <utility>

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

// The clip whose duration drives a state's phase: the single clip, or a blend tree's first child
// (children are authored to share a comparable loop length, so any one works as the phase length).
const std::string& StatePhaseClipId(const AnimatorState& state)
{
    if (state.blendTree.type != BlendTreeType::Single && !state.blendTree.children.empty())
        return state.blendTree.children.front().clipId;
    return state.clipId;
}

float StateDuration(AnimatorRuntime& rt, const AnimatorState& state, const ClipPathResolver& resolveClipPath)
{
    ClipPlayback* pb = EnsurePlayback(rt, StatePhaseClipId(state), resolveClipPath);
    return (pb && pb->clip) ? pb->clip->duration : 0.0f;
}

// 1D bracket-and-lerp weights over thresholds (sorted locally so authoring order is irrelevant).
// At most two children are non-zero; weights sum to 1. weights is indexed by ORIGINAL child index.
void Compute1DWeights(const std::vector<BlendTreeChild>& children, float param, std::vector<float>& weights)
{
    weights.assign(children.size(), 0.0f);
    if (children.empty())
        return;
    std::vector<std::pair<float, std::size_t>> order;  // (threshold, child index)
    order.reserve(children.size());
    for (std::size_t i = 0; i < children.size(); ++i)
        order.emplace_back(children[i].threshold, i);
    std::sort(order.begin(), order.end(),
        [](const std::pair<float, std::size_t>& a, const std::pair<float, std::size_t>& b) {
            return a.first < b.first;
        });

    if (order.size() == 1 || param <= order.front().first)
    {
        weights[order.front().second] = 1.0f;
    }
    else if (param >= order.back().first)
    {
        weights[order.back().second] = 1.0f;
    }
    else
    {
        for (std::size_t i = 0; i + 1 < order.size(); ++i)
        {
            const float lo = order[i].first;
            const float hi = order[i + 1].first;
            if (param >= lo && param <= hi)
            {
                const float spanLen = hi - lo;
                const float t = (spanLen > 1e-6f) ? (param - lo) / spanLen : 0.0f;
                weights[order[i].second] = 1.0f - t;
                weights[order[i + 1].second] = t;
                break;
            }
        }
    }
}

// 2D blend weights via the Unity "Freeform Cartesian" gradient-band algorithm (Johansen): for each
// child i, weight = min over j!=i of clamp(1 - ((p - p_i)·(p_j - p_i)) / |p_j - p_i|^2, 0, 1), then
// normalize. Sharp, no ghost contributions from far children (the IDW failure mode). weights is
// indexed by child index; falls back to the nearest child if everything collapses to ~0.
void Compute2DWeights(const std::vector<BlendTreeChild>& children, float px, float py,
                      std::vector<float>& weights)
{
    const std::size_t n = children.size();
    weights.assign(n, 0.0f);
    if (n == 0)
        return;
    if (n == 1)
    {
        weights[0] = 1.0f;
        return;
    }

    float sum = 0.0f;
    for (std::size_t i = 0; i < n; ++i)
    {
        const float ix = children[i].position[0];
        const float iy = children[i].position[1];
        const float pix = px - ix;
        const float piy = py - iy;
        float w = 1.0f;
        for (std::size_t j = 0; j < n; ++j)
        {
            if (j == i)
                continue;
            const float jx = children[j].position[0] - ix;
            const float jy = children[j].position[1] - iy;
            const float lenSq = jx * jx + jy * jy;
            if (lenSq > 1e-8f)  // coincident children impose no constraint (h stays 1)
            {
                const float t = (pix * jx + piy * jy) / lenSq;
                w = std::min(w, std::clamp(1.0f - t, 0.0f, 1.0f));
            }
        }
        weights[i] = w;
        sum += w;
    }

    if (sum > 1e-6f)
    {
        for (float& w : weights)
            w /= sum;
    }
    else
    {
        std::size_t nearest = 0;
        float best = 3.4e38f;
        for (std::size_t i = 0; i < n; ++i)
        {
            const float dx = px - children[i].position[0];
            const float dy = py - children[i].position[1];
            const float d = dx * dx + dy * dy;
            if (d < best) { best = d; nearest = i; }
        }
        weights[nearest] = 1.0f;
    }
}

// Evaluates one state (single clip OR a 1D/2D blend tree) to outLocals at the shared normalized phase
// (so a tree's children stay footfall-synced). ALWAYS writes the final pose into outLocals (even for
// a single clip) so the two transition sides never alias a shared per-clipId ClipPlayback buffer.
// Returns the span over outLocals, or an empty span if nothing loadable (caller uses the rest pose).
ozz::span<const ozz::math::SoaTransform> EvaluateStateToBuffer(
    AnimatorRuntime& rt,
    const AnimatorState& state,
    float normTime,
    ozz::span<const ozz::math::SoaTransform> targetRest,
    std::vector<ozz::math::SoaTransform>& outLocals,
    const ClipPathResolver& resolveClipPath)
{
    const BlendTree& tree = state.blendTree;
    const bool isTree = (tree.type == BlendTreeType::Blend1D || tree.type == BlendTreeType::Blend2D)
                        && !tree.children.empty();

    auto copyOut = [&](const ozz::span<const ozz::math::SoaTransform>& pose)
        -> ozz::span<const ozz::math::SoaTransform> {
        if (pose.size() == 0 || pose.size() > outLocals.size())
            return {};
        std::copy(pose.begin(), pose.end(), outLocals.begin());
        return ozz::make_span(outLocals);
    };

    // Single clip (Single type) or an empty tree → play the state's own clip.
    if (!isTree)
    {
        ClipPlayback* pb = EnsurePlayback(rt, state.clipId, resolveClipPath);
        if (!pb)
            return {};
        return copyOut(SampleAndRetargetAt(*pb, targetRest, normTime));
    }

    // --- Blend tree: compute per-child weights (1D bracket-lerp / 2D gradient bands), then dedup
    //     duplicate clipIds (summing weights) and blend the active set. ---
    std::vector<float> weights;
    if (tree.type == BlendTreeType::Blend2D)
        Compute2DWeights(tree.children, ParamValue(rt, tree.blendParam),
                         ParamValue(rt, tree.blendParamY), weights);
    else
        Compute1DWeights(tree.children, ParamValue(rt, tree.blendParam), weights);

    std::vector<std::pair<std::string, float>> active;  // (clipId, summed weight)
    for (std::size_t i = 0; i < tree.children.size(); ++i)
    {
        if (weights[i] <= 1e-4f)
            continue;
        const std::string& cid = tree.children[i].clipId;
        bool merged = false;
        for (std::pair<std::string, float>& a : active)
            if (a.first == cid) { a.second += weights[i]; merged = true; break; }
        if (!merged)
            active.emplace_back(cid, weights[i]);
    }
    if (active.empty())
        return {};
    if (active.size() == 1)
    {
        ClipPlayback* pb = EnsurePlayback(rt, active.front().first, resolveClipPath);
        if (!pb)
            return {};
        return copyOut(SampleAndRetargetAt(*pb, targetRest, normTime));
    }

    // >=2 distinct active clips: each clipId has its OWN ClipPlayback::targetLocals buffer, so all
    // sampled poses stay valid simultaneously for one N-layer BlendingJob.
    rt.treeLayerScratch.clear();
    for (const std::pair<std::string, float>& a : active)
    {
        ClipPlayback* pb = EnsurePlayback(rt, a.first, resolveClipPath);
        if (!pb)
            continue;
        const ozz::span<const ozz::math::SoaTransform> pose = SampleAndRetargetAt(*pb, targetRest, normTime);
        if (pose.size() == 0)
            continue;
        ozz::animation::BlendingJob::Layer layer;
        layer.transform = pose;
        layer.weight = a.second;
        rt.treeLayerScratch.push_back(layer);
    }
    if (rt.treeLayerScratch.empty())
        return {};
    if (rt.treeLayerScratch.size() == 1)
        return copyOut(rt.treeLayerScratch.front().transform);  // a child failed to load → survivor

    ozz::animation::BlendingJob blend;
    blend.threshold = 0.01f;
    blend.layers = ozz::make_span(rt.treeLayerScratch);
    blend.rest_pose = targetRest;
    blend.output = ozz::make_span(outLocals);
    if (!blend.Run())
        return copyOut(rt.treeLayerScratch.front().transform);
    return ozz::make_span(outLocals);
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
    const std::size_t soaCount = static_cast<std::size_t>(std::max(0, targetNumSoa));
    rt.blendedLocals.assign(soaCount, ozz::math::SoaTransform());
    rt.stateLocalsA.assign(soaCount, ozz::math::SoaTransform());
    rt.stateLocalsB.assign(soaCount, ozz::math::SoaTransform());
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
    {
        const float dur = StateDuration(rt, *cur, resolveClipPath);
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
            const float dur = StateDuration(rt, *dst, resolveClipPath);
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

    // Evaluate the current state (single clip or blend tree) into its own buffer.
    const ozz::span<const ozz::math::SoaTransform> curPose =
        EvaluateStateToBuffer(rt, *cur, rt.currentNormTime, targetRest, rt.stateLocalsA, resolveClipPath);

    if (!rt.inTransition)
        return curPose.size() != 0 ? curPose : targetRest;  // motionless state → rest pose

    // Evaluate the destination into a DISTINCT buffer. Because EvaluateStateToBuffer always writes
    // its final pose into the passed buffer, curPose (stateLocalsA) and dstPose (stateLocalsB) can
    // never alias a shared per-clipId ClipPlayback buffer — even if both sides use the same clip.
    const AnimatorState* dst = FindState(c, rt.transitionToStateId);
    const ozz::span<const ozz::math::SoaTransform> dstPose =
        dst ? EvaluateStateToBuffer(rt, *dst, rt.transitionToNormTime, targetRest, rt.stateLocalsB, resolveClipPath)
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
