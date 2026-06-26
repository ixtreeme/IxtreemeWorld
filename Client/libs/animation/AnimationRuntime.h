#pragma once

// Stage 3 of the animator: load a retargetable .ixclip + its ozz Animation, build a per-target
// RetargetPlan (joint-name correspondence), and sample+retarget the clip into a TARGET-ordered
// local pose each frame. The pose is then handed to SkinnedMeshRenderer::SkinInstanceFromPose.
//
// Retarget is Tier-1 (track re-index): correct when source and target share bind orientation
// (the common Mixamo case). Cross-rig rest-delta (Tier-2) is a later addition; the interface
// is unchanged. Everything operates in LOCAL space before LocalToModelJob.

#include <memory>
#include <string>
#include <vector>

#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/base/maths/soa_transform.h>
#include <ozz/base/memory/unique_ptr.h>
#include <ozz/base/span.h>

namespace ixanim
{

// A loaded animation clip, independent of any target character. Shared across all characters
// that play it (ref-counted via shared_ptr in the owning cache).
struct ClipAsset
{
    ozz::unique_ptr<ozz::animation::Animation> animation;
    std::vector<std::string> sourceJointNames;  // ordered == clip tracks (track i targets name[i])
    float duration = 0.0f;                       // seconds (falls back to animation->duration())
    bool loop = true;
};

// Loads a .ixclip wrapper (reads its joint_names + source_anim_path + flags) and the raw ozz
// Animation it points at. Returns nullptr on any failure (logs the reason).
std::shared_ptr<ClipAsset> LoadClipAsset(const std::string& ixclipPath);

// Maps each TARGET joint to a SOURCE clip track by canonical joint name (-1 = no match → the
// target keeps its rest pose for that joint).
struct RetargetPlan
{
    std::vector<int> sourceTrackForTargetJoint;  // size == target joint count
    int matchedJoints = 0;
    bool valid = false;
};

RetargetPlan BuildRetargetPlan(const std::vector<std::string>& sourceJointNames,
                               const std::vector<std::string>& targetJointNames);

// Per-entity playback state. Owns its SamplingJob::Context (one clip per playback) + scratch
// buffers sized to the clip (source) and the target skeleton.
struct ClipPlayback
{
    std::shared_ptr<const ClipAsset> clip;
    RetargetPlan plan;
    // Held by unique_ptr so ClipPlayback stays movable (SamplingJob::Context deletes its move
    // ctor) — required to store ClipPlayback by value in a per-entity map.
    std::unique_ptr<ozz::animation::SamplingJob::Context> context;
    std::vector<ozz::math::SoaTransform> sourceLocals;  // clip num_soa_joints
    std::vector<ozz::math::SoaTransform> targetLocals;  // target num_soa_joints (retarget output)
    std::vector<ozz::math::Transform> targetAoS;        // scratch, target joint count
    float normalizedTime = 0.0f;
    float speed = 1.0f;
    bool loop = true;
    int targetNumJoints = 0;
    int targetNumSoa = 0;
    bool ready = false;
};

// Binds a clip onto a specific target skeleton: builds the RetargetPlan + sizes scratch buffers.
// targetJointNames/targetNumJoints/targetNumSoa come from the target SkinnedMeshRenderer.
// Returns false (and leaves pb.ready=false) if the clip can't drive this skeleton.
bool BindClip(ClipPlayback& pb,
              std::shared_ptr<const ClipAsset> clip,
              const std::vector<std::string>& targetJointNames,
              int targetNumJoints,
              int targetNumSoa);

// Advances the playback clock by dtSeconds, samples the clip, and retargets into pb.targetLocals
// (unmatched joints take their value from targetRest). Returns the target-ordered local pose,
// ready for LocalToModelJob / SkinInstanceFromPose. Returns an empty span on failure.
ozz::span<const ozz::math::SoaTransform> SampleAndRetarget(
    ClipPlayback& pb,
    ozz::span<const ozz::math::SoaTransform> targetRest,
    float dtSeconds);

// Samples the clip at an EXPLICIT normalized phase (0..1) without advancing the internal clock,
// then retargets — for callers (the AnimatorController FSM) that own the playback clock per state.
ozz::span<const ozz::math::SoaTransform> SampleAndRetargetAt(
    ClipPlayback& pb,
    ozz::span<const ozz::math::SoaTransform> targetRest,
    float normalizedTime);

} // namespace ixanim
