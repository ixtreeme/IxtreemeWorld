#include "AnimationRuntime.h"

#include "Debug.h"

#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>
#include <ozz/base/maths/simd_math.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <unordered_map>

namespace ixanim
{
namespace
{

// --- Minimal JSON field extraction for the flat .ixclip body --------------------------------
// The .ixclip is written by AssetLibrary/the importer with only forward-slash paths and bone
// names (no embedded quotes/backslashes), so raw substring extraction is sufficient.

std::string JsonString(const std::string& text, const char* key)
{
    const std::string needle = std::string("\"") + key + "\"";
    std::size_t pos = text.find(needle);
    if (pos == std::string::npos)
        return {};
    pos = text.find(':', pos + needle.size());
    if (pos == std::string::npos)
        return {};
    const std::size_t open = text.find('"', pos + 1);
    if (open == std::string::npos)
        return {};
    const std::size_t close = text.find('"', open + 1);
    if (close == std::string::npos)
        return {};
    return text.substr(open + 1, close - open - 1);
}

float JsonFloat(const std::string& text, const char* key, float fallback)
{
    const std::string needle = std::string("\"") + key + "\"";
    std::size_t pos = text.find(needle);
    if (pos == std::string::npos)
        return fallback;
    pos = text.find(':', pos + needle.size());
    if (pos == std::string::npos)
        return fallback;
    try
    {
        return std::stof(text.substr(pos + 1));
    }
    catch (...)
    {
        return fallback;
    }
}

std::vector<std::string> JsonStringArray(const std::string& text, const char* key)
{
    std::vector<std::string> out;
    const std::string needle = std::string("\"") + key + "\"";
    std::size_t pos = text.find(needle);
    if (pos == std::string::npos)
        return out;
    const std::size_t lb = text.find('[', pos);
    if (lb == std::string::npos)
        return out;
    const std::size_t rb = text.find(']', lb);
    if (rb == std::string::npos)
        return out;
    std::size_t i = lb + 1;
    while (i < rb)
    {
        const std::size_t open = text.find('"', i);
        if (open == std::string::npos || open >= rb)
            break;
        const std::size_t close = text.find('"', open + 1);
        if (close == std::string::npos || close > rb)
            break;
        out.push_back(text.substr(open + 1, close - open - 1));
        i = close + 1;
    }
    return out;
}

std::string ReadWholeFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

ozz::unique_ptr<ozz::animation::Animation> LoadOzzAnimation(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        Tracenf("[ANIM-RT] cannot open ozz animation: %s", path.c_str());
        return nullptr;
    }
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (bytes.empty())
    {
        Tracenf("[ANIM-RT] empty ozz animation: %s", path.c_str());
        return nullptr;
    }

    ozz::io::MemoryStream stream;
    if (stream.Write(bytes.data(), static_cast<std::size_t>(bytes.size())) != static_cast<std::size_t>(bytes.size()))
        return nullptr;
    stream.Seek(0, ozz::io::Stream::kSet);
    ozz::io::IArchive archive(&stream);
    if (!archive.TestTag<ozz::animation::Animation>())
    {
        Tracenf("[ANIM-RT] not an ozz Animation archive: %s", path.c_str());
        return nullptr;
    }
    auto animation = ozz::make_unique<ozz::animation::Animation>();
    archive >> *animation;
    return animation;
}

// --- Joint-name canonicalization (strip common rig prefixes, lowercase) ----------------------

std::string CanonicalBone(const std::string& name)
{
    std::string n = name;
    auto stripPrefix = [&n](const char* prefix) {
        const std::size_t len = std::char_traits<char>::length(prefix);
        if (n.size() >= len && n.compare(0, len, prefix) == 0)
            n.erase(0, len);
    };
    stripPrefix("mixamorig:");
    stripPrefix("mixamorig1:");
    stripPrefix("mixamorig0:");
    // "Armature|Walk:Hips" style — keep the part after the last '|' and last ':'.
    if (const std::size_t bar = n.find_last_of('|'); bar != std::string::npos)
        n.erase(0, bar + 1);
    stripPrefix("Bip01 ");
    stripPrefix("Bip001 ");
    if (const std::size_t colon = n.find_last_of(':'); colon != std::string::npos)
        n.erase(0, colon + 1);
    std::transform(n.begin(), n.end(), n.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return n;
}

// --- SoA <-> AoS helpers (ozz packs 4 joints per SoaTransform lane) ---------------------------

float SimdLane(const ozz::math::SimdFloat4& v, int lane)
{
    switch (lane)
    {
    case 0: return ozz::math::GetX(v);
    case 1: return ozz::math::GetY(v);
    case 2: return ozz::math::GetZ(v);
    default: return ozz::math::GetW(v);
    }
}

ozz::math::Transform DecomposeSoaJoint(const ozz::math::SoaTransform& soa, int lane)
{
    ozz::math::Transform t;
    t.translation = ozz::math::Float3(
        SimdLane(soa.translation.x, lane), SimdLane(soa.translation.y, lane), SimdLane(soa.translation.z, lane));
    t.rotation = ozz::math::Quaternion(
        SimdLane(soa.rotation.x, lane), SimdLane(soa.rotation.y, lane),
        SimdLane(soa.rotation.z, lane), SimdLane(soa.rotation.w, lane));
    t.scale = ozz::math::Float3(
        SimdLane(soa.scale.x, lane), SimdLane(soa.scale.y, lane), SimdLane(soa.scale.z, lane));
    return t;
}

ozz::math::SoaTransform PackSoaQuad(const ozz::math::Transform (&q)[4])
{
    ozz::math::SoaTransform s;
    s.translation.x = ozz::math::simd_float4::Load(q[0].translation.x, q[1].translation.x, q[2].translation.x, q[3].translation.x);
    s.translation.y = ozz::math::simd_float4::Load(q[0].translation.y, q[1].translation.y, q[2].translation.y, q[3].translation.y);
    s.translation.z = ozz::math::simd_float4::Load(q[0].translation.z, q[1].translation.z, q[2].translation.z, q[3].translation.z);
    s.rotation.x = ozz::math::simd_float4::Load(q[0].rotation.x, q[1].rotation.x, q[2].rotation.x, q[3].rotation.x);
    s.rotation.y = ozz::math::simd_float4::Load(q[0].rotation.y, q[1].rotation.y, q[2].rotation.y, q[3].rotation.y);
    s.rotation.z = ozz::math::simd_float4::Load(q[0].rotation.z, q[1].rotation.z, q[2].rotation.z, q[3].rotation.z);
    s.rotation.w = ozz::math::simd_float4::Load(q[0].rotation.w, q[1].rotation.w, q[2].rotation.w, q[3].rotation.w);
    s.scale.x = ozz::math::simd_float4::Load(q[0].scale.x, q[1].scale.x, q[2].scale.x, q[3].scale.x);
    s.scale.y = ozz::math::simd_float4::Load(q[0].scale.y, q[1].scale.y, q[2].scale.y, q[3].scale.y);
    s.scale.z = ozz::math::simd_float4::Load(q[0].scale.z, q[1].scale.z, q[2].scale.z, q[3].scale.z);
    return s;
}

} // namespace

std::shared_ptr<ClipAsset> LoadClipAsset(const std::string& ixclipPath)
{
    const std::string text = ReadWholeFile(ixclipPath);
    if (text.empty())
    {
        Tracenf("[ANIM-RT] cannot read .ixclip: %s", ixclipPath.c_str());
        return nullptr;
    }

    const std::string animPath = JsonString(text, "source_anim_path");
    if (animPath.empty())
    {
        Tracenf("[ANIM-RT] .ixclip missing source_anim_path: %s", ixclipPath.c_str());
        return nullptr;
    }
    auto animation = LoadOzzAnimation(animPath);
    if (!animation)
        return nullptr;

    auto clip = std::make_shared<ClipAsset>();
    clip->animation = std::move(animation);
    clip->sourceJointNames = JsonStringArray(text, "joint_names");
    clip->duration = JsonFloat(text, "duration", clip->animation->duration());
    if (clip->duration <= 0.0f)
        clip->duration = clip->animation->duration();
    clip->loop = JsonFloat(text, "loop", 1.0f) != 0.0f;

    Tracenf("[ANIM-RT] clip loaded path=%s tracks=%d jointNames=%zu duration=%.3f",
        ixclipPath.c_str(),
        clip->animation->num_tracks(),
        clip->sourceJointNames.size(),
        clip->duration);
    return clip;
}

RetargetPlan BuildRetargetPlan(const std::vector<std::string>& sourceJointNames,
                               const std::vector<std::string>& targetJointNames)
{
    RetargetPlan plan;
    plan.sourceTrackForTargetJoint.assign(targetJointNames.size(), -1);

    std::unordered_map<std::string, int> sourceTrackByCanon;
    sourceTrackByCanon.reserve(sourceJointNames.size());
    for (int track = 0; track < static_cast<int>(sourceJointNames.size()); ++track)
        sourceTrackByCanon.emplace(CanonicalBone(sourceJointNames[track]), track);  // first wins

    for (std::size_t j = 0; j < targetJointNames.size(); ++j)
    {
        auto it = sourceTrackByCanon.find(CanonicalBone(targetJointNames[j]));
        if (it != sourceTrackByCanon.end())
        {
            plan.sourceTrackForTargetJoint[j] = it->second;
            ++plan.matchedJoints;
        }
    }
    plan.valid = plan.matchedJoints > 0;
    return plan;
}

bool BindClip(ClipPlayback& pb,
              std::shared_ptr<const ClipAsset> clip,
              const std::vector<std::string>& targetJointNames,
              int targetNumJoints,
              int targetNumSoa)
{
    pb.ready = false;
    if (!clip || !clip->animation || targetNumJoints <= 0 || targetNumSoa <= 0)
        return false;

    pb.clip = std::move(clip);
    pb.plan = BuildRetargetPlan(pb.clip->sourceJointNames, targetJointNames);
    if (!pb.plan.valid)
    {
        Tracenf("[ANIM-RT] retarget produced 0 joint matches (source=%zu target=%d) — clip not bound",
            pb.clip->sourceJointNames.size(), targetNumJoints);
        pb.clip.reset();
        return false;
    }

    pb.targetNumJoints = targetNumJoints;
    pb.targetNumSoa = targetNumSoa;
    pb.context = std::make_unique<ozz::animation::SamplingJob::Context>();
    pb.context->Resize(pb.clip->animation->num_tracks());
    pb.sourceLocals.assign(static_cast<std::size_t>(pb.clip->animation->num_soa_tracks()), ozz::math::SoaTransform());
    pb.targetLocals.assign(static_cast<std::size_t>(targetNumSoa), ozz::math::SoaTransform());
    pb.targetAoS.assign(static_cast<std::size_t>(targetNumJoints), ozz::math::Transform::identity());
    pb.normalizedTime = 0.0f;
    pb.loop = pb.clip->loop;
    pb.ready = true;

    Tracenf("[ANIM-RT] clip bound: matched %d/%d target joints", pb.plan.matchedJoints, targetNumJoints);
    return true;
}

ozz::span<const ozz::math::SoaTransform> SampleAndRetargetAt(
    ClipPlayback& pb,
    ozz::span<const ozz::math::SoaTransform> targetRest,
    float normalizedTime)
{
    if (!pb.ready || !pb.clip || !pb.clip->animation || !pb.context)
        return {};
    if (targetRest.size() < static_cast<std::size_t>(pb.targetNumSoa))
        return {};

    // Sample the source clip at the given phase into source-track-ordered SoA locals.
    ozz::animation::SamplingJob job;
    job.animation = pb.clip->animation.get();
    job.context = pb.context.get();
    job.ratio = std::clamp(normalizedTime, 0.0f, 1.0f);
    job.output = ozz::make_span(pb.sourceLocals);
    if (!job.Run())
        return {};

    const int numTracks = pb.clip->animation->num_tracks();

    // Start every target joint from the target rest pose (so unmatched joints keep the bind),
    // then overwrite matched joints from their source track. SoA is packed 4-joints/lane, so this
    // scatter MUST go through an AoS staging array — never a flat memcpy.
    for (int j = 0; j < pb.targetNumJoints; ++j)
        pb.targetAoS[j] = DecomposeSoaJoint(targetRest[j / 4], j % 4);
    for (int j = 0; j < pb.targetNumJoints; ++j)
    {
        const int track = pb.plan.sourceTrackForTargetJoint[j];
        if (track >= 0 && track < numTracks)
            pb.targetAoS[j] = DecomposeSoaJoint(pb.sourceLocals[track / 4], track % 4);
    }

    // Re-pack the target AoS into target-ordered SoA locals (pad the tail with identity).
    for (int s = 0; s < pb.targetNumSoa; ++s)
    {
        ozz::math::Transform quad[4];
        for (int k = 0; k < 4; ++k)
        {
            const int j = s * 4 + k;
            quad[k] = (j < pb.targetNumJoints) ? pb.targetAoS[j] : ozz::math::Transform::identity();
        }
        pb.targetLocals[static_cast<std::size_t>(s)] = PackSoaQuad(quad);
    }

    return ozz::make_span(pb.targetLocals);
}

ozz::span<const ozz::math::SoaTransform> SampleAndRetarget(
    ClipPlayback& pb,
    ozz::span<const ozz::math::SoaTransform> targetRest,
    float dtSeconds)
{
    if (!pb.ready || !pb.clip || !pb.clip->animation)
        return {};

    const float duration = pb.clip->duration > 0.0f ? pb.clip->duration : pb.clip->animation->duration();
    if (duration > 0.0f)
    {
        pb.normalizedTime += dtSeconds * pb.speed / duration;
        if (pb.loop)
            pb.normalizedTime -= std::floor(pb.normalizedTime);
        else
            pb.normalizedTime = std::clamp(pb.normalizedTime, 0.0f, 1.0f);
    }
    return SampleAndRetargetAt(pb, targetRest, pb.normalizedTime);
}

} // namespace ixanim
