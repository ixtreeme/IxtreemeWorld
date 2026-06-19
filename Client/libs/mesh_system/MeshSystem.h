#pragma once

#include "SpatialIndex.h"
#include "StaticMeshRenderer.h"
#include "WorldCamera.h"

#include <array>
#include <cstdint>

WorldVec3 TransformMeshLocalPoint(const MeshSceneEntity& mesh, WorldVec3 local);

std::array<WorldVec3, 8> BuildStaticMeshWorldAabbCorners(const MeshSceneEntity& mesh,
                                                         const StaticMeshRenderer& renderer);
SpatialIndex::Aabb StaticMeshWorldAabb(const MeshSceneEntity& mesh, const StaticMeshRenderer& renderer);
WorldVec3 StaticMeshLocalBoundsCenter(const StaticMeshRenderer& renderer);
WorldVec3 StaticMeshWorldBoundsCenter(const MeshSceneEntity& mesh, const StaticMeshRenderer& renderer);
std::array<WorldVec3, 8> SpatialAabbCorners(const SpatialIndex::Aabb& bounds);
SpatialIndex::Frustum SpatialFrustumFromCamera(const WorldCamera& camera);
bool WorldAabbOutsideCameraFrustum(const WorldCamera& camera, const std::array<WorldVec3, 8>& corners);
bool RayIntersectsAabb(const WorldVec3& origin,
                       const WorldVec3& direction,
                       const SpatialIndex::Aabb& bounds,
                       float& outT);
