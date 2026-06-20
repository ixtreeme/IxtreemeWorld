#include "MeshSystem.h"

#include <algorithm>
#include <limits>

WorldVec3 TransformMeshLocalPoint(const MeshSceneEntity& mesh, WorldVec3 local)
{
    const ixtreeme::math::Mat4 transform = ixtreeme::math::TRS(
        {mesh.position[0], mesh.position[1], mesh.position[2]},
        ixtreeme::math::FromEulerRadians({mesh.rotation[0], mesh.rotation[1], mesh.rotation[2]}),
        {mesh.scale[0], mesh.scale[1], mesh.scale[2]});
    return ixtreeme::math::TransformPoint(transform, local);
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

    const ixtreeme::math::Mat4 transform = ixtreeme::math::TRS(
        {mesh.position[0], mesh.position[1], mesh.position[2]},
        ixtreeme::math::FromEulerRadians({mesh.rotation[0], mesh.rotation[1], mesh.rotation[2]}),
        {mesh.scale[0], mesh.scale[1], mesh.scale[2]});
    const ixtreeme::math::Aabb worldBounds = ixtreeme::math::TransformAabb(
        {center - extent, center + extent},
        transform);
    return SpatialAabbCorners(worldBounds);
}

SpatialIndex::Aabb StaticMeshWorldAabb(const MeshSceneEntity& mesh, const StaticMeshRenderer& renderer)
{
    const auto corners = BuildStaticMeshWorldAabbCorners(mesh, renderer);
    SpatialIndex::Aabb bounds = ixtreeme::math::EmptyAabb();
    for (const WorldVec3& corner : corners)
        bounds = ixtreeme::math::Expand(bounds, corner);
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
    return ixtreeme::math::ExtractFrustumVulkan(camera.viewProjection);
}

bool WorldAabbOutsideCameraFrustum(const WorldCamera& camera, const std::array<WorldVec3, 8>& corners)
{
    ixtreeme::math::Aabb bounds = ixtreeme::math::EmptyAabb();
    for (const WorldVec3& corner : corners)
        bounds = ixtreeme::math::Expand(bounds, corner);
    return !ixtreeme::math::Intersects(ixtreeme::math::ExtractFrustumVulkan(camera.viewProjection), bounds);
}

bool RayIntersectsAabb(const WorldVec3& origin,
                       const WorldVec3& direction,
                       const SpatialIndex::Aabb& bounds,
                       float& outT)
{
    return ixtreeme::math::IntersectRayAabb({origin, direction}, bounds, outT);
}
