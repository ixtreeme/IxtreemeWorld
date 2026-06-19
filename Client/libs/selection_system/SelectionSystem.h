#pragma once

#include "MapEditorTypes.h"
#include "MeshSystem.h"
#include "SelectionOutlineRenderer.h"
#include "TerrainEditorSystem.h"
#include "WorldCamera.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <vector>

using EditorGizmoMode = MapEditorGizmoOperation;

enum class SelectedEditorObjectType
{
    None,
    Terrain,
    PointLight,
    SpotLight,
    WaterBody,
    MeshEntity
};

struct SelectedEditorObject
{
    SelectedEditorObjectType type = SelectedEditorObjectType::None;
    std::uint32_t id = 0;
    std::uint64_t flecsEntity = 0;
};

struct SceneGizmoTarget
{
    bool visible = false;
    HierarchyEntityType type = HierarchyEntityType::None;
    std::uint32_t id = 0;
    float position[3] = {0.0f, 0.0f, 0.0f};
    float rotation[3] = {0.0f, 0.0f, 0.0f};
    float scale[3] = {1.0f, 1.0f, 1.0f};
};

std::uint64_t HierarchyObjectKey(HierarchyEntityType type, std::uint32_t id);
SelectedEditorObjectType ToSelectedObjectType(HierarchyEntityType type);
HierarchyEntityType ToHierarchyEntityType(SelectedEditorObjectType type);

WorldVec3 SelectionCameraForward(const WorldCamera& camera);
WorldVec3 SelectionCameraRight(const WorldCamera& camera);
WorldVec3 SelectionCameraUp(const WorldCamera& camera);
WorldVec3 SelectionScreenRayDirection(const WorldCamera& camera,
                                      std::uint32_t width,
                                      std::uint32_t height,
                                      int mouseX,
                                      int mouseY);

template <typename ResolveRuntimePath, typename GetStaticRenderer>
std::optional<std::uint32_t> PickMeshEntity(const std::vector<MeshSceneEntity>& meshes,
                                            const WorldCamera& camera,
                                            std::uint32_t width,
                                            std::uint32_t height,
                                            int mouseX,
                                            int mouseY,
                                            ResolveRuntimePath&& resolveRuntimePath,
                                            GetStaticRenderer&& getStaticRenderer)
{
    const WorldVec3 rayDir = SelectionScreenRayDirection(camera, width, height, mouseX, mouseY);
    std::optional<std::uint32_t> bestId;
    float bestT = std::numeric_limits<float>::max();
    for (const MeshSceneEntity& mesh : meshes)
    {
        if (mesh.editorHidden)
            continue;
        if (mesh.skinned)
        {
            const WorldVec3 position{mesh.position[0], mesh.position[1], mesh.position[2]};
            const WorldVec3 toMesh = WorldSub(position, camera.eye);
            const float t = WorldDot(toMesh, rayDir);
            if (t <= 0.0f || t >= bestT)
                continue;
            const WorldVec3 closest = WorldAdd(camera.eye, WorldScale(rayDir, t));
            const WorldVec3 delta = WorldSub(position, closest);
            const float pickRadius = std::max({1.25f, mesh.scale[0], mesh.scale[1], mesh.scale[2]});
            if (WorldDot(delta, delta) <= pickRadius * pickRadius)
            {
                bestT = t;
                bestId = mesh.id;
            }
            continue;
        }
        StaticMeshRenderer* renderer = getStaticRenderer(resolveRuntimePath(mesh));
        if (!renderer || !renderer->IsLoaded())
            continue;
        float t = 0.0f;
        if (!RayIntersectsAabb(camera.eye, rayDir, StaticMeshWorldAabb(mesh, *renderer), t))
            continue;
        if (t >= 0.0f && t < bestT)
        {
            bestT = t;
            bestId = mesh.id;
        }
    }
    return bestId;
}

template <typename LightT>
std::optional<std::uint32_t> PickDynamicLight(const std::vector<LightT>& lights,
                                             const WorldCamera& camera,
                                             std::uint32_t width,
                                             std::uint32_t height,
                                             int mouseX,
                                             int mouseY)
{
    const WorldVec3 rayDir = SelectionScreenRayDirection(camera, width, height, mouseX, mouseY);
    std::optional<std::uint32_t> bestId;
    float bestT = 1000000.0f;
    for (const LightT& light : lights)
    {
        if (light.editorHidden)
            continue;
        const WorldVec3 position{light.position[0], light.position[1], light.position[2]};
        const WorldVec3 toLight = WorldSub(position, camera.eye);
        const float t = WorldDot(toLight, rayDir);
        if (t <= 0.0f || t >= bestT)
            continue;
        const WorldVec3 closest = WorldAdd(camera.eye, WorldScale(rayDir, t));
        const WorldVec3 delta = WorldSub(position, closest);
        constexpr float kPickRadius = 0.9f;
        if (WorldDot(delta, delta) <= kPickRadius * kPickRadius)
        {
            bestT = t;
            bestId = light.id;
        }
    }
    return bestId;
}

std::optional<std::uint32_t> PickWaterBody(const std::vector<WaterBody>& bodies,
                                           const WorldCamera& camera,
                                           std::uint32_t width,
                                           std::uint32_t height,
                                           int mouseX,
                                           int mouseY);

using StaticMeshResolver = std::function<StaticMeshRenderer*(const MeshSceneEntity&)>;

std::vector<SelectionOutlineRenderer::Line> BuildSelectionOutlineLines(
    const SelectedEditorObject& selected,
    const std::vector<MeshSceneEntity>& meshes,
    const std::vector<PointLight>& pointLights,
    const std::vector<SpotLight>& spotLights,
    const std::vector<WaterBody>& waterBodies,
    const WorldCamera& camera,
    const StaticMeshResolver& resolveStaticMesh);

std::vector<SelectionOutlineRenderer::Line> BuildEditorLightShapeLines(
    const std::vector<PointLight>& pointLights,
    const std::vector<SpotLight>& spotLights);

SceneGizmoTarget BuildSceneGizmoTarget(const SelectedEditorObject& selected,
                                       const std::vector<MeshSceneEntity>& meshes,
                                       const std::vector<PointLight>& pointLights,
                                       const std::vector<SpotLight>& spotLights,
                                       const std::vector<WaterBody>& waterBodies,
                                       const StaticMeshResolver& resolveStaticMesh);

bool ApplySceneGizmoToMesh(MeshSceneEntity& mesh,
                           const float* position,
                           const float* rotation,
                           const float* scale,
                           StaticMeshRenderer* renderer);
bool ApplySceneGizmoToPointLight(PointLight& light, const float* position, const float* scale);
bool ApplySceneGizmoToSpotLight(SpotLight& light, const float* position, const float* rotation, const float* scale);
bool ApplySceneGizmoToWaterBody(WaterBody& body, const float* position, const float* scale);
