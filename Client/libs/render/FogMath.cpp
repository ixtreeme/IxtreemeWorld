#include "FogMath.h"
#include "math/IXMath.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace xm = ixtreeme::math;
namespace {
WorldVec3 Vec(const float* v) { return {v[0], v[1], v[2]}; }
float HeightIntegral(float y0, float y1, float length, float base, float falloff)
{
    if (falloff <= 0 || std::max(y0, y1) <= base) return length;
    const float low = std::min(y0, y1), high = std::max(y0, y1);
    float below = 0;
    if (low < base) below = (base - low) / (high - low);
    const float h0 = std::max(0.0f, low - base), h1 = std::max(0.0f, high - base);
    const float delta = (h1 - h0) * falloff;
    const float average = delta < 0.001f ? std::exp(-(h0 + h1) * 0.5f * falloff) :
        (std::exp(-h0 * falloff) - std::exp(-h1 * falloff)) / delta;
    return length * (below + (1 - below) * average);
}
}

FogVolumeGpu BuildFogVolume(const MeshSceneEntity& entity)
{
    auto fog = entity.fogVolume;
    SanitizeFog(fog);
    FogVolumeGpu out;
    std::copy_n(entity.position, 3, out.centerShape);
    out.centerShape[3] = fog.shape == FogVolumeComponent::Shape::Sphere ? 1.0f : 0.0f;
    const auto rotation = xm::TRS({}, xm::FromEulerRadians(Vec(entity.rotation)), {1, 1, 1});
    float* axes[] = {out.axisXExtent, out.axisYExtent, out.axisZExtent};
    const WorldVec3 basis[] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int axis = 0; axis < 3; ++axis)
    {
        const auto v = xm::TransformVector(rotation, basis[axis]);
        axes[axis][0] = v.x; axes[axis][1] = v.y; axes[axis][2] = v.z;
        axes[axis][3] = out.centerShape[3] == 1 ? fog.radius : fog.halfExtents[axis];
    }
    std::copy_n(fog.color, 3, out.colorDensity);
    out.colorDensity[3] = fog.density;
    out.fade[0] = fog.edgeFade;
    return out;
}

bool BuildFogUniforms(FogUniforms& out, const WorldCamera& camera, GlobalFogSettings global,
                     std::span<const MeshSceneEntity> entities, bool respectEditorHidden)
{
    out = {};
    xm::Mat4 inverse;
    if (!xm::Inverse(camera.viewProjection, inverse)) return false;
    std::memcpy(out.inverseViewProjection, inverse.m, sizeof(out.inverseViewProjection));
    out.camera[0] = camera.eye.x; out.camera[1] = camera.eye.y; out.camera[2] = camera.eye.z;
    out.camera[3] = camera.farPlane;
    SanitizeFog(global);
    std::copy_n(global.color, 3, out.colorDensity);
    out.colorDensity[3] = global.density;
    out.globalParams[0] = global.startDistance;
    out.globalParams[1] = global.baseHeight;
    out.globalParams[2] = global.heightEnabled ? global.heightFalloff : 0;
    out.globalParams[3] = global.maxOpacity;
    out.options[0] = global.enabled && global.density > 0 && global.maxOpacity > 0 ? 1.0f : 0.0f;
    out.options[1] = global.affectSky ? 1.0f : 0.0f;
    // The local ray is never limited by the global fog's max distance.
    out.options[3] = global.maxDistance;
    std::size_t count = 0;
    for (const auto& entity : entities)
    {
        if (!entity.hasFogVolume || !entity.effectiveEnabled || !entity.enabled ||
            !entity.fogVolume.enabled || entity.fogVolume.density <= 0 ||
            (respectEditorHidden && entity.editorHidden)) continue;
        if (count == kMaxFogVolumes) break;
        out.volumes[count++] = BuildFogVolume(entity);
    }
    out.options[2] = static_cast<float>(count);
    return out.options[0] > 0 || count > 0;
}

float GlobalFogOpticalDepth(const FogUniforms& fog, WorldVec3 direction, float distance, bool sky)
{
    if (fog.options[0] == 0 || (sky && fog.options[1] == 0)) return 0;
    const float end = std::min(distance, fog.options[3]);
    const float begin = std::min(end, fog.globalParams[0]);
    const float optical = fog.colorDensity[3] * HeightIntegral(fog.camera[1] + direction.y * begin,
        fog.camera[1] + direction.y * end, end - begin, fog.globalParams[1], fog.globalParams[2]);
    const float cap = -std::log(std::max(1.0e-9f, 1 - fog.globalParams[3]));
    return std::min(optical, cap);
}

float LocalFogOpticalDepth(const FogVolumeGpu& fog, WorldVec3 origin, WorldVec3 direction, float distance)
{
    const auto relative = origin - Vec(fog.centerShape);
    WorldVec3 p{xm::Dot(relative, Vec(fog.axisXExtent)), xm::Dot(relative, Vec(fog.axisYExtent)), xm::Dot(relative, Vec(fog.axisZExtent))};
    WorldVec3 d{xm::Dot(direction, Vec(fog.axisXExtent)), xm::Dot(direction, Vec(fog.axisYExtent)), xm::Dot(direction, Vec(fog.axisZExtent))};
    const float extents[] = {fog.axisXExtent[3], fog.axisYExtent[3], fog.axisZExtent[3]};
    float enter = 0, exit = distance;
    if (fog.centerShape[3] == 1)
    {
        const float b = xm::Dot(p, d), c = xm::Dot(p, p) - extents[0] * extents[0];
        const float discriminant = b * b - c;
        if (discriminant <= 0) return 0;
        const float root = std::sqrt(discriminant);
        enter = std::max(enter, -b - root); exit = std::min(exit, -b + root);
    }
    else
    {
        const float pos[] = {p.x, p.y, p.z}, dir[] = {d.x, d.y, d.z};
        for (int axis = 0; axis < 3; ++axis)
        {
            if (std::abs(dir[axis]) < 1.0e-6f)
            {
                if (std::abs(pos[axis]) > extents[axis]) return 0;
            }
            else
            {
                const float a = (-extents[axis] - pos[axis]) / dir[axis];
                const float b = ( extents[axis] - pos[axis]) / dir[axis];
                enter = std::max(enter, std::min(a, b)); exit = std::min(exit, std::max(a, b));
            }
        }
    }
    if (exit <= enter) return 0;
    if (fog.fade[0] <= 0) return (exit - enter) * fog.colorDensity[3];
    float weight = 0;
    constexpr int steps = 16;
    for (int sample = 0; sample < steps; ++sample)
    {
        const auto point = p + d * (enter + (exit - enter) * (static_cast<float>(sample) + 0.5f) / steps);
        const float edge = fog.centerShape[3] == 1 ? extents[0] - xm::Length(point) :
            std::min({extents[0] - std::abs(point.x), extents[1] - std::abs(point.y), extents[2] - std::abs(point.z)});
        const float t = std::clamp(edge / fog.fade[0], 0.0f, 1.0f);
        weight += t * t * (3 - 2 * t);
    }
    return weight * (exit - enter) / steps * fog.colorDensity[3];
}

FogEvaluation EvaluateFog(const FogUniforms& fog, WorldVec3 direction, float distance, bool sky)
{
    float depth = GlobalFogOpticalDepth(fog, direction, distance, sky);
    WorldVec3 weighted = Vec(fog.colorDensity) * depth;
    const auto count = std::clamp(static_cast<int>(fog.options[2]), 0, static_cast<int>(kMaxFogVolumes));
    for (int i = 0; i < count; ++i)
    {
        const float local = LocalFogOpticalDepth(fog.volumes[i], Vec(fog.camera), direction, distance);
        depth += local; weighted = weighted + Vec(fog.volumes[i].colorDensity) * local;
    }
    return {std::exp(-depth), depth > 1.0e-6f ? weighted * (1.0f / depth) : WorldVec3{}};
}
