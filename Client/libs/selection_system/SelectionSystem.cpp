#include "SelectionSystem.h"

#include <cmath>

namespace xm = ixtreeme::math;

namespace
{
WorldVec3 SpotLightDirection(const SpotLight& spot)
{
    const float pitch = spot.rotation[0];
    const float yaw = spot.rotation[1];
    const float cosPitch = std::cos(pitch);
    return xm::Normalize(WorldVec3{std::sin(yaw) * cosPitch, std::sin(pitch), std::cos(yaw) * cosPitch});
}

WorldVec3 SafePerpendicular(WorldVec3 direction)
{
    WorldVec3 right = xm::Cross({0.0f, 1.0f, 0.0f}, direction);
    if (xm::Dot(right, right) < 0.0001f)
        right = xm::Cross({1.0f, 0.0f, 0.0f}, direction);
    return xm::Normalize(right);
}

void AddLine(std::vector<SelectionOutlineRenderer::Line>& lines,
             WorldVec3 a,
             WorldVec3 b,
             std::array<float, 4> color)
{
    lines.push_back(SelectionOutlineRenderer::Line{a, b, color});
}

void AddCircle(std::vector<SelectionOutlineRenderer::Line>& lines,
               WorldVec3 center,
               WorldVec3 axisA,
               WorldVec3 axisB,
               float radius,
               std::array<float, 4> color,
               int segments = 32)
{
    WorldVec3 previous{};
    WorldVec3 first{};
    bool hasPrevious = false;
    for (int i = 0; i < segments; ++i)
    {
        const float angle = (static_cast<float>(i) / static_cast<float>(segments)) * xm::TwoPi;
        const WorldVec3 point = center +
            axisA * (std::cos(angle) * radius) +
            axisB * (std::sin(angle) * radius);
        if (!hasPrevious)
            first = point;
        else
            AddLine(lines, previous, point, color);
        previous = point;
        hasPrevious = true;
    }
    if (hasPrevious)
        AddLine(lines, previous, first, color);
}
}

std::uint64_t HierarchyObjectKey(HierarchyEntityType type, std::uint32_t id)
{
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(type)) << 32u) | id;
}

SelectedEditorObjectType ToSelectedObjectType(HierarchyEntityType type)
{
    switch (type)
    {
    case HierarchyEntityType::Terrain: return SelectedEditorObjectType::Terrain;
    case HierarchyEntityType::WaterBody: return SelectedEditorObjectType::WaterBody;
    case HierarchyEntityType::PointLight: return SelectedEditorObjectType::PointLight;
    case HierarchyEntityType::SpotLight: return SelectedEditorObjectType::SpotLight;
    case HierarchyEntityType::MeshEntity: return SelectedEditorObjectType::MeshEntity;
    default: return SelectedEditorObjectType::None;
    }
}

HierarchyEntityType ToHierarchyEntityType(SelectedEditorObjectType type)
{
    switch (type)
    {
    case SelectedEditorObjectType::Terrain: return HierarchyEntityType::Terrain;
    case SelectedEditorObjectType::WaterBody: return HierarchyEntityType::WaterBody;
    case SelectedEditorObjectType::PointLight: return HierarchyEntityType::PointLight;
    case SelectedEditorObjectType::SpotLight: return HierarchyEntityType::SpotLight;
    case SelectedEditorObjectType::MeshEntity: return HierarchyEntityType::MeshEntity;
    default: return HierarchyEntityType::None;
    }
}

WorldVec3 SelectionCameraForward(const WorldCamera& camera)
{
    return xm::Normalize(camera.target - camera.eye);
}

WorldVec3 SelectionCameraRight(const WorldCamera& camera)
{
    return xm::Normalize(xm::Cross({0.0f, 1.0f, 0.0f}, SelectionCameraForward(camera)));
}

WorldVec3 SelectionCameraUp(const WorldCamera& camera)
{
    return xm::Normalize(xm::Cross(SelectionCameraForward(camera), SelectionCameraRight(camera)));
}

WorldVec3 SelectionScreenRayDirection(const WorldCamera& camera,
                                      std::uint32_t width,
                                      std::uint32_t height,
                                      int mouseX,
                                      int mouseY)
{
    const float aspect = height != 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
    const float ndcX = width != 0 ? (2.0f * static_cast<float>(mouseX) / static_cast<float>(width)) - 1.0f : 0.0f;
    const float ndcY = height != 0 ? 1.0f - (2.0f * static_cast<float>(mouseY) / static_cast<float>(height)) : 0.0f;
    constexpr float kTanHalfFov = 0.41421356237f;
    return xm::Normalize(
        SelectionCameraForward(camera) +
        SelectionCameraRight(camera) * (ndcX * aspect * kTanHalfFov) +
        SelectionCameraUp(camera) * (ndcY * kTanHalfFov));
}

std::optional<std::uint32_t> PickWaterBody(const std::vector<WaterBody>& bodies,
                                           const WorldCamera& camera,
                                           std::uint32_t width,
                                           std::uint32_t height,
                                           int mouseX,
                                           int mouseY)
{
    const WorldVec3 rayDir = SelectionScreenRayDirection(camera, width, height, mouseX, mouseY);
    std::optional<std::uint32_t> bestId;
    float bestT = 1000000.0f;
    for (const WaterBody& body : bodies)
    {
        if (body.editorHidden)
            continue;
        if (!body.config.enabled || body.maskWidth == 0 || body.maskHeight == 0 ||
            body.shapeMask.size() != static_cast<std::size_t>(body.maskWidth) * body.maskHeight)
        {
            continue;
        }

        const float denom = rayDir.y;
        if (std::abs(denom) < 0.0001f)
            continue;
        const float t = (body.waterLevelY - camera.eye.y) / denom;
        if (t <= 0.0f || t >= bestT)
            continue;

        const WorldVec3 hit = camera.eye + rayDir * t;
        if (hit.x < body.bboxMin[0] || hit.x > body.bboxMax[0] ||
            hit.z < body.bboxMin[1] || hit.z > body.bboxMax[1])
        {
            continue;
        }

        const float u = (hit.x - body.bboxMin[0]) / std::max(0.001f, body.bboxMax[0] - body.bboxMin[0]);
        const float v = (hit.z - body.bboxMin[1]) / std::max(0.001f, body.bboxMax[1] - body.bboxMin[1]);
        const std::uint32_t mx = std::min(body.maskWidth - 1u, static_cast<std::uint32_t>(u * body.maskWidth));
        const std::uint32_t my = std::min(body.maskHeight - 1u, static_cast<std::uint32_t>(v * body.maskHeight));
        if (body.shapeMask[static_cast<std::size_t>(my) * body.maskWidth + mx] == 0)
            continue;

        bestT = t;
        bestId = body.id;
    }
    return bestId;
}

std::vector<SelectionOutlineRenderer::Line> BuildSelectionOutlineLines(
    const SelectedEditorObject& selected,
    const std::vector<MeshSceneEntity>& meshes,
    const std::vector<PointLight>& pointLights,
    const std::vector<SpotLight>& spotLights,
    const std::vector<WaterBody>& waterBodies,
    const WorldCamera& camera,
    const StaticMeshResolver& resolveStaticMesh)
{
    std::vector<SelectionOutlineRenderer::Line> lines;
    constexpr std::array<float, 4> kOutlineColor{1.0f, 0.61f, 0.07f, 1.0f};
    auto addLine = [&](WorldVec3 a, WorldVec3 b) {
        lines.push_back(SelectionOutlineRenderer::Line{a, b, kOutlineColor});
    };
    auto addCircle = [&](const WorldVec3& center, float radius) {
        const WorldVec3 right = SelectionCameraRight(camera);
        const WorldVec3 up = SelectionCameraUp(camera);
        WorldVec3 previous{};
        bool hasPrevious = false;
        WorldVec3 first{};
        for (int i = 0; i < 32; ++i)
        {
            const float angle = (static_cast<float>(i) / 32.0f) * xm::TwoPi;
            const WorldVec3 point = center +
                right * (std::cos(angle) * radius) +
                up * (std::sin(angle) * radius);
            if (!hasPrevious)
                first = point;
            else
                addLine(previous, point);
            previous = point;
            hasPrevious = true;
        }
        if (hasPrevious)
            addLine(previous, first);
    };

    if (selected.type == SelectedEditorObjectType::MeshEntity)
    {
        auto it = std::find_if(meshes.begin(), meshes.end(),
            [&](const MeshSceneEntity& mesh) { return mesh.id == selected.id; });
        if (it != meshes.end())
        {
            if (it->skinned)
            {
                const float radius = std::max({1.25f, it->scale[0], it->scale[1], it->scale[2]});
                addCircle({it->position[0], it->position[1] + radius, it->position[2]}, radius);
            }
            else if (StaticMeshRenderer* renderer = resolveStaticMesh(*it))
            {
                if (renderer->IsLoaded())
                {
                    const auto corners = BuildStaticMeshWorldAabbCorners(*it, *renderer);
                    constexpr std::array<std::array<int, 2>, 12> kBoxEdges{{
                        {{0, 1}}, {{0, 2}}, {{1, 3}}, {{2, 3}},
                        {{4, 5}}, {{4, 6}}, {{5, 7}}, {{6, 7}},
                        {{0, 4}}, {{1, 5}}, {{2, 6}}, {{3, 7}},
                    }};
                    lines.reserve(kBoxEdges.size());
                    for (const auto& edge : kBoxEdges)
                        addLine(corners[edge[0]], corners[edge[1]]);
                }
            }
        }
    }
    else if (selected.type == SelectedEditorObjectType::PointLight)
    {
        auto it = std::find_if(pointLights.begin(), pointLights.end(),
            [&](const PointLight& light) { return light.id == selected.id; });
        if (it != pointLights.end())
            addCircle({it->position[0], it->position[1], it->position[2]},
                std::max(0.35f, std::min(it->radius, 2.0f)));
    }
    else if (selected.type == SelectedEditorObjectType::SpotLight)
    {
        auto it = std::find_if(spotLights.begin(), spotLights.end(),
            [&](const SpotLight& light) { return light.id == selected.id; });
        if (it != spotLights.end())
            addCircle({it->position[0], it->position[1], it->position[2]},
                std::max(0.45f, std::min(it->radius * 0.3f, 2.5f)));
    }
    else if (selected.type == SelectedEditorObjectType::WaterBody)
    {
        auto it = std::find_if(waterBodies.begin(), waterBodies.end(),
            [&](const WaterBody& body) { return body.id == selected.id; });
        if (it != waterBodies.end())
        {
            const WorldVec3 a{it->bboxMin[0], it->waterLevelY, it->bboxMin[1]};
            const WorldVec3 b{it->bboxMax[0], it->waterLevelY, it->bboxMin[1]};
            const WorldVec3 c{it->bboxMax[0], it->waterLevelY, it->bboxMax[1]};
            const WorldVec3 d{it->bboxMin[0], it->waterLevelY, it->bboxMax[1]};
            addLine(a, b);
            addLine(b, c);
            addLine(c, d);
            addLine(d, a);
        }
    }
    return lines;
}

std::vector<SelectionOutlineRenderer::Line> BuildEditorLightShapeLines(
    const std::vector<PointLight>& pointLights,
    const std::vector<SpotLight>& spotLights)
{
    std::vector<SelectionOutlineRenderer::Line> lines;
    lines.reserve(pointLights.size() * 96 + spotLights.size() * 48);

    for (const PointLight& light : pointLights)
    {
        if (light.editorHidden)
            continue;
        const float intensity = light.enabled ? 1.0f : 0.35f;
        const std::array<float, 4> color{
            std::max(0.25f, light.r) * intensity,
            std::max(0.25f, light.g) * intensity,
            std::max(0.10f, light.b) * intensity,
            0.85f
        };
        const WorldVec3 center{light.position[0], light.position[1], light.position[2]};
        const float markerRadius = std::clamp(light.radius * 0.12f, 0.45f, 2.25f);
        AddCircle(lines, center, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, markerRadius, color, 24);
        AddCircle(lines, center, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, markerRadius, color, 24);
        AddCircle(lines, center, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, markerRadius, color, 24);
        AddLine(lines, center + WorldVec3{-markerRadius * 1.25f, 0.0f, 0.0f},
            center + WorldVec3{markerRadius * 1.25f, 0.0f, 0.0f}, color);
        AddLine(lines, center + WorldVec3{0.0f, -markerRadius * 1.25f, 0.0f},
            center + WorldVec3{0.0f, markerRadius * 1.25f, 0.0f}, color);
        AddLine(lines, center + WorldVec3{0.0f, 0.0f, -markerRadius * 1.25f},
            center + WorldVec3{0.0f, 0.0f, markerRadius * 1.25f}, color);
    }

    for (const SpotLight& light : spotLights)
    {
        if (light.editorHidden)
            continue;
        const float intensity = light.enabled ? 1.0f : 0.35f;
        const std::array<float, 4> color{
            std::max(0.15f, light.r) * intensity,
            std::max(0.15f, light.g) * intensity,
            std::max(0.20f, light.b) * intensity,
            0.90f
        };
        const WorldVec3 apex{light.position[0], light.position[1], light.position[2]};
        const WorldVec3 direction = SpotLightDirection(light);
        const WorldVec3 right = SafePerpendicular(direction);
        const WorldVec3 up = xm::Normalize(xm::Cross(direction, right));
        const float range = std::max(0.25f, light.radius);
        const float outerRadians = xm::DegreesToRadians(std::clamp(light.outerConeDegrees, 1.0f, 90.0f));
        const float baseRadius = std::tan(outerRadians) * range;
        const WorldVec3 baseCenter = apex + direction * range;
        AddCircle(lines, baseCenter, right, up, baseRadius, color, 32);
        for (int i = 0; i < 4; ++i)
        {
            const float angle = (static_cast<float>(i) / 4.0f) * xm::TwoPi + xm::Pi * 0.25f;
            const WorldVec3 rim = baseCenter +
                right * (std::cos(angle) * baseRadius) +
                up * (std::sin(angle) * baseRadius);
            AddLine(lines, apex, rim, color);
        }
        const float iconRadius = std::clamp(range * 0.035f, 0.20f, 0.75f);
        AddCircle(lines, apex, right, up, iconRadius, color, 16);
    }

    return lines;
}

SceneGizmoTarget BuildSceneGizmoTarget(const SelectedEditorObject& selected,
                                       const std::vector<MeshSceneEntity>& meshes,
                                       const std::vector<PointLight>& pointLights,
                                       const std::vector<SpotLight>& spotLights,
                                       const std::vector<WaterBody>& waterBodies,
                                       const StaticMeshResolver& resolveStaticMesh)
{
    SceneGizmoTarget target{};
    if (selected.type == SelectedEditorObjectType::MeshEntity)
    {
        auto it = std::find_if(meshes.begin(), meshes.end(),
            [&](const MeshSceneEntity& mesh) { return mesh.id == selected.id; });
        if (it == meshes.end() || it->editorHidden)
            return target;
        target.visible = true;
        target.type = HierarchyEntityType::MeshEntity;
        target.id = it->id;
        std::copy(std::begin(it->position), std::end(it->position), std::begin(target.position));
        std::copy(std::begin(it->rotation), std::end(it->rotation), std::begin(target.rotation));
        std::copy(std::begin(it->scale), std::end(it->scale), std::begin(target.scale));
        if (!it->skinned)
        {
            if (StaticMeshRenderer* renderer = resolveStaticMesh(*it); renderer && renderer->IsLoaded())
            {
                const WorldVec3 center = StaticMeshWorldBoundsCenter(*it, *renderer);
                target.position[0] = center.x;
                target.position[1] = center.y;
                target.position[2] = center.z;
            }
        }
    }
    else if (selected.type == SelectedEditorObjectType::PointLight)
    {
        auto it = std::find_if(pointLights.begin(), pointLights.end(),
            [&](const PointLight& light) { return light.id == selected.id; });
        if (it == pointLights.end() || it->editorHidden)
            return target;
        target.visible = true;
        target.type = HierarchyEntityType::PointLight;
        target.id = it->id;
        std::copy(std::begin(it->position), std::end(it->position), std::begin(target.position));
        target.scale[0] = target.scale[1] = target.scale[2] = it->radius;
    }
    else if (selected.type == SelectedEditorObjectType::SpotLight)
    {
        auto it = std::find_if(spotLights.begin(), spotLights.end(),
            [&](const SpotLight& light) { return light.id == selected.id; });
        if (it == spotLights.end() || it->editorHidden)
            return target;
        target.visible = true;
        target.type = HierarchyEntityType::SpotLight;
        target.id = it->id;
        std::copy(std::begin(it->position), std::end(it->position), std::begin(target.position));
        std::copy(std::begin(it->rotation), std::end(it->rotation), std::begin(target.rotation));
        target.scale[0] = target.scale[1] = target.scale[2] = it->radius;
    }
    else if (selected.type == SelectedEditorObjectType::WaterBody)
    {
        auto it = std::find_if(waterBodies.begin(), waterBodies.end(),
            [&](const WaterBody& body) { return body.id == selected.id; });
        if (it == waterBodies.end() || it->editorHidden)
            return target;
        target.visible = true;
        target.type = HierarchyEntityType::WaterBody;
        target.id = it->id;
        const WorldVec3 center = WaterBodyCenter(*it);
        target.position[0] = center.x;
        target.position[1] = center.y;
        target.position[2] = center.z;
        target.scale[0] = WaterBodyWidth(*it);
        target.scale[1] = 1.0f;
        target.scale[2] = WaterBodyDepth(*it);
    }
    return target;
}

bool ApplySceneGizmoToMesh(MeshSceneEntity& mesh,
                           const float* position,
                           const float* rotation,
                           const float* scale,
                           StaticMeshRenderer* renderer)
{
    if (!position || !rotation || !scale)
        return false;

    MeshSceneEntity updated = mesh;
    std::copy(rotation, rotation + 3, std::begin(updated.rotation));
    std::copy(scale, scale + 3, std::begin(updated.scale));
    for (float& value : updated.scale)
        value = std::max(0.001f, value);
    if (renderer && renderer->IsLoaded())
    {
        MeshSceneEntity centerOffsetMesh = updated;
        centerOffsetMesh.position[0] = 0.0f;
        centerOffsetMesh.position[1] = 0.0f;
        centerOffsetMesh.position[2] = 0.0f;
        const WorldVec3 centerOffset = TransformMeshLocalPoint(
            centerOffsetMesh,
            StaticMeshLocalBoundsCenter(*renderer));
        updated.position[0] = position[0] - centerOffset.x;
        updated.position[1] = position[1] - centerOffset.y;
        updated.position[2] = position[2] - centerOffset.z;
    }
    else
    {
        std::copy(position, position + 3, std::begin(updated.position));
    }
    mesh = updated;
    return true;
}

bool ApplySceneGizmoToPointLight(PointLight& light, const float* position, const float* scale)
{
    if (!position || !scale)
        return false;
    std::copy(position, position + 3, std::begin(light.position));
    const float radius = (scale[0] + scale[1] + scale[2]) / 3.0f;
    light.radius = std::clamp(radius, 0.5f, 100.0f);
    return true;
}

bool ApplySceneGizmoToSpotLight(SpotLight& light, const float* position, const float* rotation, const float* scale)
{
    if (!position || !rotation || !scale)
        return false;
    std::copy(position, position + 3, std::begin(light.position));
    std::copy(rotation, rotation + 3, std::begin(light.rotation));
    const float radius = (scale[0] + scale[1] + scale[2]) / 3.0f;
    light.radius = std::clamp(radius, 0.5f, 100.0f);
    return true;
}

bool ApplySceneGizmoToWaterBody(WaterBody& body, const float* position, const float* scale)
{
    if (!position || !scale)
        return false;
    WaterBodyEditorState state{};
    state.name = body.name;
    state.materialId = body.materialId;
    state.config = body.config;
    std::copy(position, position + 3, std::begin(state.center));
    state.width = std::clamp(scale[0], 1.0f, 200.0f);
    state.depth = std::clamp(scale[2], 1.0f, 200.0f);
    ApplyWaterBodyEditorStateToBody(body, state);
    return true;
}
