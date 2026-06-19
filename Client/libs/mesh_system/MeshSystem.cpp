#include "MeshSystem.h"

#include <algorithm>
#include <cmath>

WorldVec3 TransformMeshLocalPoint(const MeshSceneEntity& mesh, WorldVec3 local)
{
    local.x *= mesh.scale[0];
    local.y *= mesh.scale[1];
    local.z *= mesh.scale[2];

    const float cx = std::cos(mesh.rotation[0]);
    const float sx = std::sin(mesh.rotation[0]);
    const float yx = local.y * cx - local.z * sx;
    const float zx = local.y * sx + local.z * cx;
    local.y = yx;
    local.z = zx;

    const float cy = std::cos(mesh.rotation[1]);
    const float sy = std::sin(mesh.rotation[1]);
    const float xy = local.x * cy - local.z * sy;
    const float zy = local.x * sy + local.z * cy;
    local.x = xy;
    local.z = zy;

    const float cz = std::cos(mesh.rotation[2]);
    const float sz = std::sin(mesh.rotation[2]);
    const float xz = local.x * cz - local.y * sz;
    const float yz = local.x * sz + local.y * cz;
    local.x = xz + mesh.position[0];
    local.y = yz + mesh.position[1];
    local.z += mesh.position[2];
    return local;
}

std::array<WorldVec3, 8> BuildStaticMeshWorldAabbCorners(const MeshSceneEntity& mesh,
                                                         const StaticMeshRenderer& renderer)
{
    const auto& bmin = renderer.BoundsMin();
    const auto& bmax = renderer.BoundsMax();
    const WorldVec3 center{
        (bmin[0] + bmax[0]) * 0.5f,
        (bmin[1] + bmax[1]) * 0.5f,
        (bmin[2] + bmax[2]) * 0.5f};
    WorldVec3 extent{
        std::max(0.001f, (bmax[0] - bmin[0]) * 0.5f),
        std::max(0.001f, (bmax[1] - bmin[1]) * 0.5f),
        std::max(0.001f, (bmax[2] - bmin[2]) * 0.5f)};
    const float inflate = std::max({extent.x, extent.y, extent.z}) * 0.02f + 0.10f;
    extent.x += inflate;
    extent.y += inflate;
    extent.z += inflate;

    std::array<WorldVec3, 8> corners{};
    std::size_t index = 0;
    for (int z = -1; z <= 1; z += 2)
    {
        for (int y = -1; y <= 1; y += 2)
        {
            for (int x = -1; x <= 1; x += 2)
            {
                corners[index++] = TransformMeshLocalPoint(mesh, {
                    center.x + extent.x * static_cast<float>(x),
                    center.y + extent.y * static_cast<float>(y),
                    center.z + extent.z * static_cast<float>(z)});
            }
        }
    }
    return corners;
}

SpatialIndex::Aabb StaticMeshWorldAabb(const MeshSceneEntity& mesh, const StaticMeshRenderer& renderer)
{
    const auto corners = BuildStaticMeshWorldAabbCorners(mesh, renderer);
    SpatialIndex::Aabb bounds{};
    bounds.min = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    bounds.max = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
    for (const WorldVec3& corner : corners)
    {
        bounds.min.x = std::min(bounds.min.x, corner.x);
        bounds.min.y = std::min(bounds.min.y, corner.y);
        bounds.min.z = std::min(bounds.min.z, corner.z);
        bounds.max.x = std::max(bounds.max.x, corner.x);
        bounds.max.y = std::max(bounds.max.y, corner.y);
        bounds.max.z = std::max(bounds.max.z, corner.z);
    }
    return bounds;
}

WorldVec3 StaticMeshLocalBoundsCenter(const StaticMeshRenderer& renderer)
{
    const auto& bmin = renderer.BoundsMin();
    const auto& bmax = renderer.BoundsMax();
    return {
        (bmin[0] + bmax[0]) * 0.5f,
        (bmin[1] + bmax[1]) * 0.5f,
        (bmin[2] + bmax[2]) * 0.5f};
}

WorldVec3 StaticMeshWorldBoundsCenter(const MeshSceneEntity& mesh, const StaticMeshRenderer& renderer)
{
    return TransformMeshLocalPoint(mesh, StaticMeshLocalBoundsCenter(renderer));
}

std::array<WorldVec3, 8> SpatialAabbCorners(const SpatialIndex::Aabb& bounds)
{
    return {{
        {bounds.min.x, bounds.min.y, bounds.min.z},
        {bounds.max.x, bounds.min.y, bounds.min.z},
        {bounds.min.x, bounds.max.y, bounds.min.z},
        {bounds.max.x, bounds.max.y, bounds.min.z},
        {bounds.min.x, bounds.min.y, bounds.max.z},
        {bounds.max.x, bounds.min.y, bounds.max.z},
        {bounds.min.x, bounds.max.y, bounds.max.z},
        {bounds.max.x, bounds.max.y, bounds.max.z},
    }};
}

SpatialIndex::Frustum SpatialFrustumFromCamera(const WorldCamera& camera)
{
    SpatialIndex::Frustum frustum{};
    std::copy(std::begin(camera.viewProjection.m), std::end(camera.viewProjection.m), std::begin(frustum.viewProjection));
    return frustum;
}

bool WorldAabbOutsideCameraFrustum(const WorldCamera& camera, const std::array<WorldVec3, 8>& corners)
{
    bool outsideLeft = true;
    bool outsideRight = true;
    bool outsideBottom = true;
    bool outsideTop = true;
    bool outsideNear = true;
    bool outsideFar = true;
    const auto& m = camera.viewProjection.m;
    for (const WorldVec3& p : corners)
    {
        const float clipX = p.x * m[0] + p.y * m[4] + p.z * m[8] + m[12];
        const float clipY = p.x * m[1] + p.y * m[5] + p.z * m[9] + m[13];
        const float clipZ = p.x * m[2] + p.y * m[6] + p.z * m[10] + m[14];
        const float clipW = p.x * m[3] + p.y * m[7] + p.z * m[11] + m[15];
        outsideLeft = outsideLeft && (clipX < -clipW);
        outsideRight = outsideRight && (clipX > clipW);
        outsideBottom = outsideBottom && (clipY < -clipW);
        outsideTop = outsideTop && (clipY > clipW);
        outsideNear = outsideNear && (clipZ < 0.0f);
        outsideFar = outsideFar && (clipZ > clipW);
    }
    return outsideLeft || outsideRight || outsideBottom || outsideTop || outsideNear || outsideFar;
}

bool RayIntersectsAabb(const WorldVec3& origin,
                       const WorldVec3& direction,
                       const SpatialIndex::Aabb& bounds,
                       float& outT)
{
    float tMin = 0.0f;
    float tMax = std::numeric_limits<float>::max();
    const float originValues[3] = {origin.x, origin.y, origin.z};
    const float directionValues[3] = {direction.x, direction.y, direction.z};
    const float minValues[3] = {bounds.min.x, bounds.min.y, bounds.min.z};
    const float maxValues[3] = {bounds.max.x, bounds.max.y, bounds.max.z};
    for (int axis = 0; axis < 3; ++axis)
    {
        if (std::abs(directionValues[axis]) < 0.000001f)
        {
            if (originValues[axis] < minValues[axis] || originValues[axis] > maxValues[axis])
                return false;
            continue;
        }
        float t1 = (minValues[axis] - originValues[axis]) / directionValues[axis];
        float t2 = (maxValues[axis] - originValues[axis]) / directionValues[axis];
        if (t1 > t2)
            std::swap(t1, t2);
        tMin = std::max(tMin, t1);
        tMax = std::min(tMax, t2);
        if (tMin > tMax)
            return false;
    }
    outT = tMin;
    return tMax >= 0.0f;
}
