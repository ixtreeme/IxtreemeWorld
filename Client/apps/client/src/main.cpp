#if defined(_WIN32)
#include <windows.h>
#endif

#include "WorldLabelRenderer.h"
#include "NativeWindow.h"
#include "EditorImGui.h"
#if defined(_WIN32)
#include "NativeWindow_Win32.h"
#endif
#if defined(__ANDROID__)
#include "NativeWindow_Android.h"
#endif
#include "OffscreenSceneRenderer.h"
#include "ProjectManager.h"
#include "RmlUiLayer.h"
#include "RuntimeSession.h"
#include "RuntimeUiAdapter.h"
#include "SceneManager.h"
#include "SpatialIndex.h"
#include "StaticMeshRenderer.h"
#include "TerrainRenderer.h"
#include "VulkanDevice.h"
#include "SkinnedMeshRenderer.h"
#include "Debug.h"
#include "asset/IAssetReader.h"

#include <flecs.h>

#if defined(_WIN32)
#include "asset/FileAssetReader.h"
#endif
#if defined(__ANDROID__)
#include "asset/AAssetManagerAssetReader.h"
#include <android_native_app_glue.h>
#endif

#include <array>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
const char* InputEventTypeName(InputEvent::Type type)
{
    switch (type)
    {
    case InputEvent::MouseMove: return "MouseMove";
    case InputEvent::MouseDown: return "MouseDown";
    case InputEvent::MouseUp: return "MouseUp";
    case InputEvent::MouseWheel: return "MouseWheel";
    case InputEvent::KeyDown: return "KeyDown";
    case InputEvent::KeyUp: return "KeyUp";
    case InputEvent::Char: return "Char";
    case InputEvent::TouchDown: return "TouchDown";
    case InputEvent::TouchMove: return "TouchMove";
    case InputEvent::TouchUp: return "TouchUp";
    default: return "Unknown";
    }
}

const char* SculptDiagToolModeName(MapEditorToolMode mode)
{
    switch (mode)
    {
    case MapEditorToolMode::Heightmap: return "sculpt";
    case MapEditorToolMode::SplatPaint: return "splat";
    case MapEditorToolMode::WaterSculpt: return "water";
    case MapEditorToolMode::None:
    default: return "none";
    }
}

void LogUnhandledInput(const InputEvent& event)
{
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "Input not consumed by gameClient: %s\n",
        InputEventTypeName(event.type));
#if defined(_WIN32)
    OutputDebugStringA(buffer);
#endif
    std::fprintf(stderr, "%s", buffer);
}

void ShowFatal(const char* message)
{
#if defined(_WIN32)
    MessageBoxA(nullptr, message, "VulkanClear", MB_ICONERROR);
#else
    std::fprintf(stderr, "%s\n", message);
#endif
}

float HeadingFromQuantized(std::uint16_t heading)
{
    constexpr float kTwoPi = 6.28318530717958647692f;
    return (static_cast<float>(heading) / 65535.0f) * kTwoPi;
}

WorldVec3 ServerMetersToDisplay(RuntimeVec3 position)
{
    return {position.x, position.z, -position.y};
}

RuntimeVec3 DisplayToServerMeters(WorldVec3 position)
{
    return {position.x, -position.z, position.y};
}

bool ProjectWorldToScreen(const WorldCamera& camera,
                          WorldVec3 world,
                          uint32_t width,
                          uint32_t height,
                          float& outX,
                          float& outY)
{
    const auto& m = camera.viewProjection.m;
    const float clipX = world.x * m[0] + world.y * m[4] + world.z * m[8] + m[12];
    const float clipY = world.x * m[1] + world.y * m[5] + world.z * m[9] + m[13];
    const float clipW = world.x * m[3] + world.y * m[7] + world.z * m[11] + m[15];
    if (clipW <= 0.0001f)
        return false;

    const float ndcX = clipX / clipW;
    const float ndcY = clipY / clipW;
    if (ndcX < -1.2f || ndcX > 1.2f || ndcY < -1.2f || ndcY > 1.2f)
        return false;

    outX = (ndcX * 0.5f + 0.5f) * static_cast<float>(width);
    outY = (1.0f - (ndcY * 0.5f + 0.5f)) * static_cast<float>(height);
    return true;
}

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

std::uint64_t HashLodConfig(const LodConfig& config)
{
    std::uint64_t hash = 1469598103934665603ull;
    auto mix = [&](std::uint64_t value) {
        hash ^= value;
        hash *= 1099511628211ull;
    };
    mix(std::clamp(config.levelCount, 1u, LodConfig::MaxLevels));
    mix(static_cast<std::uint32_t>(std::max(0.0f, config.hysteresisMeters) * 100.0f));
    for (std::uint32_t i = 0; i < LodConfig::MaxLevels; ++i)
    {
        mix(static_cast<std::uint32_t>(std::clamp(config.targetRatios[i], 0.001f, 1.0f) * 100000.0f));
        mix(static_cast<std::uint32_t>(std::max(0.0f, config.distances[i]) * 100.0f));
    }
    return hash == 0 ? 1 : hash;
}

float DistanceToAabb(const WorldVec3& point, const SpatialIndex::Aabb& bounds)
{
    const float dx = std::max({bounds.min.x - point.x, 0.0f, point.x - bounds.max.x});
    const float dy = std::max({bounds.min.y - point.y, 0.0f, point.y - bounds.max.y});
    const float dz = std::max({bounds.min.z - point.z, 0.0f, point.z - bounds.max.z});
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

std::uint32_t SelectLodLevel(const LodConfig& config, float distanceMeters, std::uint32_t previousLevel)
{
    const std::uint32_t levelCount = std::clamp(config.levelCount, 1u, LodConfig::MaxLevels);
    std::uint32_t selected = 0;
    for (std::uint32_t level = 1; level < levelCount; ++level)
    {
        if (distanceMeters >= config.distances[level])
            selected = level;
    }

    const float hysteresis = std::max(0.0f, config.hysteresisMeters);
    if (previousLevel < levelCount && previousLevel != selected && hysteresis > 0.0f)
    {
        if (previousLevel < selected && distanceMeters < config.distances[selected] + hysteresis)
            return previousLevel;
        if (previousLevel > selected && distanceMeters > config.distances[previousLevel] - hysteresis)
            return previousLevel;
    }
    return selected;
}

struct StaticMeshLodBatchKey
{
    StaticMeshRenderer* renderer = nullptr;
    std::uint64_t configHash = 0;
    std::uint32_t lodLevel = 0;

    bool operator==(const StaticMeshLodBatchKey& rhs) const
    {
        return renderer == rhs.renderer && configHash == rhs.configHash && lodLevel == rhs.lodLevel;
    }
};

struct StaticMeshLodBatchKeyHash
{
    std::size_t operator()(const StaticMeshLodBatchKey& key) const
    {
        std::size_t hash = std::hash<StaticMeshRenderer*>{}(key.renderer);
        hash ^= std::hash<std::uint64_t>{}(key.configHash) + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
        hash ^= std::hash<std::uint32_t>{}(key.lodLevel) + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
        return hash;
    }
};

struct StaticMeshLodBatch
{
    LodConfig config;
    std::vector<StaticMeshRenderer::Instance> instances;
    struct LodDispositionRecord
    {
        std::uint32_t entityId = 0;
        float distance = 0.0f;
        std::uint32_t previousLevel = 0;
        std::uint32_t selectedLevel = 0;
        std::uint32_t levelCount = 0;
        LodConfig configSnapshot;
        bool overrideEnabled = false;
        bool bufferValid = false;
        const char* bufferSource = "none";
        std::size_t selectedVertices = 0;
        std::size_t selectedIndices = 0;
        SpatialIndex::Aabb bounds{};
        const char* bboxSource = "entity";
        bool culled = false;
        const char* cullReason = "none";
        bool submitted = false;
        bool fullResFallback = false;
        std::uint32_t drawIndexCount = 0;
    };
    std::vector<LodDispositionRecord> lodDispositionRecords;
};

struct LodDispositionState
{
    std::uint32_t selectedLevel = std::numeric_limits<std::uint32_t>::max();
    bool bufferValid = false;
    bool culled = false;
    bool submitted = false;
    bool fullResFallback = false;
    std::string disposition;
};

struct LodCfgLogState
{
    std::uint32_t levelCount = 0;
    std::array<float, LodConfig::MaxLevels> ratios{};
    std::array<float, LodConfig::MaxLevels> distances{};
    bool overrideEnabled = false;
    bool initialized = false;
};

struct LodPickLogState
{
    std::uint32_t selectedLevel = 0;
    std::uint32_t levelCount = 0;
    std::array<std::size_t, LodConfig::MaxLevels> levelTris{};
    std::size_t selectedTris = 0;
    bool selectedBufferValid = false;
    std::string source;
    bool initialized = false;
};

struct MPerfMainState
{
    std::size_t drawcalls = 0;
    std::size_t tris = 0;
    bool initialized = false;
};

struct MPerfOverrideState
{
    std::size_t uniformUpdates = 0;
    std::size_t activeOverrideDraws = 0;
    bool initialized = false;
};

struct MPerfMeshesState
{
    std::size_t total = 0;
    std::size_t culled = 0;
    std::size_t drawn = 0;
    bool initialized = false;
};

struct InstSummaryState
{
    std::size_t batches = 0;
    std::size_t draws = 0;
    std::size_t instances = 0;
    std::size_t maxBatch = 0;
    bool initialized = false;
};

struct InstBufferState
{
    std::size_t bytes = 0;
    bool initialized = false;
};

std::uint32_t PickRenderEntityTarget(const std::vector<WorldRenderEntity>& entities,
                            const WorldCamera& camera,
                            uint32_t width,
                            uint32_t height,
                            int mouseX,
                            int mouseY)
{
    constexpr float kPickRadiusPixels = 74.0f;
    float bestDistanceSq = kPickRadiusPixels * kPickRadiusPixels;
    std::uint32_t bestNetId = 0;
    for (const auto& entity : entities)
    {
        if (entity.visualClassId == 0)
            continue;

        WorldVec3 screenAnchor = WorldAdd(ServerMetersToDisplay(entity.position), {0.0f, 1.4f, 0.0f});
        float sx = 0.0f;
        float sy = 0.0f;
        if (!ProjectWorldToScreen(camera, screenAnchor, width, height, sx, sy))
            continue;

        const float dx = sx - static_cast<float>(mouseX);
        const float dy = sy - static_cast<float>(mouseY);
        const float distanceSq = dx * dx + dy * dy;
        if (distanceSq < bestDistanceSq)
        {
            bestDistanceSq = distanceSq;
            bestNetId = entity.netId;
        }
    }
    return bestNetId;
}

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

std::string EditorDisplayName(const WaterBody& body)
{
    return body.name.empty() ? "Water Body " + std::to_string(body.id) : body.name;
}

std::string EditorDisplayName(const PointLight& light)
{
    return light.name.empty() ? "Point Light " + std::to_string(light.id) : light.name;
}

std::string EditorDisplayName(const SpotLight& light)
{
    return light.name.empty() ? "Spot Light " + std::to_string(light.id) : light.name;
}

std::string EditorDisplayName(const MeshSceneEntity& mesh)
{
    return mesh.name.empty() ? "Mesh Entity " + std::to_string(mesh.id) : mesh.name;
}

std::string EditorDisplayName(const TerrainSceneData& terrain)
{
    return terrain.name.empty() ? "Terrain" : terrain.name;
}

WorldVec3 CameraForward(const WorldCamera& camera)
{
    return WorldNormalize(WorldSub(camera.target, camera.eye));
}

WorldVec3 CameraRight(const WorldCamera& camera)
{
    return WorldNormalize(WorldCross({0.0f, 1.0f, 0.0f}, CameraForward(camera)));
}

WorldVec3 CameraUp(const WorldCamera& camera)
{
    return WorldNormalize(WorldCross(CameraForward(camera), CameraRight(camera)));
}

WorldVec3 ScreenRayDirection(const WorldCamera& camera, uint32_t width, uint32_t height, int mouseX, int mouseY)
{
    const float aspect = height != 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
    const float ndcX = width != 0 ? (2.0f * static_cast<float>(mouseX) / static_cast<float>(width)) - 1.0f : 0.0f;
    const float ndcY = height != 0 ? 1.0f - (2.0f * static_cast<float>(mouseY) / static_cast<float>(height)) : 0.0f;
    constexpr float kTanHalfFov = 0.41421356237f; // tan(45deg / 2)
    return WorldNormalize(WorldAdd(
        WorldAdd(CameraForward(camera), WorldScale(CameraRight(camera), ndcX * aspect * kTanHalfFov)),
        WorldScale(CameraUp(camera), ndcY * kTanHalfFov)));
}

template <typename LightT>
std::optional<std::uint32_t> PickDynamicLight(const std::vector<LightT>& lights,
                                             const WorldCamera& camera,
                                             uint32_t width,
                                             uint32_t height,
                                             int mouseX,
                                             int mouseY)
{
    const WorldVec3 rayDir = ScreenRayDirection(camera, width, height, mouseX, mouseY);
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

void RegenerateCircularWaterMask(WaterBody& body)
{
    body.maskWidth = 32;
    body.maskHeight = 32;
    body.shapeMask.assign(static_cast<std::size_t>(body.maskWidth) * body.maskHeight, 0);

    const float cx = static_cast<float>(body.maskWidth) * 0.5f;
    const float cy = static_cast<float>(body.maskHeight) * 0.5f;
    const float radius = std::min(cx, cy) * 0.95f;
    for (std::uint32_t y = 0; y < body.maskHeight; ++y)
    {
        for (std::uint32_t x = 0; x < body.maskWidth; ++x)
        {
            const float dx = static_cast<float>(x) + 0.5f - cx;
            const float dy = static_cast<float>(y) + 0.5f - cy;
            const bool inside = dx * dx + dy * dy <= radius * radius;
            body.shapeMask[static_cast<std::size_t>(y) * body.maskWidth + x] = inside ? 1u : 0u;
        }
    }
}

WorldVec3 WaterBodyCenter(const WaterBody& body)
{
    return {
        (body.bboxMin[0] + body.bboxMax[0]) * 0.5f,
        body.waterLevelY,
        (body.bboxMin[1] + body.bboxMax[1]) * 0.5f
    };
}

float WaterBodyWidth(const WaterBody& body)
{
    return std::max(0.0f, body.bboxMax[0] - body.bboxMin[0]);
}

float WaterBodyDepth(const WaterBody& body)
{
    return std::max(0.0f, body.bboxMax[1] - body.bboxMin[1]);
}

void ApplyWaterBodyEditorStateToBody(WaterBody& body, const WaterBodyEditorState& state)
{
    const float width = std::clamp(state.width, 1.0f, 200.0f);
    const float depth = std::clamp(state.depth, 1.0f, 200.0f);
    const bool hasValidMask = body.maskWidth > 0 && body.maskHeight > 0 &&
        body.shapeMask.size() == static_cast<std::size_t>(body.maskWidth) * body.maskHeight;
    body.name = state.name.empty() ? ("Water_" + std::to_string(body.id)) : state.name;
    body.materialId = state.materialId;
    body.waterLevelY = state.center[1];
    body.bboxMin[0] = state.center[0] - width * 0.5f;
    body.bboxMax[0] = state.center[0] + width * 0.5f;
    body.bboxMin[1] = state.center[2] - depth * 0.5f;
    body.bboxMax[1] = state.center[2] + depth * 0.5f;
    if (!hasValidMask)
        RegenerateCircularWaterMask(body);
}

WaterBodyEditorState BuildWaterBodyEditorState(const std::vector<WaterBody>& bodies, std::uint32_t selectedId)
{
    WaterBodyEditorState state{};
    state.count = static_cast<std::uint32_t>(bodies.size());
    auto it = std::find_if(bodies.begin(), bodies.end(),
        [selectedId](const WaterBody& body) { return selectedId != 0 && body.id == selectedId; });
    if (it == bodies.end())
        return state;

    state.selected = true;
    state.id = it->id;
    state.name = it->name;
    const WorldVec3 center = WaterBodyCenter(*it);
    state.center[0] = center.x;
    state.center[1] = center.y;
    state.center[2] = center.z;
    state.width = WaterBodyWidth(*it);
    state.depth = WaterBodyDepth(*it);
    state.materialId = it->materialId;
    state.materialName = it->materialId.empty() ? "Inline Water" : it->materialId;
    state.config = it->config;
    state.config.waterLevelY = it->waterLevelY;
    return state;
}

MeshRendererEditorState BuildMeshRendererEditorState(const std::vector<MeshSceneEntity>& meshes,
                                                     std::uint32_t selectedId)
{
    MeshRendererEditorState state{};
    state.count = static_cast<std::uint32_t>(meshes.size());
    auto it = std::find_if(meshes.begin(), meshes.end(),
        [selectedId](const MeshSceneEntity& mesh) { return selectedId != 0 && mesh.id == selectedId; });
    if (it == meshes.end())
        return state;

    state.selected = true;
    state.id = it->id;
    state.name = EditorDisplayName(*it);
    state.meshAssetId = it->meshAssetId;
    state.meshAssetPath = it->meshAssetPath;
    state.meshDisplayName = it->meshAssetId.empty() ? it->meshAssetPath : it->meshAssetId;
    std::copy(std::begin(it->position), std::end(it->position), std::begin(state.position));
    std::copy(std::begin(it->rotation), std::end(it->rotation), std::begin(state.rotation));
    std::copy(std::begin(it->scale), std::end(it->scale), std::begin(state.scale));
    state.skinned = it->skinned;
    state.materialOverrides = it->materialOverrides;
    state.editorComponents = it->editorComponents;
    state.lod = it->lod;
    state.materialSlotCount = std::max<std::uint32_t>(1u, static_cast<std::uint32_t>(state.materialOverrides.size()));
    state.selectedMaterialSlot = std::min(state.selectedMaterialSlot, state.materialSlotCount - 1u);
    return state;
}

void ApplyMeshRendererEditorState(MeshSceneEntity& mesh, const MeshRendererEditorState& state)
{
    mesh.name = state.name;
    mesh.meshAssetId = state.meshAssetId;
    mesh.meshAssetPath = state.meshAssetPath;
    std::copy(std::begin(state.position), std::end(state.position), std::begin(mesh.position));
    std::copy(std::begin(state.rotation), std::end(state.rotation), std::begin(mesh.rotation));
    std::copy(std::begin(state.scale), std::end(state.scale), std::begin(mesh.scale));
    mesh.skinned = state.skinned;
    mesh.materialOverrides = state.materialOverrides;
    mesh.editorComponents = state.editorComponents;
    mesh.lod = state.lod;
}

std::optional<std::uint32_t> PickWaterBody(const std::vector<WaterBody>& bodies,
                                           const WorldCamera& camera,
                                           uint32_t width,
                                           uint32_t height,
                                           int mouseX,
                                           int mouseY)
{
    const WorldVec3 rayDir = ScreenRayDirection(camera, width, height, mouseX, mouseY);
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

        const WorldVec3 hit = WorldAdd(camera.eye, WorldScale(rayDir, t));
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

std::optional<WorldVec3> RaycastTerrainPoint(const TerrainRenderer& terrain,
                                             const WorldCamera& camera,
                                             uint32_t width,
                                             uint32_t height,
                                             int mouseX,
                                             int mouseY)
{
    if (width == 0 || height == 0)
        return std::nullopt;

    const WorldVec3 rayDir = ScreenRayDirection(camera, width, height, mouseX, mouseY);
    constexpr float kStepMeters = 0.5f;
    constexpr float kMaxDistanceMeters = 700.0f;
    float previousT = 0.0f;
    float previousDelta = camera.eye.y - terrain.SampleHeight(camera.eye);
    for (float t = kStepMeters; t <= kMaxDistanceMeters; t += kStepMeters)
    {
        const WorldVec3 p = WorldAdd(camera.eye, WorldScale(rayDir, t));
        const float terrainY = terrain.SampleHeight(p);
        const float delta = p.y - terrainY;
        if (delta <= 0.0f && previousDelta > 0.0f)
        {
            const float denom = previousDelta - delta;
            const float lerp = denom > 0.0001f ? previousDelta / denom : 0.0f;
            const float hitT = previousT + (t - previousT) * std::clamp(lerp, 0.0f, 1.0f);
            WorldVec3 hit = WorldAdd(camera.eye, WorldScale(rayDir, hitT));
            hit.y = terrain.SampleHeight(hit);
            return hit;
        }
        previousT = t;
        previousDelta = delta;
    }
    return std::nullopt;
}

float SnapValue(float value, float step)
{
    if (step <= 0.0001f)
        return value;
    return std::round(value / step) * step;
}

WorldVec3 SnapPoint(WorldVec3 point, float step)
{
    point.x = SnapValue(point.x, step);
    point.y = SnapValue(point.y, step);
    point.z = SnapValue(point.z, step);
    return point;
}

bool ExpandWaterBodyForSculpt(WaterBody& body, const WorldVec3& point, float radiusMeters)
{
    if (body.maskWidth == 0 || body.maskHeight == 0 ||
        body.shapeMask.size() != static_cast<std::size_t>(body.maskWidth) * body.maskHeight)
    {
        return false;
    }

    const float minX = std::min(body.bboxMin[0], body.bboxMax[0]);
    const float maxX = std::max(body.bboxMin[0], body.bboxMax[0]);
    const float minZ = std::min(body.bboxMin[1], body.bboxMax[1]);
    const float maxZ = std::max(body.bboxMin[1], body.bboxMax[1]);
    const float brushRadius = std::max(radiusMeters, 0.001f);
    const float nextMinX = std::min(minX, point.x - brushRadius);
    const float nextMaxX = std::max(maxX, point.x + brushRadius);
    const float nextMinZ = std::min(minZ, point.z - brushRadius);
    const float nextMaxZ = std::max(maxZ, point.z + brushRadius);
    if (std::abs(nextMinX - minX) < 0.001f && std::abs(nextMaxX - maxX) < 0.001f &&
        std::abs(nextMinZ - minZ) < 0.001f && std::abs(nextMaxZ - maxZ) < 0.001f)
    {
        return true;
    }

    const float oldSizeX = std::max(maxX - minX, 0.001f);
    const float oldSizeZ = std::max(maxZ - minZ, 0.001f);
    const float cellX = oldSizeX / static_cast<float>(body.maskWidth);
    const float cellZ = oldSizeZ / static_cast<float>(body.maskHeight);
    const float targetCell = std::max(0.1f, std::min(cellX, cellZ));
    const float newSizeX = std::max(nextMaxX - nextMinX, targetCell);
    const float newSizeZ = std::max(nextMaxZ - nextMinZ, targetCell);
    const std::uint32_t newWidth = std::clamp(
        static_cast<std::uint32_t>(std::ceil(newSizeX / targetCell)), 8u, 256u);
    const std::uint32_t newHeight = std::clamp(
        static_cast<std::uint32_t>(std::ceil(newSizeZ / targetCell)), 8u, 256u);
    std::vector<std::uint8_t> nextMask(static_cast<std::size_t>(newWidth) * newHeight, 0u);
    const std::vector<std::uint8_t> oldMask = body.shapeMask;

    for (std::uint32_t y = 0; y < newHeight; ++y)
    {
        const float worldZ = nextMinZ + (static_cast<float>(y) + 0.5f) / static_cast<float>(newHeight) * newSizeZ;
        if (worldZ < minZ || worldZ > maxZ)
            continue;
        const float oldV = (worldZ - minZ) / oldSizeZ;
        const std::uint32_t oldY = std::min(body.maskHeight - 1u,
            static_cast<std::uint32_t>(std::clamp(oldV, 0.0f, 0.9999f) * static_cast<float>(body.maskHeight)));
        for (std::uint32_t x = 0; x < newWidth; ++x)
        {
            const float worldX = nextMinX + (static_cast<float>(x) + 0.5f) / static_cast<float>(newWidth) * newSizeX;
            if (worldX < minX || worldX > maxX)
                continue;
            const float oldU = (worldX - minX) / oldSizeX;
            const std::uint32_t oldX = std::min(body.maskWidth - 1u,
                static_cast<std::uint32_t>(std::clamp(oldU, 0.0f, 0.9999f) * static_cast<float>(body.maskWidth)));
            nextMask[static_cast<std::size_t>(y) * newWidth + x] =
                oldMask[static_cast<std::size_t>(oldY) * body.maskWidth + oldX];
        }
    }

    body.bboxMin[0] = nextMinX;
    body.bboxMax[0] = nextMaxX;
    body.bboxMin[1] = nextMinZ;
    body.bboxMax[1] = nextMaxZ;
    body.maskWidth = newWidth;
    body.maskHeight = newHeight;
    body.shapeMask = std::move(nextMask);
    return true;
}

std::uint32_t ApplyWaterSculptBrush(WaterBody& body, const WorldVec3& point, float radiusMeters, bool addMode)
{
    if (body.maskWidth == 0 || body.maskHeight == 0 ||
        body.shapeMask.size() != static_cast<std::size_t>(body.maskWidth) * body.maskHeight)
    {
        return 0;
    }

    if (addMode && !ExpandWaterBodyForSculpt(body, point, radiusMeters))
        return 0;

    const float minX = std::min(body.bboxMin[0], body.bboxMax[0]);
    const float maxX = std::max(body.bboxMin[0], body.bboxMax[0]);
    const float minZ = std::min(body.bboxMin[1], body.bboxMax[1]);
    const float maxZ = std::max(body.bboxMin[1], body.bboxMax[1]);
    const float sizeX = std::max(maxX - minX, 0.001f);
    const float sizeZ = std::max(maxZ - minZ, 0.001f);
    if (point.x < minX || point.x > maxX || point.z < minZ || point.z > maxZ)
        return 0;

    const float u = (point.x - minX) / sizeX;
    const float v = (point.z - minZ) / sizeZ;
    const int centerX = static_cast<int>(std::clamp(u, 0.0f, 0.9999f) * static_cast<float>(body.maskWidth));
    const int centerY = static_cast<int>(std::clamp(v, 0.0f, 0.9999f) * static_cast<float>(body.maskHeight));
    const float radiusPxX = (std::max(radiusMeters, 0.001f) / sizeX) * static_cast<float>(body.maskWidth);
    const float radiusPxY = (std::max(radiusMeters, 0.001f) / sizeZ) * static_cast<float>(body.maskHeight);
    const float radiusPx = std::max(1.0f, (radiusPxX + radiusPxY) * 0.5f);
    const int radiusCeil = static_cast<int>(std::ceil(radiusPx));
    const int xMin = std::max(0, centerX - radiusCeil);
    const int xMax = std::min(static_cast<int>(body.maskWidth) - 1, centerX + radiusCeil);
    const int yMin = std::max(0, centerY - radiusCeil);
    const int yMax = std::min(static_cast<int>(body.maskHeight) - 1, centerY + radiusCeil);
    const std::uint8_t value = addMode ? 1u : 0u;

    std::uint32_t modified = 0;
    const float radiusSq = radiusPx * radiusPx;
    for (int y = yMin; y <= yMax; ++y)
    {
        for (int x = xMin; x <= xMax; ++x)
        {
            const float dx = static_cast<float>(x) + 0.5f - static_cast<float>(centerX);
            const float dy = static_cast<float>(y) + 0.5f - static_cast<float>(centerY);
            if (dx * dx + dy * dy > radiusSq)
                continue;
            const std::size_t index = static_cast<std::size_t>(y) * body.maskWidth + static_cast<std::size_t>(x);
            if (body.shapeMask[index] == value)
                continue;
            body.shapeMask[index] = value;
            ++modified;
        }
    }
    return modified;
}

WorldVec3 SpotLightDirection(const SpotLight& spot)
{
    const float pitch = spot.rotation[0];
    const float yaw = spot.rotation[1];
    const float cosPitch = std::cos(pitch);
    return WorldNormalize({std::sin(yaw) * cosPitch, std::sin(pitch), std::cos(yaw) * cosPitch});
}

SkinnedMeshRenderer::MotionState ToSkinnedMeshMotion(RuntimeMoveState state)
{
    switch (state)
    {
    case RuntimeMoveState::Walking:
        return SkinnedMeshRenderer::MotionState::Walk;
    case RuntimeMoveState::Running:
        return SkinnedMeshRenderer::MotionState::Run;
    default:
        return SkinnedMeshRenderer::MotionState::Idle;
    }
}

struct MovementInputState
{
    bool w = false;
    bool a = false;
    bool s = false;
    bool d = false;
    bool space = false;
    bool control = false;
    bool shift = false;

    bool Apply(const InputEvent& event)
    {
        if (event.type != InputEvent::KeyDown && event.type != InputEvent::KeyUp)
            return false;

        const bool pressed = event.type == InputEvent::KeyDown;
        switch (event.key)
        {
        case Key_W: w = pressed; return true;
        case Key_A: a = pressed; return true;
        case Key_S: s = pressed; return true;
        case Key_D: d = pressed; return true;
        case Key_Space: space = pressed; return true;
        case Key_Control: control = pressed; return true;
        case Key_Shift: shift = pressed; return true;
        default: return false;
        }
    }

    void Clear()
    {
        w = false;
        a = false;
        s = false;
        d = false;
        space = false;
        control = false;
        shift = false;
    }

    bool HasDirection() const { return w || a || s || d; }
    bool HasFlyVertical() const { return space || control; }

    float DirectionAngle(float cameraYawRadians) const
    {
        float localX = 0.0f;
        float localZ = 0.0f;
        if (w) localZ += 1.0f;
        if (s) localZ -= 1.0f;
        if (d) localX += 1.0f;
        if (a) localX -= 1.0f;

        const float length = std::sqrt(localX * localX + localZ * localZ);
        if (length > 0.0001f)
        {
            localX /= length;
            localZ /= length;
        }

        const WorldVec3 forward = {std::sin(cameraYawRadians), 0.0f, std::cos(cameraYawRadians)};
        const WorldVec3 right = {std::cos(cameraYawRadians), 0.0f, -std::sin(cameraYawRadians)};
        const WorldVec3 displayDir = WorldAdd(WorldScale(right, localX), WorldScale(forward, localZ));
        return std::atan2(displayDir.x, -displayDir.z);
    }

    RuntimeMoveState State() const
    {
        if (!HasDirection())
            return RuntimeMoveState::Idle;
        return shift ? RuntimeMoveState::Running : RuntimeMoveState::Walking;
    }
};

class FlyCameraController
{
public:
    struct Snapshot
    {
        WorldVec3 eye{};
        float yaw = 0.0f;
        float pitch = 0.0f;
    };

    void SetFreeCameraEnabled(bool enabled)
    {
        if (enabled == inputEnabled_)
            return;

        inputEnabled_ = enabled;
        dragActive_ = false;
        Tracenf("[CAMERA] mode=free enabled=%d", inputEnabled_ ? 1 : 0);
    }

    bool HandleInput(const InputEvent& event)
    {
        if (!inputEnabled_)
            return false;

        switch (event.type)
        {
        case InputEvent::MouseDown:
            dragActive_ = true;
            lastMouseX_ = event.x;
            lastMouseY_ = event.y;
            return true;
        case InputEvent::MouseUp:
            dragActive_ = false;
            return true;
        case InputEvent::MouseMove:
            if (!dragActive_)
                return false;
            ApplyMouseDelta(static_cast<float>(event.x - lastMouseX_),
                static_cast<float>(event.y - lastMouseY_));
            lastMouseX_ = event.x;
            lastMouseY_ = event.y;
            return true;
        case InputEvent::MouseWheel:
            speedScale_ = std::clamp(speedScale_ + static_cast<float>(event.wheelDelta) / 120.0f * 0.15f, 0.25f, 4.0f);
            return true;
        default:
            return false;
        }
    }

    void Update(double dt, const MovementInputState& movement)
    {
        if (!inputEnabled_)
            return;
        const float frameDt = static_cast<float>(std::clamp(dt, 0.0, 0.05));
        UpdateFly(frameDt, movement);
    }

    WorldCamera BuildCamera(uint32_t width, uint32_t height) const
    {
        return BuildFlyCamera(width, height);
    }

    bool IsFreeCameraEnabled() const { return inputEnabled_; }
    float MovementYaw() const { return yaw_; }
    Snapshot SaveSnapshot() const
    {
        return Snapshot{eye_, yaw_, pitch_};
    }
    void RestoreSnapshot(const Snapshot& snapshot)
    {
        eye_ = snapshot.eye;
        yaw_ = snapshot.yaw;
        pitch_ = snapshot.pitch;
        dragActive_ = false;
    }

    void FocusOn(WorldVec3 target, float distance = 15.0f)
    {
        inputEnabled_ = true;
        const WorldVec3 eye = {target.x, target.y + 5.0f, target.z - distance};
        const WorldVec3 forward = WorldNormalize(WorldSub(target, eye));
        eye_ = eye;
        yaw_ = std::atan2(forward.x, forward.z);
        pitch_ = std::clamp(std::asin(std::clamp(forward.y, -1.0f, 1.0f)), -kMaxPitch, kMaxPitch);
        dragActive_ = false;
        Tracenf("[HIERARCHY] Focused camera on target: %.2f, %.2f, %.2f", target.x, target.y, target.z);
    }

private:
    static constexpr float kMaxPitch = 80.0f * 3.1415926535f / 180.0f;

    void ApplyMouseDelta(float dx, float dy)
    {
        constexpr float kSensitivity = 0.0045f;
        yaw_ += dx * kSensitivity;
        pitch_ = std::clamp(pitch_ + dy * kSensitivity, -kMaxPitch, kMaxPitch);
    }

    void UpdateFly(float dt, const MovementInputState& movement)
    {
        float localX = 0.0f;
        float localZ = 0.0f;
        float localY = 0.0f;
        if (movement.w) localZ += 1.0f;
        if (movement.s) localZ -= 1.0f;
        if (movement.d) localX += 1.0f;
        if (movement.a) localX -= 1.0f;
        if (movement.space) localY += 1.0f;
        if (movement.control) localY -= 1.0f;

        const float cosPitch = std::cos(pitch_);
        const WorldVec3 forward = {std::sin(yaw_) * cosPitch, std::sin(pitch_), std::cos(yaw_) * cosPitch};
        const WorldVec3 right = {std::cos(yaw_), 0.0f, -std::sin(yaw_)};
        WorldVec3 delta = WorldAdd(WorldAdd(WorldScale(forward, localZ), WorldScale(right, localX)),
            {0.0f, localY, 0.0f});
        if (WorldDot(delta, delta) > 0.0001f)
            delta = WorldNormalize(delta);

        const float speed = (movement.shift ? 22.0f : 9.0f) * speedScale_;
        eye_ = WorldAdd(eye_, WorldScale(delta, speed * dt));
    }

    WorldCamera BuildFlyCamera(uint32_t width, uint32_t height) const
    {
        const float aspect = height != 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
        const float cosPitch = std::cos(pitch_);
        const WorldVec3 forward = {std::sin(yaw_) * cosPitch, std::sin(pitch_), std::cos(yaw_) * cosPitch};
        WorldCamera camera{};
        camera.eye = eye_;
        camera.target = WorldAdd(eye_, forward);
        const WorldMat4 view = WorldLookAt(camera.eye, camera.target, {0.0f, 1.0f, 0.0f});
        camera.nearPlane = 0.1f;
        camera.farPlane = 1000.0f;
        const WorldMat4 projection = WorldPerspective(45.0f * 3.1415926535f / 180.0f, aspect, camera.nearPlane, camera.farPlane);
        camera.viewProjection = WorldMultiply(view, projection);
        return camera;
    }

    bool inputEnabled_ = true;
    bool dragActive_ = false;
    int lastMouseX_ = 0;
    int lastMouseY_ = 0;
    float yaw_ = 0.0f;
    float pitch_ = -25.0f * 3.1415926535f / 180.0f;
    float speedScale_ = 1.0f;
    WorldVec3 eye_ = {0.0f, 8.0f, -18.0f};
};

std::string ExecutableDirectory()
{
#if defined(_WIN32)
    char path[MAX_PATH]{};
    DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (length == 0 || length == MAX_PATH)
        return ".";

    std::string result(path, length);
    size_t slash = result.find_last_of("\\/");
    return slash == std::string::npos ? "." : result.substr(0, slash);
#else
    return ".";
#endif
}

bool EngineAssetRootLooksValid(const std::filesystem::path& root)
{
    std::error_code ec;
    return std::filesystem::exists(root / "assets" / "shaders" / "composite_ps.spv", ec) &&
        std::filesystem::exists(root / "assets" / "shaders" / "terrain_ps.spv", ec) &&
        std::filesystem::exists(root / "assets" / "shaders" / "rmlui_ps.spv", ec);
}

std::optional<std::filesystem::path> FindClientRootNear(std::filesystem::path start)
{
    std::error_code ec;
    start = std::filesystem::absolute(start, ec);
    if (ec)
        return std::nullopt;
    if (std::filesystem::is_regular_file(start, ec))
        start = start.parent_path();

    for (std::filesystem::path current = start; !current.empty(); current = current.parent_path())
    {
        if (EngineAssetRootLooksValid(current))
            return current;

        const std::filesystem::path clientChild = current / "Client";
        if (EngineAssetRootLooksValid(clientChild))
            return clientChild;

        if (current == current.root_path())
            break;
    }
    return std::nullopt;
}

std::filesystem::path ResolveEngineAssetRoot()
{
    const std::filesystem::path exeDir(ExecutableDirectory());
    std::vector<std::filesystem::path> candidates;

    if (auto clientRoot = FindClientRootNear(exeDir))
        candidates.push_back(*clientRoot);

    std::error_code ec;
    const std::filesystem::path cwd = std::filesystem::current_path(ec);
    if (!ec)
    {
        if (auto clientRoot = FindClientRootNear(cwd))
            candidates.push_back(*clientRoot);
    }

    candidates.push_back(exeDir);

    for (const std::filesystem::path& candidate : candidates)
    {
        if (EngineAssetRootLooksValid(candidate))
        {
            Tracenf("[BOOT] engine asset root = %s", candidate.string().c_str());
            return candidate;
        }
    }

    Tracenf("[BOOT] engine asset root fallback = %s (required shaders not found)", exeDir.string().c_str());
    return exeDir;
}

std::optional<std::string> ExtractJsonStringField(const std::string& text, const char* key)
{
    const std::string quotedKey = "\"" + std::string(key) + "\"";
    size_t keyPos = text.find(quotedKey);
    if (keyPos == std::string::npos)
        return std::nullopt;
    size_t colon = text.find(':', keyPos + quotedKey.size());
    if (colon == std::string::npos)
        return std::nullopt;
    size_t valueStart = text.find('"', colon + 1);
    if (valueStart == std::string::npos)
        return std::nullopt;
    ++valueStart;

    std::string value;
    bool escaped = false;
    for (size_t i = valueStart; i < text.size(); ++i)
    {
        const char ch = text[i];
        if (escaped)
        {
            value.push_back(ch);
            escaped = false;
            continue;
        }
        if (ch == '\\')
        {
            escaped = true;
            continue;
        }
        if (ch == '"')
            return value;
        value.push_back(ch);
    }
    return std::nullopt;
}

std::string StartupSceneFromConfig(const client::asset::IAssetReader& assets)
{
    if (auto root = assets.RootPath())
    {
        Tracenf("[BOOT] config probe: %s",
            (*root / "assets" / "app_config.json").string().c_str());
        Tracenf("[BOOT] config probe: %s",
            (*root / "app_config.json").string().c_str());
    }
    std::optional<std::string> configText = assets.ReadText("assets/app_config.json");
    const char* configSource = "assets/app_config.json";
    if (!configText)
    {
        configText = assets.ReadText("app_config.json");
        configSource = "app_config.json";
    }
    if (!configText)
    {
        Tracen("[BOOT] config missing/empty -> fallback = empty runtime");
        return {};
    }

    if (auto root = assets.RootPath())
        Tracenf("[BOOT] config loaded: %s", (*root / configSource).string().c_str());
    else
        Tracenf("[BOOT] config loaded: %s", configSource);

    if (std::optional<std::string> startupScene = ExtractJsonStringField(*configText, "startup_scene");
        startupScene && !startupScene->empty())
    {
        Tracenf("[BOOT] startup_scene = %s", startupScene->c_str());
        return *startupScene;
    }

    Tracen("[BOOT] config missing/empty -> fallback = empty runtime");
    return {};
}

std::filesystem::path ResolveRuntimeScenePath(const client::asset::IAssetReader& assets, const std::string& sceneAssetPath)
{
    std::filesystem::path requested(sceneAssetPath);
    if (requested.is_absolute() && std::filesystem::exists(requested))
        return requested;

    if (auto root = assets.RootPath())
    {
        const std::filesystem::path assetRelative = *root / "assets" / requested;
        if (std::filesystem::exists(assetRelative))
            return assetRelative;
        const std::filesystem::path rootRelative = *root / requested;
        if (std::filesystem::exists(rootRelative))
            return rootRelative;
        return assetRelative;
    }

    return requested;
}

bool LoadRuntimeScene(client::asset::IAssetReader& assets, const std::string& sceneAssetPath)
{
    if (sceneAssetPath.empty())
    {
        Tracen("[BOOT] runtime scene request: <empty>");
        Tracen("[SCENE] no scene loaded (empty runtime startup)");
        return false;
    }
    const std::filesystem::path scenePath = ResolveRuntimeScenePath(assets, sceneAssetPath);
    Tracenf("[BOOT] runtime scene request: %s", sceneAssetPath.c_str());
    Tracenf("[SCENE] load attempt: %s",
        scenePath.string().c_str());
    Tracenf("[SCENE-RUNTIME] Request load: %s -> %s",
        sceneAssetPath.c_str(),
        scenePath.string().c_str());
    return SceneManager::Instance().LoadScene(scenePath.string());
}

void MergeMapEditorCommands(MapEditorCommands& target, const MapEditorCommands& source)
{
    target.save = target.save || source.save;
    target.reload = target.reload || source.reload;
    target.undo = target.undo || source.undo;
    target.enterPlayMode = target.enterPlayMode || source.enterPlayMode;
    target.exitPlayMode = target.exitPlayMode || source.exitPlayMode;
    target.pausePlayMode = target.pausePlayMode || source.pausePlayMode;
    target.resumePlayMode = target.resumePlayMode || source.resumePlayMode;
    target.addWaterBody = target.addWaterBody || source.addWaterBody;
    if (source.createTerrain)
    {
        target.createTerrain = true;
        target.terrainCreate = source.terrainCreate;
    }
    if (source.addMeshEntity)
    {
        target.addMeshEntity = true;
        target.meshAssetId = source.meshAssetId;
        target.meshDropScreenPositionValid = source.meshDropScreenPositionValid;
        target.meshDropScreenPosition[0] = source.meshDropScreenPosition[0];
        target.meshDropScreenPosition[1] = source.meshDropScreenPosition[1];
    }
    if (source.addComponentToSelectedEntity)
    {
        target.addComponentToSelectedEntity = true;
        target.addComponentType = source.addComponentType;
        target.addComponentTypeId = source.addComponentTypeId;
    }
    if (source.removeComponentFromSelectedEntity)
    {
        target.removeComponentFromSelectedEntity = true;
        target.removeComponentTypeId = source.removeComponentTypeId;
    }
    if (source.lodQualityCommitRequested)
    {
        target.lodQualityCommitRequested = true;
        target.lodQualityCommitEntityId = source.lodQualityCommitEntityId;
        target.lodQualityCommitConfig = source.lodQualityCommitConfig;
    }
    if (source.assignMeshAssetToSelectedEntity)
    {
        target.assignMeshAssetToSelectedEntity = true;
        target.assignMeshAssetId = source.assignMeshAssetId;
    }
    target.deleteSelectedWaterBody = target.deleteSelectedWaterBody || source.deleteSelectedWaterBody;
    target.openSelectedWaterMaterialEditor =
        target.openSelectedWaterMaterialEditor || source.openSelectedWaterMaterialEditor;
    if (source.waterMaterialDeleted)
    {
        target.waterMaterialDeleted = true;
        target.deletedWaterMaterialId = source.deletedWaterMaterialId;
    }
    if (source.selectedWaterBodyChanged)
    {
        target.selectedWaterBodyChanged = true;
        target.selectedWaterBody = source.selectedWaterBody;
    }
    target.addPointLight = target.addPointLight || source.addPointLight;
    target.addSpotLight = target.addSpotLight || source.addSpotLight;
    target.deleteSelectedLight = target.deleteSelectedLight || source.deleteSelectedLight;
    if (source.selectedLightChanged)
    {
        target.selectedLightChanged = true;
        target.selectedLight = source.selectedLight;
    }
    target.deleteSelectedMeshEntity = target.deleteSelectedMeshEntity || source.deleteSelectedMeshEntity;
    if (source.selectedMeshEntityChanged)
    {
        target.selectedMeshEntityChanged = true;
        target.selectedMeshEntity = source.selectedMeshEntity;
    }
    if (source.hierarchySelectEntity)
    {
        target.hierarchySelectEntity = true;
        target.hierarchyEntityType = source.hierarchyEntityType;
        target.hierarchyEntityId = source.hierarchyEntityId;
    }
    if (source.hierarchyFocusEntity)
    {
        target.hierarchyFocusEntity = true;
        target.hierarchyEntityType = source.hierarchyEntityType;
        target.hierarchyEntityId = source.hierarchyEntityId;
    }
    if (source.hierarchyDeleteEntity)
    {
        target.hierarchyDeleteEntity = true;
        target.hierarchyEntityType = source.hierarchyEntityType;
        target.hierarchyEntityId = source.hierarchyEntityId;
    }
    if (source.hierarchyDuplicateEntity)
    {
        target.hierarchyDuplicateEntity = true;
        target.hierarchyEntityType = source.hierarchyEntityType;
        target.hierarchyEntityId = source.hierarchyEntityId;
    }
    if (source.hierarchyRenameEntity)
    {
        target.hierarchyRenameEntity = true;
        target.hierarchyEntityType = source.hierarchyEntityType;
        target.hierarchyEntityId = source.hierarchyEntityId;
        target.hierarchyRenameValue = source.hierarchyRenameValue;
    }
    if (source.hierarchyToggleHidden)
    {
        target.hierarchyToggleHidden = true;
        target.hierarchyEntityType = source.hierarchyEntityType;
        target.hierarchyEntityId = source.hierarchyEntityId;
    }
    if (source.paletteSlotChanged)
    {
        target.paletteSlotChanged = true;
        target.paletteSlot = source.paletteSlot;
        target.paletteAssetId = source.paletteAssetId;
        target.paletteTexturePath = source.paletteTexturePath;
        target.paletteSlotData = source.paletteSlotData;
    }
    if (source.paletteSlotParamsChanged)
    {
        target.paletteSlotParamsChanged = true;
        target.paletteSlot = source.paletteSlot;
        target.paletteSlotData = source.paletteSlotData;
    }
    if (source.terrainTriplanarChanged)
    {
        target.terrainTriplanarChanged = true;
        target.terrainTriplanarEnabled = source.terrainTriplanarEnabled;
        target.terrainTriplanarSharpness = source.terrainTriplanarSharpness;
        target.terrainTriplanarSlopeThreshold = source.terrainTriplanarSlopeThreshold;
        target.terrainTriplanarSlopeTransition = source.terrainTriplanarSlopeTransition;
    }
    if (source.gizmoSettingsChanged)
    {
        target.gizmoSettingsChanged = true;
        target.gizmoOperation = source.gizmoOperation;
        target.gizmoSnapEnabled = source.gizmoSnapEnabled;
        target.gizmoSnapValue = source.gizmoSnapValue;
    }
}

struct EditorPlayRuntime
{
    EditorPlayModeState state;
    EditorPlayMode appliedMode = EditorPlayMode::Edit;
    std::optional<FlyCameraController::Snapshot> editorCameraSnapshot;
    std::string playStartScenePath;
    SceneData playStartSceneSnapshot;
    bool playStartSceneWasOpen = false;
    bool playStartSceneDirty = false;
};

std::unique_ptr<RuntimeSession> CreateRuntimeSession()
{
    Tracen("[RUNTIME] active session = EmptyRuntimeSession");
    return CreateEmptyRuntimeSession();
}

std::unique_ptr<RuntimeUiAdapter> CreateRuntimeUiAdapter(RmlUiLayer& rmlUi)
{
    Tracen("[RUNTIME] active UI adapter = NullRuntimeUiAdapter");
    return CreateNullRuntimeUiAdapter(rmlUi);
}

int RunGame(NativeWindow& window,
            client::asset::IAssetReader& assets)
{
    VulkanDevice device;
    if (!device.Create(window, window.GetWidth(), window.GetHeight()))
    {
        ShowFatal("Failed to create Vulkan device. See debug output/stderr.");
        return 1;
    }
#if defined(IXTREEME_WITH_EDITOR)
    Tracen("[BUILD] Editor: ENABLED");
    Tracen("[BOOT] build = EDITOR");
    Tracen("[BOOT] entry state = editor boot, editor UI available, no startup scene auto-load");
#else
    Tracen("[BUILD] Editor: DISABLED");
    Tracen("[BOOT] build = RELEASE");
    Tracen("[BOOT] entry state = release boot, default runtime, no startup scene");
#endif
    Tracenf("[BOOT] window size = %ux%u", window.GetWidth(), window.GetHeight());
    Tracenf("[LOG-CONFIG] quiet_logs_for_lod_diag = %s", QuietLogsForLodDiag() ? "true" : "false");

    VkExtent2D renderSize = device.GetSwapchainExtent();
    Tracenf("[BOOT] swapchain size = %ux%u", renderSize.width, renderSize.height);
    std::unique_ptr<RuntimeSession> runtimeSession = CreateRuntimeSession();
    if (!runtimeSession->Create(device, assets, renderSize.width, renderSize.height))
    {
        ShowFatal("Failed to create runtime session. See debug output/stderr.");
        device.Destroy();
        return 1;
    }

    RmlUiLayer rmlUi;
    if (!rmlUi.Create(device, assets, renderSize.width, renderSize.height))
    {
        ShowFatal("Failed to create RmlUi layer. See debug output/stderr.");
        runtimeSession->Destroy();
        device.Destroy();
        return 1;
    }
    std::unique_ptr<RuntimeUiAdapter> runtimeUi = CreateRuntimeUiAdapter(rmlUi);
    runtimeUi->SetQuitCallback([&window]() {
        window.RequestClose();
    });
    runtimeUi->BindRuntime(*runtimeSession);
#if defined(IXTREEME_WITH_EDITOR)
    runtimeUi->HideAll();
    Tracen("[STARTUP] Editor build starts in editor-only empty scene state");
#endif

    EditorImGui editorImGui;
#if defined(IXTREEME_WITH_EDITOR)
#if defined(_WIN32)
    NativeWindow_Win32* win32Window = dynamic_cast<NativeWindow_Win32*>(&window);
    if (!win32Window || !editorImGui.Create(device, win32Window->GetHwnd()))
    {
        ShowFatal("Failed to create ImGui editor layer. See debug output/stderr.");
        runtimeSession->Destroy();
        device.Destroy();
        return 1;
    }
    SceneManager::Instance().SetWindowTitleCallback([&window](const std::string& title) {
        window.SetTitle(title);
    });
    win32Window->SetMessageCallback([&editorImGui](HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result)
    {
        return editorImGui.HandleWin32Message(hwnd, message, wParam, lParam, result);
    });
#else
    if (!editorImGui.Create(device, nullptr))
    {
        ShowFatal("Failed to create ImGui editor layer. See debug output/stderr.");
        runtimeSession->Destroy();
        device.Destroy();
        return 1;
    }
    SceneManager::Instance().SetWindowTitleCallback([&window](const std::string& title) {
        window.SetTitle(title);
    });
#endif
    editorImGui.SetMapEditorSettings(runtimeSession->GetMapEditorSettings());
    editorImGui.SetLightingState(runtimeSession->GetLightingState());
    if (auto assetRoot = assets.RootPath())
        editorImGui.SetEngineRoot(*assetRoot);
#else
    Tracen("[SCENE] no scene loaded (default runtime release state)");
#endif
    SkinnedMeshRenderer skinnedMesh;
    bool skinnedMeshOk = false;
    std::string loadedSkinnedMeshPath;
    Tracen("[MAIN] SkinnedMeshRenderer available; no default skinned mesh asset loaded");

    TerrainRenderer terrain;
    bool terrainOk = terrain.Create(device, assets);
    auto syncTerrainAssetRoots = [&]() {
        std::vector<std::filesystem::path> roots;
        if (ProjectManager::Instance().HasProject())
            roots.push_back(ProjectManager::Instance().ProjectRoot());
        terrain.SetAdditionalAssetRoots(std::move(roots));
    };
    syncTerrainAssetRoots();
    if (!terrainOk)
    {
        Tracenf("[MAIN] TerrainRenderer failed to initialize - terrain will not be available");
        terrain.Destroy();
    }
    else
    {
        runtimeSession->InitializeAssetLibrary("", terrain.GetPaletteSlots());
#if defined(IXTREEME_WITH_EDITOR)
        editorImGui.SetPaletteSlots(runtimeSession->GetPaletteSlots());
        editorImGui.SetWaterMaterials(editorImGui.GetWaterMaterialsSnapshot());
#endif
        if (!terrain.ApplyPaletteSlots(device, runtimeSession->GetPaletteSlots()))
            Tracenf("[MAIN] world palette could not be applied; keeping initial terrain palette");
    }

    WorldLabelRenderer worldLabels;
    bool worldLabelsOk = worldLabels.Create(device, assets);
    if (!worldLabelsOk)
    {
        Tracenf("[MAIN] WorldLabelRenderer failed to initialize - worldLabels will not be available");
        worldLabels.Destroy();
    }

    OffscreenSceneRenderer offscreenScene;
    bool offscreenSceneOk = offscreenScene.Create(device, assets);
    if (offscreenSceneOk)
    {
        if (skinnedMeshOk)
        {
            skinnedMesh.SetMainRenderPass(offscreenScene.GetRenderPass());
            skinnedMesh.RecreatePipeline(device);
        }
        if (terrainOk)
        {
            terrain.SetMainRenderPass(offscreenScene.GetRenderPass());
            terrain.SetWaterRefractionInputs(offscreenScene.GetSceneColorSnapshotView(),
                offscreenScene.GetSceneDepthSnapshotView(),
                offscreenScene.GetLinearSampler(),
                offscreenScene.GetExtent());
            terrain.RecreatePipeline(device);
        }
#if defined(IXTREEME_WITH_EDITOR)
        editorImGui.SetSceneViewTexture(offscreenScene.GetLinearSampler(),
            offscreenScene.GetSceneColorView(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            offscreenScene.GetExtent());
#endif
    }
    struct StaticMeshCacheEntry
    {
        enum class State
        {
            Unknown,
            LoadedStatic,
            UnsupportedSkinned,
            Failed
        };

        std::unique_ptr<StaticMeshRenderer> renderer;
        State state = State::Unknown;
    };
    std::unordered_map<std::string, StaticMeshCacheEntry> staticMeshCache;
    auto ensureSkinnedMeshLoaded = [&](const std::string& modelPath) {
        if (modelPath.empty())
            return false;
        if (skinnedMeshOk && loadedSkinnedMeshPath == modelPath)
            return true;
        skinnedMesh.Destroy();
        skinnedMeshOk = skinnedMesh.Create(device, assets, modelPath);
        if (skinnedMeshOk)
        {
            loadedSkinnedMeshPath = modelPath;
            if (offscreenSceneOk)
            {
                skinnedMesh.SetMainRenderPass(offscreenScene.GetRenderPass());
                skinnedMesh.RecreatePipeline(device);
            }
            Tracenf("[MESH-ENTITY] SkinnedMeshRenderer loaded: %s", modelPath.c_str());
        }
        else
        {
            loadedSkinnedMeshPath.clear();
            TraceError("[MESH-ENTITY] Failed to load model: %s", modelPath.c_str());
        }
        return skinnedMeshOk;
    };
    auto resolveMeshRuntimePath = [&](const MeshSceneEntity& mesh) {
        if (mesh.meshAssetPath.empty())
            return std::string{};
        const std::filesystem::path stored(mesh.meshAssetPath);
        if (stored.is_absolute())
            return stored.string();
        if (ProjectManager::Instance().HasProject())
        {
            const std::filesystem::path projectPath = ProjectManager::Instance().ProjectRoot() / stored;
            if (std::filesystem::exists(projectPath))
                return projectPath.string();
        }
        if (auto root = assets.RootPath())
        {
            const std::filesystem::path enginePath = *root / stored;
            if (std::filesystem::exists(enginePath))
                return stored.generic_string();
        }
        return mesh.meshAssetPath;
    };
    auto getStaticMeshRenderer = [&](const std::string& modelPath) -> StaticMeshRenderer* {
        if (modelPath.empty())
            return nullptr;
        auto& entry = staticMeshCache[modelPath];
        if (entry.state == StaticMeshCacheEntry::State::LoadedStatic)
            return entry.renderer.get();
        if (entry.state == StaticMeshCacheEntry::State::UnsupportedSkinned ||
            entry.state == StaticMeshCacheEntry::State::Failed)
            return nullptr;

        bool isSkinned = false;
        std::string inspectError;
        if (!StaticMeshRenderer::DetectSkinnedGltf(assets, modelPath, isSkinned, &inspectError))
        {
            entry.state = StaticMeshCacheEntry::State::Failed;
            TraceError("[MESH-ENTITY] Static mesh inspect failed: %s (%s)", modelPath.c_str(), inspectError.c_str());
            return nullptr;
        }
        if (isSkinned)
        {
            entry.state = StaticMeshCacheEntry::State::UnsupportedSkinned;
            Tracenf("[MESH-ENTITY] Skinned glTF detected and skipped for mesh entity static path: %s", modelPath.c_str());
            return nullptr;
        }

        entry.renderer = std::make_unique<StaticMeshRenderer>();
        if (offscreenSceneOk)
            entry.renderer->SetMainRenderPass(offscreenScene.GetRenderPass());
        if (!entry.renderer->Create(device, assets, modelPath))
        {
            entry.renderer.reset();
            entry.state = StaticMeshCacheEntry::State::Failed;
            TraceError("[MESH-ENTITY] Static mesh load failed: %s", modelPath.c_str());
            return nullptr;
        }
        entry.state = StaticMeshCacheEntry::State::LoadedStatic;
        Tracenf("[MESH-ENTITY] StaticMeshRenderer loaded: %s", modelPath.c_str());
        return entry.renderer.get();
    };

    runtimeSession->SetQuitCallback([&window]()
    {
        window.RequestClose();
    });

    MovementInputState movement;
    FlyCameraController cameraController;
#if defined(IXTREEME_WITH_EDITOR)
    EditorPlayRuntime editorPlay;
    runtimeSession->SetMapEditorOpen(true);
    if (terrainOk)
        terrain.SetMapEditorOpen(true);
    cameraController.SetFreeCameraEnabled(true);
    runtimeSession->SetEditorStatus("Editor opened at boot");
    Tracen("[BOOT] editor_open forced = 1 (editor build boot)");
#endif
    std::vector<WorldRenderEntity> lastPickEntities;
    WorldCamera lastPickCamera{};
    bool hasLastPickCamera = false;
    std::uint32_t selectedTargetNetId = 0;
    std::vector<PointLight> editorPointLights;
    std::vector<SpotLight> editorSpotLights;
    std::vector<MeshSceneEntity> editorMeshEntities;
    std::unordered_map<std::uint32_t, std::size_t> editorMeshEntityLookup;
    SpatialIndex staticMeshSpatialIndex;
    std::unordered_set<std::uint32_t> staticMeshSpatialIndexed;
    std::unordered_map<std::uint32_t, std::uint32_t> staticMeshSelectedLods;
    std::unordered_map<std::uint32_t, LodDispositionState> staticMeshLodDispositionStates;
    std::unordered_map<std::uint32_t, LodCfgLogState> lodCfgLogStates;
    std::unordered_map<std::uint32_t, LodPickLogState> lodPickLogStates;
    std::unordered_set<std::uint32_t> previousCulledMeshLogSet;
    std::array<std::uint32_t, LodConfig::MaxLevels> previousFrameLodSelection{};
    bool previousFrameLodSelectionInitialized = false;
    std::uint32_t previousFrameNonLodEntities = 0;
    bool previousFrameNonLodInitialized = false;
    MPerfMainState previousMperfMain;
    MPerfOverrideState previousMperfOverride;
    MPerfMeshesState previousMperfMeshes;
    InstSummaryState previousInstSummary;
    InstBufferState previousInstBuffer;
    std::size_t previousMeshSubmitDetailInstances = 0;
    std::size_t previousMeshSubmitDetailDrawCalls = 0;
    bool previousMeshSubmitDetailInitialized = false;
    std::vector<WaterBody> editorWaterBodies = terrainOk ? terrain.GetWaterBodies() : std::vector<WaterBody>{};
    bool editorWaterBodiesDirty = false;
#if defined(IXTREEME_WITH_EDITOR)
    std::uint32_t nextEditorLightId = 1;
#endif
    std::uint32_t nextEditorWaterBodyId = 1;
    for (const WaterBody& body : editorWaterBodies)
        nextEditorWaterBodyId = std::max(nextEditorWaterBodyId, body.id + 1u);
    std::uint32_t nextEditorMeshEntityId = 1;
    SelectedEditorObject selectedEditorObject;
    std::unique_ptr<ecs_world_t, void(*)(ecs_world_t*)> editorHierarchyWorld(ecs_init(), [](ecs_world_t* world) {
        if (world)
            ecs_fini(world);
    });
    ecs_entity_t editorSceneRootEntity = ecs_new(editorHierarchyWorld.get());
    ecs_set_name(editorHierarchyWorld.get(), editorSceneRootEntity, "Untitled");
    ecs_entity_t editorNoteComponentEntity = ecs_new(editorHierarchyWorld.get());
    ecs_set_name(editorHierarchyWorld.get(), editorNoteComponentEntity, "EditorNoteComponent");
    std::unordered_map<std::uint64_t, ecs_entity_t> editorHierarchyEntities;
    auto resetEditorHierarchyEntities = [&]() {
        for (const auto& [_, entity] : editorHierarchyEntities)
            ecs_delete(editorHierarchyWorld.get(), entity);
        editorHierarchyEntities.clear();
        selectedEditorObject.flecsEntity = 0;
    };
    auto sceneEntityNameExists = [&](const std::string& name) {
        auto matches = [&](const auto& entity) {
            return entity.name == name;
        };
        return std::any_of(editorWaterBodies.begin(), editorWaterBodies.end(), matches) ||
            std::any_of(editorPointLights.begin(), editorPointLights.end(), matches) ||
            std::any_of(editorSpotLights.begin(), editorSpotLights.end(), matches) ||
            std::any_of(editorMeshEntities.begin(), editorMeshEntities.end(), matches);
    };
    auto makeUniqueSceneEntityName = [&](const std::string& base) {
        if (!sceneEntityNameExists(base))
            return base;
        for (std::uint32_t suffix = 2; suffix < 10000; ++suffix)
        {
            const std::string candidate = base + " " + std::to_string(suffix);
            if (!sceneEntityNameExists(candidate))
                return candidate;
        }
        return base + " " + std::to_string(nextEditorLightId + nextEditorWaterBodyId + nextEditorMeshEntityId);
    };
    EditorGizmoMode editorGizmoMode = EditorGizmoMode::Translate;
    bool editorGizmoSnapEnabled = false;
    float editorGizmoSnapValue = 1.0f;
    bool editorObjectDragActive = false;
    int editorObjectDragLastX = 0;
    int editorObjectDragLastY = 0;
#if defined(IXTREEME_WITH_EDITOR)
    auto rebuildMeshEntityLookup = [&]() {
        editorMeshEntityLookup.clear();
        for (std::size_t i = 0; i < editorMeshEntities.size(); ++i)
            editorMeshEntityLookup[editorMeshEntities[i].id] = i;
    };
    auto findMeshEntityById = [&](std::uint32_t id) -> MeshSceneEntity* {
        auto lookupIt = editorMeshEntityLookup.find(id);
        if (lookupIt == editorMeshEntityLookup.end() || lookupIt->second >= editorMeshEntities.size())
            return nullptr;
        MeshSceneEntity& mesh = editorMeshEntities[lookupIt->second];
        return mesh.id == id ? &mesh : nullptr;
    };
    auto syncStaticMeshSpatialEntity = [&](const MeshSceneEntity& mesh) {
        const std::string runtimePath = resolveMeshRuntimePath(mesh);
        StaticMeshRenderer* renderer = getStaticMeshRenderer(runtimePath);
        if (!renderer || !renderer->IsLoaded())
        {
            if (staticMeshSpatialIndexed.erase(mesh.id) > 0)
                staticMeshSpatialIndex.Remove(mesh.id);
            return false;
        }

        const SpatialIndex::Aabb bounds = StaticMeshWorldAabb(mesh, *renderer);
        if (staticMeshSpatialIndexed.find(mesh.id) == staticMeshSpatialIndexed.end())
        {
            staticMeshSpatialIndex.Insert(mesh.id, bounds);
            staticMeshSpatialIndexed.insert(mesh.id);
        }
        else
        {
            staticMeshSpatialIndex.Update(mesh.id, bounds);
        }
        return true;
    };
    auto removeStaticMeshSpatialEntity = [&](std::uint32_t id) {
        staticMeshSelectedLods.erase(id);
        staticMeshLodDispositionStates.erase(id);
        if (staticMeshSpatialIndexed.erase(id) > 0)
            staticMeshSpatialIndex.Remove(id);
    };
    auto logStaticMeshSpatialBuild = [&]() {
        const SpatialIndex::Aabb& b = staticMeshSpatialIndex.WorldBounds();
        Tracenf("[SPATIAL] built nodes=%u maxDepth=%u objects=%u worldBounds=(%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f)",
            staticMeshSpatialIndex.NodeCount(),
            staticMeshSpatialIndex.MaxDepth(),
            staticMeshSpatialIndex.ObjectCount(),
            b.min.x, b.min.y, b.min.z,
            b.max.x, b.max.y, b.max.z);
    };
    auto rebuildStaticMeshSpatialIndex = [&]() {
        staticMeshSpatialIndex.Clear();
        staticMeshSpatialIndexed.clear();
        rebuildMeshEntityLookup();
        for (const MeshSceneEntity& mesh : editorMeshEntities)
            syncStaticMeshSpatialEntity(mesh);
        logStaticMeshSpatialBuild();
    };
    auto logStaticMeshSpatialMutations = [&]() {
        const SpatialIndex::MutationStats mutations = staticMeshSpatialIndex.ConsumeMutationStats();
        if (mutations.inserts == 0 && mutations.removes == 0 && mutations.updates == 0)
            return;
        if (QuietLogsForLodDiag() && mutations.inserts == 0 && mutations.removes == 0 && mutations.updates == 1)
            return;
        Tracenf("[SPATIAL] mutate insert=%u remove=%u update=%u (this load/edit)",
            mutations.inserts,
            mutations.removes,
            mutations.updates);
    };
    auto buildSceneSnapshot = [&]() {
        SceneData scene;
        scene.lighting = editorImGui.GetLightingState();
        scene.waterBodies = editorWaterBodies;
        scene.pointLights = editorPointLights;
        scene.spotLights = editorSpotLights;
        scene.meshEntities = editorMeshEntities;
        scene.terrain = terrainOk ? terrain.GetTerrainSceneData() : TerrainSceneData{};
        scene.paletteSlots = terrainOk ? terrain.GetPaletteSlots() : runtimeSession->GetPaletteSlots();
        return scene;
    };
    auto applySceneData = [&](const SceneData& scene) {
        editorWaterBodies = scene.waterBodies;
        editorPointLights = scene.pointLights;
        editorSpotLights = scene.spotLights;
        editorMeshEntities = scene.meshEntities;
        selectedEditorObject = {};
        resetEditorHierarchyEntities();
        nextEditorWaterBodyId = 1;
        for (const WaterBody& body : editorWaterBodies)
            nextEditorWaterBodyId = std::max(nextEditorWaterBodyId, body.id + 1u);
        nextEditorLightId = 1;
        for (const PointLight& light : editorPointLights)
            nextEditorLightId = std::max(nextEditorLightId, light.id + 1u);
        for (const SpotLight& light : editorSpotLights)
            nextEditorLightId = std::max(nextEditorLightId, light.id + 1u);
        nextEditorMeshEntityId = 1;
        for (const MeshSceneEntity& mesh : editorMeshEntities)
            nextEditorMeshEntityId = std::max(nextEditorMeshEntityId, mesh.id + 1u);
        rebuildStaticMeshSpatialIndex();

        editorImGui.SetLightingState(scene.lighting);
        runtimeSession->SetDynamicLightEditorState({});
        runtimeSession->SetWaterBodyEditorState({});
        if (terrainOk)
        {
            syncTerrainAssetRoots();
            terrain.SetLightingState(scene.lighting);
            if (scene.terrain.exists)
            {
                terrain.CreateFlatTerrain(device, scene.terrain);
                terrain.SetWaterBodies(device, editorWaterBodies);
                editorWaterBodies = terrain.GetWaterBodies();
            }
            else
            {
                terrain.ClearTerrain(device);
                editorWaterBodies.clear();
                Tracen("[SCENE] no terrain in scene");
            }
            if (terrain.ApplyPaletteSlots(device, scene.paletteSlots))
                editorImGui.SetPaletteSlots(terrain.GetPaletteSlots());
        }
        editorWaterBodiesDirty = false;
        Tracenf("[SCENE] Applied editor scene state: terrain=%s water=%zu point=%zu spot=%zu mesh=%zu",
            (terrainOk && terrain.HasTerrain()) ? "yes" : "no",
            editorWaterBodies.size(),
            editorPointLights.size(),
            editorSpotLights.size(),
            editorMeshEntities.size());
    };
    auto buildHierarchyEntities = [&]() {
        std::vector<HierarchySceneEntity> entities;
        std::vector<std::uint64_t> liveKeys;
        selectedEditorObject.flecsEntity = 0;

        const SceneData& scene = SceneManager::Instance().GetCurrentScene();
        const std::filesystem::path scenePath(SceneManager::Instance().GetCurrentScenePath());
        std::string sceneName = scenePath.stem().empty() ? scene.name : scenePath.stem().string();
        if (sceneName.empty())
            sceneName = "Untitled";
        ecs_set_name(editorHierarchyWorld.get(), editorSceneRootEntity, sceneName.c_str());
        auto syncEditorComponentTags = [&](ecs_entity_t entity, const std::vector<EditorAttachedComponent>& components) {
            const bool hasNote = std::any_of(components.begin(), components.end(),
                [](const EditorAttachedComponent& component) { return component.type == "editor.note"; });
            if (hasNote)
                ecs_add_id(editorHierarchyWorld.get(), entity, editorNoteComponentEntity);
            else
                ecs_remove_id(editorHierarchyWorld.get(), entity, editorNoteComponentEntity);
        };

        auto ensureEntity = [&](HierarchyEntityType type,
                                std::uint32_t objectId,
                                const std::string& displayName,
                                bool editorHidden) {
            const std::uint64_t key = HierarchyObjectKey(type, objectId);
            liveKeys.push_back(key);
            ecs_entity_t entity = 0;
            auto it = editorHierarchyEntities.find(key);
            if (it == editorHierarchyEntities.end())
            {
                entity = ecs_new(editorHierarchyWorld.get());
                ecs_add_pair(editorHierarchyWorld.get(), entity, EcsChildOf, editorSceneRootEntity);
                editorHierarchyEntities[key] = entity;
                Tracenf("[HIERARCHY] Created flecs scene entity: flecs=%llu object=%u type=%d",
                    static_cast<unsigned long long>(entity),
                    objectId,
                    static_cast<int>(type));
            }
            else
            {
                entity = it->second;
            }

            ecs_set_name(editorHierarchyWorld.get(), entity, displayName.c_str());
            ecs_add_pair(editorHierarchyWorld.get(), entity, EcsChildOf, editorSceneRootEntity);

            const bool selected =
                selectedEditorObject.type == ToSelectedObjectType(type) &&
                selectedEditorObject.id == objectId;
            if (selected)
                selectedEditorObject.flecsEntity = static_cast<std::uint64_t>(entity);

            entities.push_back(HierarchySceneEntity{
                static_cast<std::uint64_t>(entity),
                static_cast<std::uint64_t>(editorSceneRootEntity),
                type,
                objectId,
                displayName,
                editorHidden,
                selected});
        };

        for (const WaterBody& body : editorWaterBodies)
            ensureEntity(HierarchyEntityType::WaterBody, body.id, EditorDisplayName(body), body.editorHidden);
        if (terrainOk && terrain.HasTerrain())
        {
            const TerrainSceneData terrainData = terrain.GetTerrainSceneData();
            ensureEntity(HierarchyEntityType::Terrain, 1u, EditorDisplayName(terrainData), terrainData.editorHidden);
        }
        for (const PointLight& light : editorPointLights)
            ensureEntity(HierarchyEntityType::PointLight, light.id, EditorDisplayName(light), light.editorHidden);
        for (const SpotLight& light : editorSpotLights)
            ensureEntity(HierarchyEntityType::SpotLight, light.id, EditorDisplayName(light), light.editorHidden);
        for (const MeshSceneEntity& mesh : editorMeshEntities)
            ensureEntity(HierarchyEntityType::MeshEntity, mesh.id, EditorDisplayName(mesh), mesh.editorHidden);

        for (auto it = editorHierarchyEntities.begin(); it != editorHierarchyEntities.end();)
        {
            if (std::find(liveKeys.begin(), liveKeys.end(), it->first) == liveKeys.end())
            {
                ecs_delete(editorHierarchyWorld.get(), it->second);
                it = editorHierarchyEntities.erase(it);
            }
            else
            {
                ++it;
            }
        }

        for (const MeshSceneEntity& mesh : editorMeshEntities)
        {
            auto meshIt = editorHierarchyEntities.find(HierarchyObjectKey(HierarchyEntityType::MeshEntity, mesh.id));
            if (meshIt != editorHierarchyEntities.end())
                syncEditorComponentTags(meshIt->second, mesh.editorComponents);
        }

        return std::pair<std::string, std::vector<HierarchySceneEntity>>(sceneName, std::move(entities));
    };
    {
        SceneManager& scenes = SceneManager::Instance();
        scenes.CloseScene();
        Tracen("[SCENE] no scene loaded (editor empty state)");
        Tracen("[STARTUP] Editor build: no automatic default.scene load");
    }
#endif
    bool waterSculptStrokeActive = false;
    std::uint32_t waterSculptStrokeBodyId = 0;
    std::uint32_t waterSculptStrokeModifiedCells = 0;
    bool waterSculptMeshRegenPending = false;
#if defined(IXTREEME_WITH_EDITOR)
    bool editorShiftDown = false;
    bool editorLeftMouseHeld = false;
    bool editorRightMouseHeld = false;
    MovementInputState editorFlyMovement;
#endif

    window.SetInputCallback([&runtimeSession,
                             &runtimeUi,
                             &editorImGui,
                             &movement,
                             &cameraController,
                             &terrain,
                             &terrainOk,
                             &lastPickEntities,
                             &lastPickCamera,
                             &hasLastPickCamera,
                             &selectedTargetNetId,
                             &editorPointLights,
                             &editorSpotLights,
                             &editorMeshEntities,
                             &editorWaterBodies,
                             &editorWaterBodiesDirty,
                             &selectedEditorObject,
                             &editorGizmoMode,
                             &editorGizmoSnapEnabled,
                             &editorGizmoSnapValue,
                             &editorObjectDragActive,
                             &editorObjectDragLastX,
                             &editorObjectDragLastY,
                             &waterSculptStrokeActive,
                             &waterSculptStrokeBodyId,
                             &waterSculptStrokeModifiedCells,
                             &waterSculptMeshRegenPending,
#if defined(IXTREEME_WITH_EDITOR)
                             &editorPlay,
                             &editorShiftDown,
                             &editorLeftMouseHeld,
                             &editorRightMouseHeld,
                             &editorFlyMovement,
                             &rebuildMeshEntityLookup,
                             &syncStaticMeshSpatialEntity,
                             &removeStaticMeshSpatialEntity,
#endif
                             &renderSize](const InputEvent& event)
    {
#if defined(IXTREEME_WITH_EDITOR)
        if (event.type == InputEvent::MouseDown && event.button == MouseButton_Left)
            editorLeftMouseHeld = true;
        else if (event.type == InputEvent::MouseUp && event.button == MouseButton_Left)
            editorLeftMouseHeld = false;
        else if (event.type == InputEvent::MouseDown && event.button == MouseButton_Right)
            editorRightMouseHeld = true;
        else if (event.type == InputEvent::MouseUp && event.button == MouseButton_Right)
            editorRightMouseHeld = false;

        if (event.type == InputEvent::KeyDown && event.key == Key_Shift)
            editorShiftDown = true;
        else if (event.type == InputEvent::KeyUp && event.key == Key_Shift)
            editorShiftDown = false;

        if (runtimeSession->IsMapEditorOpen() && event.type == InputEvent::KeyDown && event.key == Key_F5)
        {
            if (editorShiftDown)
            {
                if (editorPlay.state.mode != EditorPlayMode::Edit)
                    editorPlay.state.mode = EditorPlayMode::Edit;
            }
            else if (editorPlay.state.mode == EditorPlayMode::Edit)
            {
                if (SceneManager::Instance().HasOpenScene())
                    editorPlay.state.mode = EditorPlayMode::Play;
                else
                    Tracen("[EDIT-PLAY] Play ignored: no open scene");
            }
            else
            {
                editorPlay.state.mode = EditorPlayMode::Edit;
            }
            movement.Clear();
#if defined(IXTREEME_WITH_EDITOR)
            editorFlyMovement.Clear();
#endif
            return;
        }
        if (runtimeSession->IsMapEditorOpen() && event.type == InputEvent::KeyDown && event.key == Key_F6)
        {
            if (editorPlay.state.mode == EditorPlayMode::Play)
                editorPlay.state.mode = EditorPlayMode::PlayPaused;
            else if (editorPlay.state.mode == EditorPlayMode::PlayPaused)
                editorPlay.state.mode = EditorPlayMode::Play;
            movement.Clear();
#if defined(IXTREEME_WITH_EDITOR)
            editorFlyMovement.Clear();
#endif
            return;
        }
#endif
        bool sceneViewInputTarget = false;
        InputEvent viewportEvent = event;
        const auto isEditorFlyCameraKey = [](const InputEvent& input) {
            if (input.type != InputEvent::KeyDown && input.type != InputEvent::KeyUp)
                return false;
            switch (input.key)
            {
            case Key_W:
            case Key_A:
            case Key_S:
            case Key_D:
            case Key_Space:
            case Key_Control:
            case Key_Shift:
                return true;
            default:
                return false;
            }
        };
#if defined(IXTREEME_WITH_EDITOR)
        sceneViewInputTarget = runtimeSession->IsMapEditorOpen() && editorImGui.IsSceneViewInputTarget(event);
        if (runtimeSession->IsMapEditorOpen() && event.type == InputEvent::MouseDown)
            editorImGui.SetSceneViewKeyboardFocus(sceneViewInputTarget);
        if (sceneViewInputTarget)
            viewportEvent = editorImGui.MapInputToSceneView(event);
#endif
        const bool editorFlyCameraKey =
            runtimeSession->IsMapEditorOpen() &&
            cameraController.IsFreeCameraEnabled() &&
            isEditorFlyCameraKey(event);
        if (editorFlyCameraKey)
        {
            editorFlyMovement.Apply(event);
            movement.Apply(event);
            if (!QuietLogsForLodDiag())
            {
                Tracenf("[EDITOR-CAMERA-INPUT] key=%d type=%s state w=%d a=%d s=%d d=%d space=%d ctrl=%d shift=%d",
                    static_cast<int>(event.key),
                    InputEventTypeName(event.type),
                    editorFlyMovement.w ? 1 : 0,
                    editorFlyMovement.a ? 1 : 0,
                    editorFlyMovement.s ? 1 : 0,
                    editorFlyMovement.d ? 1 : 0,
                    editorFlyMovement.space ? 1 : 0,
                    editorFlyMovement.control ? 1 : 0,
                    editorFlyMovement.shift ? 1 : 0);
            }
            return;
        }
        if (editorImGui.WantsInputCapture(event) && !sceneViewInputTarget && !editorFlyCameraKey)
        {
            movement.Clear();
            return;
        }

        if (runtimeSession->IsInWorld() && event.type == InputEvent::KeyDown && event.key == Key_Escape)
        {
            if (runtimeUi->IsSettingsVisible())
                runtimeUi->HideSettings();
            else
                runtimeUi->ToggleInGameMenu();
            movement.Clear();
            return;
        }
        if (runtimeSession->IsInWorld() && event.type == InputEvent::KeyDown && event.key == Key_I)
        {
            runtimeUi->ToggleInventory();
            movement.Clear();
            return;
        }

        if (runtimeUi->OnInput(event))
        {
            movement.Clear();
            return;
        }

        const bool editorTextInputFocused = runtimeSession->IsMapEditorOpen() && runtimeSession->IsTextInputFocused();
        if (editorTextInputFocused)
            movement.Clear();
        else
            movement.Apply(event);

        if (runtimeSession->IsInWorld() && event.type == InputEvent::MouseDown && event.button == MouseButton_Left &&
            hasLastPickCamera && !runtimeSession->IsMapEditorOpen())
        {
            selectedTargetNetId = PickRenderEntityTarget(lastPickEntities,
                                               lastPickCamera,
                                               renderSize.width,
                                               renderSize.height,
                                               event.x,
                                               event.y);
            Tracenf("[PICK] selected render entity net_id=%u", selectedTargetNetId);
            return;
        }
        if (runtimeSession->IsInWorld() && !runtimeSession->IsMapEditorOpen() &&
            event.type == InputEvent::KeyDown && event.key == Key_F)
        {
            if (selectedTargetNetId != 0)
                runtimeSession->SendAttackTarget(selectedTargetNetId);
            return;
        }
#if defined(IXTREEME_WITH_EDITOR)
        if (event.type == InputEvent::KeyDown && event.key == Key_F2 && runtimeSession->IsMapEditorOpen())
        {
            if (runtimeSession->OnInput(event))
                return;
        }
        if (event.type == InputEvent::KeyDown && event.key == Key_F2 && terrainOk)
        {
            terrain.ToggleWalkabilityDebug();
            if (!terrain.IsWalkabilityDebugEnabled() && runtimeSession->IsMapEditorOpen())
            {
                runtimeSession->ToggleMapEditor();
                terrain.SetMapEditorOpen(false);
                cameraController.SetFreeCameraEnabled(false);
            }
            return;
        }
        if (event.type == InputEvent::KeyDown && event.key == Key_F4 && terrainOk &&
            runtimeSession->IsInWorld())
        {
            runtimeSession->ToggleMapEditor();
            runtimeSession->ClearKeyboardFocus();
            terrain.SetMapEditorOpen(runtimeSession->IsMapEditorOpen());
            cameraController.SetFreeCameraEnabled(runtimeSession->IsMapEditorOpen());
            if (!runtimeSession->IsMapEditorOpen())
            {
                selectedEditorObject = {};
                editorObjectDragActive = false;
                runtimeSession->SetEditorStatus("Editor closed");
            }
            else
            {
                runtimeSession->SetEditorStatus("Editor fly camera active: RMB look, WASD move, Space/Ctrl up/down");
            }
            return;
        }

        if (terrainOk && runtimeSession->IsMapEditorOpen()
#if defined(IXTREEME_WITH_EDITOR)
            && editorPlay.state.mode == EditorPlayMode::Edit
#endif
            )
        {
            if (editorTextInputFocused &&
                (event.type == InputEvent::KeyDown ||
                 event.type == InputEvent::KeyUp ||
                 event.type == InputEvent::Char))
            {
                runtimeSession->OnInput(event);
                if (event.type == InputEvent::KeyDown && event.key == Key_Enter)
                    runtimeSession->ClearKeyboardFocus();
                return;
            }

            if (event.type == InputEvent::KeyDown || event.type == InputEvent::KeyUp)
            {
                if (event.type == InputEvent::KeyDown)
                {
                    if (event.key == Key_W)
                    {
                        editorGizmoMode = EditorGizmoMode::Translate;
                        runtimeSession->SetEditorStatus("Gizmo: translate");
                    }
                    else if (event.key == Key_E)
                    {
                        editorGizmoMode = EditorGizmoMode::Rotate;
                        runtimeSession->SetEditorStatus("Gizmo: rotate");
                    }
                    else if (event.key == Key_R)
                    {
                        editorGizmoMode = EditorGizmoMode::Scale;
                        runtimeSession->SetEditorStatus("Gizmo: scale");
                    }
                    else if (event.key == Key_Delete && selectedEditorObject.type == SelectedEditorObjectType::WaterBody)
                    {
                        editorWaterBodies.erase(std::remove_if(editorWaterBodies.begin(), editorWaterBodies.end(),
                            [&](const WaterBody& body) { return body.id == selectedEditorObject.id; }), editorWaterBodies.end());
                        runtimeSession->SetEditorStatus("Deleted water body #" + std::to_string(selectedEditorObject.id));
                        selectedEditorObject = {};
                        editorWaterBodiesDirty = true;
                        return;
                    }
                    else if (event.key == Key_Delete && selectedEditorObject.type == SelectedEditorObjectType::MeshEntity)
                    {
                        removeStaticMeshSpatialEntity(selectedEditorObject.id);
                        editorMeshEntities.erase(std::remove_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                            [&](const MeshSceneEntity& mesh) { return mesh.id == selectedEditorObject.id; }), editorMeshEntities.end());
                        rebuildMeshEntityLookup();
                        runtimeSession->SetEditorStatus("Deleted mesh entity #" + std::to_string(selectedEditorObject.id));
                        selectedEditorObject = {};
                        SceneManager::Instance().MarkDirty();
                        return;
                    }
                }
                if (terrain.HandleEditorInput(viewportEvent))
                    return;
            }

            bool consumedByEditorUi = false;
            if (event.type == InputEvent::MouseMove ||
                event.type == InputEvent::MouseDown ||
                event.type == InputEvent::MouseUp ||
                event.type == InputEvent::MouseWheel)
            {
                const MapEditorSettings editorSettings = editorImGui.GetMapEditorSettings();
                const bool terrainToolActiveForDiag =
                    editorSettings.toolMode == MapEditorToolMode::Heightmap ||
                    editorSettings.toolMode == MapEditorToolMode::SplatPaint;
                const bool shouldLogSculptDiag =
                    QuietLogsForLodDiag()
                        ? terrainToolActiveForDiag
                        : (terrainToolActiveForDiag ||
                            event.type == InputEvent::MouseDown ||
                            event.type == InputEvent::MouseUp);
                const auto viewportDiag = editorImGui.GetViewportInputDiagnostics();
                auto logSculptGateState = [&](const char* stage, bool brushReached, const char* reason) {
                    if (!shouldLogSculptDiag)
                        return;
                    const bool wantCaptureMouse = editorImGui.WantsInputCapture(event);
                    const bool isLeftMouseDown = event.type == InputEvent::MouseDown && event.button == MouseButton_Left;
                    const bool dropTargetCapturing =
                        viewportDiag.dropTargetActive ||
                        viewportDiag.overlayDropTargetActive;
                    Tracenf("[SCULPT-DIAG] viewport mouseDown=%s drag=%s pos=(%d,%d) sculptModeActive=%s activeTool=%s event=%s stage=%s",
                        isLeftMouseDown ? "yes" : "no",
                        (editorLeftMouseHeld || editorObjectDragActive || viewportDiag.assetDragActive) ? "yes" : "no",
                        event.x,
                        event.y,
                        terrainToolActiveForDiag ? "yes" : "no",
                        SculptDiagToolModeName(editorSettings.toolMode),
                        InputEventTypeName(event.type),
                        stage ? stage : "unknown");
                    Tracenf("[SCULPT-DIAG] gate imgui WantCaptureMouse=%s",
                        wantCaptureMouse ? "yes" : "no");
                    if (viewportDiag.hoveredItemId != 0)
                    {
                        Tracenf("[SCULPT-DIAG] gate hoveredItem=0x%08x dropTargetCapturing=%s dropVisible=%s assetDrag=%s dropHovered=%s overlayHovered=%s activeItem=0x%08x",
                            viewportDiag.hoveredItemId,
                            dropTargetCapturing ? "yes" : "no",
                            viewportDiag.dropTargetVisible ? "yes" : "no",
                            viewportDiag.assetDragActive ? "yes" : "no",
                            viewportDiag.dropTargetHovered ? "yes" : "no",
                            viewportDiag.overlayDropTargetHovered ? "yes" : "no",
                            viewportDiag.activeItemId);
                    }
                    else
                    {
                        Tracenf("[SCULPT-DIAG] gate hoveredItem=none dropTargetCapturing=%s dropVisible=%s assetDrag=%s dropHovered=%s overlayHovered=%s activeItem=0x%08x",
                            dropTargetCapturing ? "yes" : "no",
                            viewportDiag.dropTargetVisible ? "yes" : "no",
                            viewportDiag.assetDragActive ? "yes" : "no",
                            viewportDiag.dropTargetHovered ? "yes" : "no",
                            viewportDiag.overlayDropTargetHovered ? "yes" : "no",
                            viewportDiag.activeItemId);
                    }
                    Tracenf("[SCULPT-DIAG] gate sculptToolSelected=%s",
                        terrainToolActiveForDiag ? "yes" : "no");
                    Tracenf("[SCULPT-DIAG] gate mapLoadedFlag=%s",
                        terrain.IsMapLoadedForDiagnostics() ? "yes" : "no");
                    Tracenf("[SCULPT-DIAG] gate terrainTarget=%p",
                        terrain.HasTerrain() ? static_cast<void*>(&terrain) : nullptr);
                    Tracenf("[SCULPT-DIAG] brush handler reached=%s reason=%s consumedByEditorUi=%s",
                        brushReached ? "yes" : "no",
                        reason ? reason : "n/a",
                        consumedByEditorUi ? "yes" : "no");
                };

                logSculptGateState("before-runtime-ui", false, "pre-runtime-ui");
                consumedByEditorUi = runtimeSession->OnInput(event);
                logSculptGateState("after-runtime-ui", false, consumedByEditorUi ? "runtime-ui-consumed" : "runtime-ui-pass");
                auto selectedWaterBodyIt = [&]() {
                    return std::find_if(editorWaterBodies.begin(), editorWaterBodies.end(),
                        [&](const WaterBody& body) {
                            return selectedEditorObject.type == SelectedEditorObjectType::WaterBody &&
                                body.id == selectedEditorObject.id;
                        });
                };
                auto updateWaterSculptCursor = [&]() -> std::optional<WorldVec3> {
                    if (!editorSettings.waterSculptActive || !hasLastPickCamera ||
                        selectedEditorObject.type != SelectedEditorObjectType::WaterBody)
                    {
                        terrain.SetWaterSculptBrush(false, 0.0f, 0.0f, editorSettings.waterSculptRadiusMeters,
                            editorSettings.waterSculptAdd);
                        return std::nullopt;
                    }
                    auto bodyIt = selectedWaterBodyIt();
                    if (bodyIt == editorWaterBodies.end())
                    {
                        terrain.SetWaterSculptBrush(false, 0.0f, 0.0f, editorSettings.waterSculptRadiusMeters,
                            editorSettings.waterSculptAdd);
                        return std::nullopt;
                    }
                    std::optional<WorldVec3> hit = RaycastTerrainPoint(terrain,
                        lastPickCamera,
                        renderSize.width,
                        renderSize.height,
                        viewportEvent.x,
                        viewportEvent.y);
                    if (!hit)
                    {
                        terrain.SetWaterSculptBrush(false, 0.0f, 0.0f, editorSettings.waterSculptRadiusMeters,
                            editorSettings.waterSculptAdd);
                        return std::nullopt;
                    }
                    terrain.SetWaterSculptBrush(true, hit->x, hit->z, editorSettings.waterSculptRadiusMeters,
                        editorSettings.waterSculptAdd);
                    return hit;
                };
                auto applyWaterSculptAtCursor = [&](const WorldVec3& hit) -> std::uint32_t {
                    auto bodyIt = selectedWaterBodyIt();
                    if (bodyIt == editorWaterBodies.end() || bodyIt->id != waterSculptStrokeBodyId)
                        return 0;
                    return ApplyWaterSculptBrush(*bodyIt, hit, editorSettings.waterSculptRadiusMeters,
                        editorSettings.waterSculptAdd);
                };

                if (waterSculptStrokeActive)
                {
                    if (event.type == InputEvent::MouseMove)
                    {
                        if (std::optional<WorldVec3> hit = updateWaterSculptCursor())
                            waterSculptStrokeModifiedCells += applyWaterSculptAtCursor(*hit);
                        return;
                    }
                    if (event.type == InputEvent::MouseUp && event.button == MouseButton_Left)
                    {
                        if (std::optional<WorldVec3> hit = updateWaterSculptCursor())
                            waterSculptStrokeModifiedCells += applyWaterSculptAtCursor(*hit);
                        waterSculptStrokeActive = false;
                        if (waterSculptStrokeModifiedCells > 0)
                        {
                            editorWaterBodiesDirty = true;
                            waterSculptMeshRegenPending = true;
                            runtimeSession->SetEditorStatus("Water sculpt stroke: " +
                                std::to_string(waterSculptStrokeModifiedCells) + " cells modified");
                        }
                        Tracenf("[WATER-OBJ-5] Brush stroke ended: body_id=%u mode=%s cells_modified=%u",
                            waterSculptStrokeBodyId,
                            editorSettings.waterSculptAdd ? "add" : "remove",
                            waterSculptStrokeModifiedCells);
                        waterSculptStrokeBodyId = 0;
                        waterSculptStrokeModifiedCells = 0;
                        return;
                    }
                }

                if (!consumedByEditorUi && editorSettings.waterSculptActive &&
                    selectedEditorObject.type == SelectedEditorObjectType::WaterBody &&
                    (event.type == InputEvent::MouseMove ||
                     (event.type == InputEvent::MouseDown && event.button == MouseButton_Left)))
                {
                    std::optional<WorldVec3> hit = updateWaterSculptCursor();
                    if (event.type == InputEvent::MouseDown && event.button == MouseButton_Left)
                    {
                        waterSculptStrokeActive = true;
                        waterSculptStrokeBodyId = selectedEditorObject.id;
                        waterSculptStrokeModifiedCells = 0;
                        editorObjectDragActive = false;
                        if (hit)
                            waterSculptStrokeModifiedCells += applyWaterSculptAtCursor(*hit);
                    }
                    return;
                }
                if (!editorSettings.waterSculptActive && !waterSculptStrokeActive)
                    terrain.SetWaterSculptBrush(false, 0.0f, 0.0f, editorSettings.waterSculptRadiusMeters,
                        editorSettings.waterSculptAdd);

                const bool terrainToolActive =
                    editorSettings.toolMode == MapEditorToolMode::Heightmap ||
                    editorSettings.toolMode == MapEditorToolMode::SplatPaint;
                const bool rightMouseButtonEvent =
                    (event.type == InputEvent::MouseDown || event.type == InputEvent::MouseUp) &&
                    event.button == MouseButton_Right;
                const bool cameraRmbInput =
                    rightMouseButtonEvent ||
                    (event.type == InputEvent::MouseMove && editorRightMouseHeld);
                const bool terrainBrushInput =
                    event.type == InputEvent::MouseMove ||
                    ((event.type == InputEvent::MouseDown || event.type == InputEvent::MouseUp) &&
                     event.button == MouseButton_Left);
                if (!consumedByEditorUi && terrainToolActive && !cameraRmbInput && terrainBrushInput)
                {
                    editorObjectDragActive = false;
                    logSculptGateState("before-brush-handler", true, "terrain-tool-route");
                    terrain.HandleEditorInput(viewportEvent);
                    return;
                }
                if (shouldLogSculptDiag &&
                    (event.type == InputEvent::MouseMove ||
                     event.type == InputEvent::MouseDown ||
                     event.type == InputEvent::MouseUp))
                {
                    logSculptGateState("terrain-tool-route-skipped", false,
                        consumedByEditorUi ? "consumed-by-editor-ui" :
                        (!terrainToolActive ? "terrain-tool-inactive" : "event-not-routed"));
                }

                if (event.type == InputEvent::MouseUp)
                {
                    if (event.button == MouseButton_Left)
                        editorObjectDragActive = false;
                    terrain.HandleEditorInput(viewportEvent);
                }
                else if (!consumedByEditorUi)
                {
                    if (event.type == InputEvent::MouseDown)
                        runtimeSession->ClearKeyboardFocus();

                    if (event.type == InputEvent::MouseDown && event.button == MouseButton_Left && hasLastPickCamera)
                    {
                        if (auto pointId = PickDynamicLight(editorPointLights,
                                lastPickCamera,
                                renderSize.width,
                                renderSize.height,
                                viewportEvent.x,
                                viewportEvent.y))
                        {
                            selectedEditorObject = {SelectedEditorObjectType::PointLight, *pointId};
                            editorObjectDragActive = true;
                            editorObjectDragLastX = event.x;
                            editorObjectDragLastY = event.y;
                            runtimeSession->SetEditorStatus("Selected point light #" + std::to_string(*pointId));
                            return;
                        }
                        if (auto spotId = PickDynamicLight(editorSpotLights,
                                lastPickCamera,
                                renderSize.width,
                                renderSize.height,
                                viewportEvent.x,
                                viewportEvent.y))
                        {
                            selectedEditorObject = {SelectedEditorObjectType::SpotLight, *spotId};
                            editorObjectDragActive = true;
                            editorObjectDragLastX = event.x;
                            editorObjectDragLastY = event.y;
                            runtimeSession->SetEditorStatus("Selected spot light #" + std::to_string(*spotId));
                            return;
                        }
                        if (auto waterId = PickWaterBody(editorWaterBodies,
                                lastPickCamera,
                                renderSize.width,
                                renderSize.height,
                                viewportEvent.x,
                                viewportEvent.y))
                        {
                            selectedEditorObject = {SelectedEditorObjectType::WaterBody, *waterId};
                            editorObjectDragActive = true;
                            editorObjectDragLastX = event.x;
                            editorObjectDragLastY = event.y;
                            runtimeSession->SetEditorStatus("Selected water body #" + std::to_string(*waterId));
                            return;
                        }
                    }
                    if (event.type == InputEvent::MouseMove && editorObjectDragActive &&
                        selectedEditorObject.type != SelectedEditorObjectType::None &&
                        selectedEditorObject.type != SelectedEditorObjectType::WaterBody)
                    {
                        const int dx = event.x - editorObjectDragLastX;
                        const int dy = event.y - editorObjectDragLastY;
                        editorObjectDragLastX = event.x;
                        editorObjectDragLastY = event.y;
                        auto moveLight = [&](auto& light) {
                            WorldVec3 position{light.position[0], light.position[1], light.position[2]};
                            const float scale = 0.025f * std::max(1.0f, std::sqrt(WorldDot(WorldSub(position, lastPickCamera.eye),
                                WorldSub(position, lastPickCamera.eye))));
                            if (editorGizmoMode == EditorGizmoMode::Translate)
                            {
                                position = WorldAdd(position,
                                    WorldAdd(WorldScale(CameraRight(lastPickCamera), static_cast<float>(dx) * scale),
                                             WorldScale(CameraUp(lastPickCamera), static_cast<float>(-dy) * scale)));
                                if (editorGizmoSnapEnabled)
                                    position = SnapPoint(position, editorGizmoSnapValue);
                                light.position[0] = position.x;
                                light.position[1] = position.y;
                                light.position[2] = position.z;
                            }
                            else if (editorGizmoMode == EditorGizmoMode::Scale)
                            {
                                const float delta = static_cast<float>(dx - dy) * 0.05f * std::max(1.0f, scale);
                                light.radius = std::clamp(light.radius + delta, 0.5f, 100.0f);
                                if (editorGizmoSnapEnabled)
                                    light.radius = std::clamp(SnapValue(light.radius, editorGizmoSnapValue), 0.5f, 100.0f);
                            }
                            else if constexpr (std::is_same_v<std::decay_t<decltype(light)>, SpotLight>)
                            {
                                if (editorGizmoMode == EditorGizmoMode::Rotate)
                                {
                                    light.rotation[1] += static_cast<float>(dx) * 0.01f;
                                    light.rotation[0] = std::clamp(light.rotation[0] + static_cast<float>(-dy) * 0.01f,
                                        -1.5708f,
                                        1.5708f);
                                }
                            }
                        };
                        if (selectedEditorObject.type == SelectedEditorObjectType::PointLight)
                        {
                            auto it = std::find_if(editorPointLights.begin(), editorPointLights.end(),
                                [&](const PointLight& light) { return light.id == selectedEditorObject.id; });
                            if (it != editorPointLights.end())
                            {
                                moveLight(*it);
                                return;
                            }
                        }
                        if (selectedEditorObject.type == SelectedEditorObjectType::SpotLight)
                        {
                            auto it = std::find_if(editorSpotLights.begin(), editorSpotLights.end(),
                                [&](const SpotLight& light) { return light.id == selectedEditorObject.id; });
                            if (it != editorSpotLights.end())
                            {
                                moveLight(*it);
                                return;
                            }
                        }
                        if (selectedEditorObject.type == SelectedEditorObjectType::MeshEntity)
                        {
                            auto it = std::find_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                                [&](const MeshSceneEntity& mesh) { return mesh.id == selectedEditorObject.id; });
                            if (it != editorMeshEntities.end())
                            {
                                WorldVec3 position{it->position[0], it->position[1], it->position[2]};
                                const float scale = 0.025f * std::max(1.0f, std::sqrt(WorldDot(WorldSub(position, lastPickCamera.eye),
                                    WorldSub(position, lastPickCamera.eye))));
                                if (editorGizmoMode == EditorGizmoMode::Translate)
                                {
                                    position = WorldAdd(position,
                                        WorldAdd(WorldScale(CameraRight(lastPickCamera), static_cast<float>(dx) * scale),
                                                 WorldScale(CameraUp(lastPickCamera), static_cast<float>(-dy) * scale)));
                                    if (editorGizmoSnapEnabled)
                                        position = SnapPoint(position, editorGizmoSnapValue);
                                    it->position[0] = position.x;
                                    it->position[1] = position.y;
                                    it->position[2] = position.z;
                                }
                                else if (editorGizmoMode == EditorGizmoMode::Scale)
                                {
                                    const float delta = static_cast<float>(dx - dy) * 0.01f * std::max(1.0f, scale);
                                    it->scale[0] = std::max(0.001f, it->scale[0] + delta);
                                    it->scale[1] = std::max(0.001f, it->scale[1] + delta);
                                    it->scale[2] = std::max(0.001f, it->scale[2] + delta);
                                    if (editorGizmoSnapEnabled)
                                    {
                                        it->scale[0] = std::max(0.001f, SnapValue(it->scale[0], editorGizmoSnapValue));
                                        it->scale[1] = std::max(0.001f, SnapValue(it->scale[1], editorGizmoSnapValue));
                                        it->scale[2] = std::max(0.001f, SnapValue(it->scale[2], editorGizmoSnapValue));
                                    }
                                }
                                else if (editorGizmoMode == EditorGizmoMode::Rotate)
                                {
                                    it->rotation[1] += static_cast<float>(dx) * 0.01f;
                                    it->rotation[0] += static_cast<float>(-dy) * 0.01f;
                                }
                                syncStaticMeshSpatialEntity(*it);
                                SceneManager::Instance().MarkDirty();
                                return;
                            }
                        }
                    }
                    if (event.type == InputEvent::MouseMove && editorObjectDragActive &&
                        selectedEditorObject.type == SelectedEditorObjectType::WaterBody)
                    {
                        const int dx = event.x - editorObjectDragLastX;
                        const int dy = event.y - editorObjectDragLastY;
                        editorObjectDragLastX = event.x;
                        editorObjectDragLastY = event.y;
                        auto it = std::find_if(editorWaterBodies.begin(), editorWaterBodies.end(),
                            [&](const WaterBody& body) { return body.id == selectedEditorObject.id; });
                        if (it != editorWaterBodies.end())
                        {
                            WaterBodyEditorState state = BuildWaterBodyEditorState(editorWaterBodies, it->id);
                            const WorldVec3 center{state.center[0], state.center[1], state.center[2]};
                            const float scale = 0.025f * std::max(1.0f, std::sqrt(WorldDot(WorldSub(center, lastPickCamera.eye),
                                WorldSub(center, lastPickCamera.eye))));
                            if (editorGizmoMode == EditorGizmoMode::Translate)
                            {
                                const WorldVec3 moved = WorldAdd(center,
                                    WorldAdd(WorldScale(CameraRight(lastPickCamera), static_cast<float>(dx) * scale),
                                             WorldScale(CameraUp(lastPickCamera), static_cast<float>(-dy) * scale)));
                                const WorldVec3 snapped = editorGizmoSnapEnabled ? SnapPoint(moved, editorGizmoSnapValue) : moved;
                                state.center[0] = snapped.x;
                                state.center[1] = snapped.y;
                                state.center[2] = snapped.z;
                            }
                            else if (editorGizmoMode == EditorGizmoMode::Scale)
                            {
                                const float delta = static_cast<float>(dx - dy) * 0.05f * std::max(1.0f, scale);
                                state.width = std::clamp(state.width + delta, 1.0f, 200.0f);
                                state.depth = std::clamp(state.depth + delta, 1.0f, 200.0f);
                                if (editorGizmoSnapEnabled)
                                {
                                    state.width = std::clamp(SnapValue(state.width, editorGizmoSnapValue), 1.0f, 200.0f);
                                    state.depth = std::clamp(SnapValue(state.depth, editorGizmoSnapValue), 1.0f, 200.0f);
                                }
                            }
                            ApplyWaterBodyEditorStateToBody(*it, state);
                            editorWaterBodiesDirty = true;
                            return;
                        }
                    }
                    terrain.HandleEditorInput(viewportEvent);
                }

                const bool cameraMouse =
                    event.type == InputEvent::MouseMove ||
                    ((event.type == InputEvent::MouseDown || event.type == InputEvent::MouseUp) &&
                     event.button == MouseButton_Right);
                if (!consumedByEditorUi && runtimeSession->IsInWorld() && cameraMouse)
                    cameraController.HandleInput(event);
                return;
            }
        }

        if (terrainOk
#if defined(IXTREEME_WITH_EDITOR)
            && editorPlay.state.mode == EditorPlayMode::Edit
#endif
            && terrain.HandleEditorInput(viewportEvent))
            return;
#endif
        if (runtimeSession->IsInWorld() && cameraController.HandleInput(event))
            return;

        if (!runtimeSession->OnInput(event))
        {
            // TODO: forward unconsumed events to the game/3D scene input path.
            //LogUnhandledInput(event);
        }
    });

#if defined(IXTREEME_WITH_EDITOR)
    std::vector<std::string> pendingDroppedFiles;
    window.SetFileDropCallback([&pendingDroppedFiles](const std::vector<std::string>& paths)
    {
        pendingDroppedFiles.insert(pendingDroppedFiles.end(), paths.begin(), paths.end());
    });
#endif

    const auto startTime = std::chrono::steady_clock::now();
    double previousSeconds = 0.0;
#if defined(IXTREEME_WITH_EDITOR)
    EngineStats engineStats{};
    double statsAccumSeconds = 0.0;
    double statsFrameMsAccum = 0.0;
    double statsMinFrameMs = std::numeric_limits<double>::max();
    double statsMaxFrameMs = 0.0;
    std::uint32_t statsFrameCount = 0;
    std::clock_t statsPreviousCpuClock = std::clock();
    double statsPreviousCpuSampleSeconds = 0.0;
    const unsigned int statsHardwareThreads = std::max(1u, std::thread::hardware_concurrency());
    double lastPerfLogSeconds = -1000.0;
#endif
    bool running = true;
    while (running)
    {
        running = window.PumpMessages();

        uint32_t width = 0;
        uint32_t height = 0;
        if (window.ConsumeResize(width, height))
        {
            Tracenf("[MAIN] Resize event consumed: %ux%u, calling device.Resize()", width, height);
            if (device.Resize(width, height))
            {
                Tracen("[MAIN] device.Resize() returned true, recreating pipelines");
                if (offscreenSceneOk)
                {
                    offscreenSceneOk = offscreenScene.Recreate(device);
                    if (offscreenSceneOk)
                    {
                        if (skinnedMeshOk)
                            skinnedMesh.SetMainRenderPass(offscreenScene.GetRenderPass());
                        for (auto& [path, entry] : staticMeshCache)
                        {
                            (void)path;
                            if (entry.renderer)
                                entry.renderer->SetMainRenderPass(offscreenScene.GetRenderPass());
                        }
                        if (terrainOk)
                        {
                            terrain.SetMainRenderPass(offscreenScene.GetRenderPass());
                            terrain.SetWaterRefractionInputs(offscreenScene.GetSceneColorSnapshotView(),
                                offscreenScene.GetSceneDepthSnapshotView(),
                                offscreenScene.GetLinearSampler(),
                                offscreenScene.GetExtent());
                        }
#if defined(IXTREEME_WITH_EDITOR)
                        editorImGui.SetSceneViewTexture(offscreenScene.GetLinearSampler(),
                            offscreenScene.GetSceneColorView(),
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            offscreenScene.GetExtent());
#endif
                    }
#if defined(IXTREEME_WITH_EDITOR)
                    else
                    {
                        editorImGui.SetSceneViewTexture(VK_NULL_HANDLE,
                            VK_NULL_HANDLE,
                            VK_IMAGE_LAYOUT_UNDEFINED,
                            {});
                    }
#endif
                }
                if (skinnedMeshOk)
                    skinnedMesh.RecreatePipeline(device);
                for (auto& [path, entry] : staticMeshCache)
                {
                    (void)path;
                    if (entry.renderer)
                        entry.renderer->RecreatePipeline(device);
                }
                if (terrainOk)
                    terrain.RecreatePipeline(device);
                if (worldLabelsOk)
                    worldLabels.RecreatePipeline(device);
                runtimeSession->OnRenderPassChanged(device);
                rmlUi.OnRenderPassChanged(device);
#if defined(IXTREEME_WITH_EDITOR)
                editorImGui.OnRenderPassChanged(device);
#endif
                renderSize = device.GetSwapchainExtent();
                runtimeSession->Resize(renderSize.width, renderSize.height);
                rmlUi.Resize(renderSize.width, renderSize.height);
            }
            else
            {
                Tracen("[MAIN] device.Resize() returned false (unchanged), skipping pipeline recreate");
            }
        }

#if defined(IXTREEME_WITH_EDITOR)
        if (!pendingDroppedFiles.empty())
        {
            std::vector<std::string> dropped;
            dropped.swap(pendingDroppedFiles);
            Tracenf("[ASSET-DROP] queued file drop import: %zu path(s)", dropped.size());
            runtimeSession->ImportDroppedFiles(dropped);
            editorImGui.RefreshAssetLibrary();
        }
#endif

        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(now - startTime).count();
        const double deltaSeconds = seconds - previousSeconds;
        previousSeconds = seconds;
#if defined(IXTREEME_WITH_EDITOR)
        const double frameMs = std::max(0.0, deltaSeconds * 1000.0);
        statsAccumSeconds += std::max(0.0, deltaSeconds);
        statsFrameMsAccum += frameMs;
        statsMinFrameMs = std::min(statsMinFrameMs, frameMs);
        statsMaxFrameMs = std::max(statsMaxFrameMs, frameMs);
        ++statsFrameCount;
        engineStats.frameMs = frameMs;
        if (statsAccumSeconds >= 0.25 && statsFrameCount > 0)
        {
            engineStats.fps = static_cast<double>(statsFrameCount) / statsAccumSeconds;
            engineStats.averageFrameMs = statsFrameMsAccum / static_cast<double>(statsFrameCount);
            engineStats.minFrameMs = statsMinFrameMs == std::numeric_limits<double>::max() ? 0.0 : statsMinFrameMs;
            engineStats.maxFrameMs = statsMaxFrameMs;
            engineStats.frameBudgetPercent = (engineStats.averageFrameMs / (1000.0 / 60.0)) * 100.0;

            const std::clock_t cpuClock = std::clock();
            const double cpuSeconds = static_cast<double>(cpuClock - statsPreviousCpuClock) / CLOCKS_PER_SEC;
            const double sampleSeconds = std::max(0.0001, seconds - statsPreviousCpuSampleSeconds);
            engineStats.processCpuPercent =
                std::clamp((cpuSeconds / sampleSeconds) * 100.0 / static_cast<double>(statsHardwareThreads), 0.0, 100.0);
            statsPreviousCpuClock = cpuClock;
            statsPreviousCpuSampleSeconds = seconds;

            if (!QuietLogsForLodDiag() || seconds - lastPerfLogSeconds >= 5.0)
            {
                Tracenf("[PERF] fps=%.1f frame_ms=%.2f budget60=%.0f%% cpu=%.1f%% swapchain=%ux%u",
                    engineStats.fps,
                    engineStats.averageFrameMs,
                    engineStats.frameBudgetPercent,
                    engineStats.processCpuPercent,
                    renderSize.width,
                    renderSize.height);
                lastPerfLogSeconds = seconds;
            }

            statsAccumSeconds = 0.0;
            statsFrameMsAccum = 0.0;
            statsMinFrameMs = std::numeric_limits<double>::max();
            statsMaxFrameMs = 0.0;
            statsFrameCount = 0;
        }
#endif
        terrain.SetPerformanceFps(engineStats.fps);
#if defined(IXTREEME_WITH_EDITOR)
        if (runtimeSession->IsMapEditorOpen() && cameraController.IsFreeCameraEnabled())
            cameraController.Update(deltaSeconds, editorFlyMovement);
        else
            cameraController.Update(deltaSeconds, movement);
#else
        cameraController.Update(deltaSeconds, movement);
#endif
#if defined(IXTREEME_WITH_EDITOR)
        if (editorPlay.state.mode == EditorPlayMode::Play)
        {
            editorPlay.state.elapsedSeconds += deltaSeconds;
            ++editorPlay.state.frameCount;
        }
#endif
        {
            runtimeSession->UpdateNetwork();
            runtimeSession->SendMoveInput(movement.DirectionAngle(cameraController.MovementYaw()), RuntimeMoveState::Idle);
        }
        runtimeSession->Update(seconds);
        rmlUi.Update();

        std::vector<WorldRenderEntity> frameEntities;
        WorldCamera frameCamera{};
        bool hasFrameCamera = false;
        if (runtimeSession->IsInWorld())
        {
            frameEntities = runtimeSession->GetWorldEntities();
            frameCamera = cameraController.BuildCamera(renderSize.width, renderSize.height);
            hasFrameCamera = true;
            lastPickEntities = frameEntities;
            lastPickCamera = frameCamera;
            hasLastPickCamera = true;
            const bool selectedStillVisible = std::any_of(lastPickEntities.begin(),
                lastPickEntities.end(),
                [selectedTargetNetId](const WorldRenderEntity& entity) {
                    return selectedTargetNetId != 0 && entity.netId == selectedTargetNetId;
                });
            if (selectedTargetNetId != 0 && !selectedStillVisible)
                selectedTargetNetId = 0;

            RmlHudData hudData{};
            const WorldRenderEntity* ownEntity = nullptr;
            const WorldRenderEntity* targetEntity = nullptr;
            for (const WorldRenderEntity& entity : frameEntities)
            {
                if (selectedTargetNetId != 0 && entity.netId == selectedTargetNetId)
                    targetEntity = &entity;
            }
            if (!ownEntity && !frameEntities.empty())
                ownEntity = &frameEntities.front();
            if (ownEntity)
            {
                hudData.playerName = ownEntity->name.empty() ? "Player" : ownEntity->name;
                hudData.playerLevel = static_cast<int>(std::max(1u, ownEntity->level));
                hudData.currentHp = ownEntity->hpCurrent;
                hudData.maxHp = ownEntity->hpMax <= 0.0f ? 1.0f : ownEntity->hpMax;
                const WorldVec3 displayPos = ServerMetersToDisplay(ownEntity->position);
                hudData.playerX = displayPos.x;
                hudData.playerZ = displayPos.z;
            }
            hudData.hasTarget = targetEntity != nullptr && targetEntity != ownEntity;
            if (hudData.hasTarget)
            {
                hudData.targetName = targetEntity->name.empty() ? "Target" : targetEntity->name;
                hudData.targetLevel = static_cast<int>(std::max(1u, targetEntity->level));
                hudData.targetCurrentHp = targetEntity->hpCurrent;
                hudData.targetMaxHp = targetEntity->hpMax <= 0.0f ? 1.0f : targetEntity->hpMax;
            }
            runtimeUi->UpdateHud(hudData);

#if defined(IXTREEME_WITH_EDITOR)
            if (terrainOk)
            {
                terrain.SetMapEditorOpen(runtimeSession->IsMapEditorOpen());
                const MapEditorSettings editorSettings = editorImGui.GetMapEditorSettings();
                terrain.SetMapEditorSettings(editorSettings);
                SceneData pendingScene;
                if (SceneManager::Instance().ConsumePendingScene(pendingScene))
                    applySceneData(pendingScene);
                MapEditorCommands commands = runtimeSession->ConsumeMapEditorCommands();
                MergeMapEditorCommands(commands, editorImGui.ConsumeCommands());
                if (commands.gizmoSettingsChanged)
                {
                    editorGizmoMode = commands.gizmoOperation;
                    editorGizmoSnapEnabled = commands.gizmoSnapEnabled;
                    editorGizmoSnapValue = std::max(commands.gizmoSnapValue, 0.001f);
                    Tracenf("[EDITOR-GIZMO] Settings applied: operation=%d snap=%d value=%.2f",
                        static_cast<int>(editorGizmoMode),
                        editorGizmoSnapEnabled ? 1 : 0,
                        editorGizmoSnapValue);
                }
                auto spawnAtCameraCenter = [&]() {
                    std::optional<WorldVec3> hit = RaycastTerrainPoint(terrain,
                        frameCamera,
                        renderSize.width,
                        renderSize.height,
                        static_cast<int>(renderSize.width / 2u),
                        static_cast<int>(renderSize.height / 2u));
                    if (hit)
                        return *hit;

                    WorldVec3 fallback = WorldAdd(frameCamera.eye, WorldScale(CameraForward(frameCamera), 30.0f));
                    fallback.y = terrain.SampleHeight(fallback);
                    return fallback;
                };
                auto spawnAtScreenPosition = [&](float screenX, float screenY) {
                    const int maxX = renderSize.width > 0 ? static_cast<int>(renderSize.width - 1u) : 0;
                    const int maxY = renderSize.height > 0 ? static_cast<int>(renderSize.height - 1u) : 0;
                    const int mouseX = std::clamp(static_cast<int>(std::round(screenX)), 0, maxX);
                    const int mouseY = std::clamp(static_cast<int>(std::round(screenY)), 0, maxY);
                    std::optional<WorldVec3> hit = RaycastTerrainPoint(terrain,
                        frameCamera,
                        renderSize.width,
                        renderSize.height,
                        mouseX,
                        mouseY);
                    if (hit)
                        return *hit;

                    WorldVec3 fallback = WorldAdd(frameCamera.eye,
                        WorldScale(ScreenRayDirection(frameCamera, renderSize.width, renderSize.height, mouseX, mouseY), 30.0f));
                    fallback.y = terrain.SampleHeight(fallback);
                    return fallback;
                };
                auto selectHierarchyEntity = [&](HierarchyEntityType type, std::uint32_t id, std::uint64_t flecsEntity = 0) {
                    switch (type)
                    {
                    case HierarchyEntityType::Terrain:
                        selectedEditorObject = {SelectedEditorObjectType::Terrain, id, flecsEntity};
                        runtimeSession->SetEditorStatus("Selected terrain");
                        break;
                    case HierarchyEntityType::WaterBody:
                        selectedEditorObject = {SelectedEditorObjectType::WaterBody, id, flecsEntity};
                        runtimeSession->SetEditorStatus("Selected water body #" + std::to_string(id));
                        break;
                    case HierarchyEntityType::PointLight:
                        selectedEditorObject = {SelectedEditorObjectType::PointLight, id, flecsEntity};
                        runtimeSession->SetEditorStatus("Selected point light #" + std::to_string(id));
                        break;
                    case HierarchyEntityType::SpotLight:
                        selectedEditorObject = {SelectedEditorObjectType::SpotLight, id, flecsEntity};
                        runtimeSession->SetEditorStatus("Selected spot light #" + std::to_string(id));
                        break;
                    case HierarchyEntityType::MeshEntity:
                        selectedEditorObject = {SelectedEditorObjectType::MeshEntity, id, flecsEntity};
                        runtimeSession->SetEditorStatus("Selected mesh entity #" + std::to_string(id));
                        break;
                    default:
                        break;
                    }
                    Tracenf("[HIERARCHY] Selected entity: flecs=%llu id=%u type=%d",
                        static_cast<unsigned long long>(flecsEntity), id, static_cast<int>(type));
                };
                auto focusHierarchyEntity = [&](HierarchyEntityType type, std::uint32_t id) {
                    std::optional<WorldVec3> target;
                    if (type == HierarchyEntityType::Terrain)
                    {
                        target = WorldVec3{0.0f, 0.0f, 0.0f};
                    }
                    else if (type == HierarchyEntityType::WaterBody)
                    {
                        auto it = std::find_if(editorWaterBodies.begin(), editorWaterBodies.end(),
                            [&](const WaterBody& body) { return body.id == id; });
                        if (it != editorWaterBodies.end())
                            target = WaterBodyCenter(*it);
                    }
                    else if (type == HierarchyEntityType::PointLight)
                    {
                        auto it = std::find_if(editorPointLights.begin(), editorPointLights.end(),
                            [&](const PointLight& light) { return light.id == id; });
                        if (it != editorPointLights.end())
                            target = WorldVec3{it->position[0], it->position[1], it->position[2]};
                    }
                    else if (type == HierarchyEntityType::SpotLight)
                    {
                        auto it = std::find_if(editorSpotLights.begin(), editorSpotLights.end(),
                            [&](const SpotLight& light) { return light.id == id; });
                        if (it != editorSpotLights.end())
                            target = WorldVec3{it->position[0], it->position[1], it->position[2]};
                    }
                    else if (type == HierarchyEntityType::MeshEntity)
                    {
                        auto it = std::find_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                            [&](const MeshSceneEntity& mesh) { return mesh.id == id; });
                        if (it != editorMeshEntities.end())
                            target = WorldVec3{it->position[0], it->position[1], it->position[2]};
                    }
                    if (!target)
                        return;
                    cameraController.FocusOn(*target);
                    runtimeSession->SetEditorStatus("Focused camera on entity #" + std::to_string(id));
                    Tracenf("[HIERARCHY] Focused camera on entity: id=%u type=%d", id, static_cast<int>(type));
                };
                auto deleteHierarchyEntity = [&](HierarchyEntityType type, std::uint32_t id) {
                    if (type == HierarchyEntityType::WaterBody)
                    {
                        editorWaterBodies.erase(std::remove_if(editorWaterBodies.begin(), editorWaterBodies.end(),
                            [&](const WaterBody& body) { return body.id == id; }), editorWaterBodies.end());
                        editorWaterBodiesDirty = true;
                    }
                    else if (type == HierarchyEntityType::Terrain)
                    {
                        if (terrainOk)
                            terrain.ClearTerrain(device);
                    }
                    else if (type == HierarchyEntityType::PointLight)
                    {
                        editorPointLights.erase(std::remove_if(editorPointLights.begin(), editorPointLights.end(),
                            [&](const PointLight& light) { return light.id == id; }), editorPointLights.end());
                    }
                    else if (type == HierarchyEntityType::SpotLight)
                    {
                        editorSpotLights.erase(std::remove_if(editorSpotLights.begin(), editorSpotLights.end(),
                            [&](const SpotLight& light) { return light.id == id; }), editorSpotLights.end());
                    }
                    else if (type == HierarchyEntityType::MeshEntity)
                    {
                        removeStaticMeshSpatialEntity(id);
                        editorMeshEntities.erase(std::remove_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                            [&](const MeshSceneEntity& mesh) { return mesh.id == id; }), editorMeshEntities.end());
                        rebuildMeshEntityLookup();
                    }
                    if ((type == HierarchyEntityType::Terrain && selectedEditorObject.type == SelectedEditorObjectType::Terrain) ||
                        (type == HierarchyEntityType::WaterBody && selectedEditorObject.type == SelectedEditorObjectType::WaterBody && selectedEditorObject.id == id) ||
                        (type == HierarchyEntityType::PointLight && selectedEditorObject.type == SelectedEditorObjectType::PointLight && selectedEditorObject.id == id) ||
                        (type == HierarchyEntityType::SpotLight && selectedEditorObject.type == SelectedEditorObjectType::SpotLight && selectedEditorObject.id == id) ||
                        (type == HierarchyEntityType::MeshEntity && selectedEditorObject.type == SelectedEditorObjectType::MeshEntity && selectedEditorObject.id == id))
                    {
                        selectedEditorObject = {};
                    }
                    SceneManager::Instance().MarkDirty();
                    runtimeSession->SetEditorStatus("Deleted hierarchy entity #" + std::to_string(id));
                    Tracenf("[HIERARCHY] Deleted entity: id=%u type=%d", id, static_cast<int>(type));
                };
                auto duplicateHierarchyEntity = [&](HierarchyEntityType type, std::uint32_t id) {
                    if (type == HierarchyEntityType::WaterBody)
                    {
                        auto it = std::find_if(editorWaterBodies.begin(), editorWaterBodies.end(),
                            [&](const WaterBody& body) { return body.id == id; });
                        if (it == editorWaterBodies.end())
                            return;
                        WaterBody copy = *it;
                        copy.id = nextEditorWaterBodyId++;
                        copy.name = makeUniqueSceneEntityName((copy.name.empty() ? "Water Body" : copy.name) + " Copy");
                        copy.bboxMin[0] += 5.0f;
                        copy.bboxMax[0] += 5.0f;
                        copy.editorHidden = false;
                        editorWaterBodies.push_back(copy);
                        selectedEditorObject = {SelectedEditorObjectType::WaterBody, copy.id};
                        editorWaterBodiesDirty = true;
                        SceneManager::Instance().MarkDirty();
                        Tracenf("[HIERARCHY] Duplicated entity: original=%u new=%u", id, copy.id);
                    }
                    else if (type == HierarchyEntityType::Terrain)
                    {
                        runtimeSession->SetEditorStatus("Only one terrain is supported per scene");
                        Tracen("[HIERARCHY] Duplicate ignored for Terrain: one terrain per scene");
                    }
                    else if (type == HierarchyEntityType::PointLight)
                    {
                        if (editorPointLights.size() >= kMaxDynamicPointLights)
                            return;
                        auto it = std::find_if(editorPointLights.begin(), editorPointLights.end(),
                            [&](const PointLight& light) { return light.id == id; });
                        if (it == editorPointLights.end())
                            return;
                        PointLight copy = *it;
                        copy.id = nextEditorLightId++;
                        copy.name = makeUniqueSceneEntityName((copy.name.empty() ? "Point Light" : copy.name) + " Copy");
                        copy.position[0] += 5.0f;
                        copy.editorHidden = false;
                        editorPointLights.push_back(copy);
                        selectedEditorObject = {SelectedEditorObjectType::PointLight, copy.id};
                        SceneManager::Instance().MarkDirty();
                        Tracenf("[HIERARCHY] Duplicated entity: original=%u new=%u", id, copy.id);
                    }
                    else if (type == HierarchyEntityType::SpotLight)
                    {
                        if (editorSpotLights.size() >= kMaxDynamicSpotLights)
                            return;
                        auto it = std::find_if(editorSpotLights.begin(), editorSpotLights.end(),
                            [&](const SpotLight& light) { return light.id == id; });
                        if (it == editorSpotLights.end())
                            return;
                        SpotLight copy = *it;
                        copy.id = nextEditorLightId++;
                        copy.name = makeUniqueSceneEntityName((copy.name.empty() ? "Spot Light" : copy.name) + " Copy");
                        copy.position[0] += 5.0f;
                        copy.editorHidden = false;
                        editorSpotLights.push_back(copy);
                        selectedEditorObject = {SelectedEditorObjectType::SpotLight, copy.id};
                        SceneManager::Instance().MarkDirty();
                        Tracenf("[HIERARCHY] Duplicated entity: original=%u new=%u", id, copy.id);
                    }
                    else if (type == HierarchyEntityType::MeshEntity)
                    {
                        auto it = std::find_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                            [&](const MeshSceneEntity& mesh) { return mesh.id == id; });
                        if (it == editorMeshEntities.end())
                            return;
                        MeshSceneEntity copy = *it;
                        copy.id = nextEditorMeshEntityId++;
                        copy.name = makeUniqueSceneEntityName((copy.name.empty() ? "Mesh Entity" : copy.name) + " Copy");
                        copy.position[0] += 5.0f;
                        copy.editorHidden = false;
                        editorMeshEntities.push_back(copy);
                        editorMeshEntityLookup[copy.id] = editorMeshEntities.size() - 1u;
                        syncStaticMeshSpatialEntity(editorMeshEntities.back());
                        selectedEditorObject = {SelectedEditorObjectType::MeshEntity, copy.id};
                        SceneManager::Instance().MarkDirty();
                        Tracenf("[HIERARCHY] Duplicated entity: original=%u new=%u", id, copy.id);
                    }
                    runtimeSession->SetEditorStatus("Duplicated hierarchy entity #" + std::to_string(id));
                };
                auto renameHierarchyEntity = [&](HierarchyEntityType type, std::uint32_t id, const std::string& name) {
                    if (name.empty())
                        return;
                    if (type == HierarchyEntityType::WaterBody)
                    {
                        auto it = std::find_if(editorWaterBodies.begin(), editorWaterBodies.end(),
                            [&](const WaterBody& body) { return body.id == id; });
                        if (it != editorWaterBodies.end())
                            it->name = name;
                    }
                    else if (type == HierarchyEntityType::Terrain)
                    {
                        if (terrainOk && terrain.HasTerrain())
                        {
                            TerrainSceneData data = terrain.GetTerrainSceneData();
                            data.name = name;
                            terrain.SetTerrainSceneData(data);
                        }
                    }
                    else if (type == HierarchyEntityType::PointLight)
                    {
                        auto it = std::find_if(editorPointLights.begin(), editorPointLights.end(),
                            [&](const PointLight& light) { return light.id == id; });
                        if (it != editorPointLights.end())
                            it->name = name;
                    }
                    else if (type == HierarchyEntityType::SpotLight)
                    {
                        auto it = std::find_if(editorSpotLights.begin(), editorSpotLights.end(),
                            [&](const SpotLight& light) { return light.id == id; });
                        if (it != editorSpotLights.end())
                            it->name = name;
                    }
                    else if (type == HierarchyEntityType::MeshEntity)
                    {
                        auto it = std::find_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                            [&](const MeshSceneEntity& mesh) { return mesh.id == id; });
                        if (it != editorMeshEntities.end())
                            it->name = name;
                    }
                    SceneManager::Instance().MarkDirty();
                    runtimeSession->SetEditorStatus("Renamed hierarchy entity #" + std::to_string(id));
                    Tracenf("[HIERARCHY] Renamed entity: id=%u new_name=%s", id, name.c_str());
                };
                auto toggleHierarchyHidden = [&](HierarchyEntityType type, std::uint32_t id) {
                    bool hidden = false;
                    if (type == HierarchyEntityType::WaterBody)
                    {
                        auto it = std::find_if(editorWaterBodies.begin(), editorWaterBodies.end(),
                            [&](const WaterBody& body) { return body.id == id; });
                        if (it != editorWaterBodies.end())
                        {
                            it->editorHidden = !it->editorHidden;
                            hidden = it->editorHidden;
                            editorWaterBodiesDirty = true;
                        }
                    }
                    else if (type == HierarchyEntityType::Terrain)
                    {
                        if (terrainOk && terrain.HasTerrain())
                        {
                            TerrainSceneData data = terrain.GetTerrainSceneData();
                            data.editorHidden = !data.editorHidden;
                            hidden = data.editorHidden;
                            terrain.SetTerrainSceneData(data);
                        }
                    }
                    else if (type == HierarchyEntityType::PointLight)
                    {
                        auto it = std::find_if(editorPointLights.begin(), editorPointLights.end(),
                            [&](const PointLight& light) { return light.id == id; });
                        if (it != editorPointLights.end())
                        {
                            it->editorHidden = !it->editorHidden;
                            hidden = it->editorHidden;
                        }
                    }
                    else if (type == HierarchyEntityType::SpotLight)
                    {
                        auto it = std::find_if(editorSpotLights.begin(), editorSpotLights.end(),
                            [&](const SpotLight& light) { return light.id == id; });
                        if (it != editorSpotLights.end())
                        {
                            it->editorHidden = !it->editorHidden;
                            hidden = it->editorHidden;
                        }
                    }
                    else if (type == HierarchyEntityType::MeshEntity)
                    {
                        auto it = std::find_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                            [&](const MeshSceneEntity& mesh) { return mesh.id == id; });
                        if (it != editorMeshEntities.end())
                        {
                            it->editorHidden = !it->editorHidden;
                            hidden = it->editorHidden;
                        }
                    }
                    SceneManager::Instance().MarkDirty();
                    runtimeSession->SetEditorStatus(std::string(hidden ? "Hidden" : "Shown") + " hierarchy entity #" + std::to_string(id));
                    Tracenf("[HIERARCHY] Toggled editor-visibility: id=%u hidden=%d", id, hidden ? 1 : 0);
                };
#if defined(IXTREEME_WITH_EDITOR)
                if (commands.enterPlayMode)
                {
                    if (SceneManager::Instance().HasOpenScene())
                        editorPlay.state.mode = EditorPlayMode::Play;
                    else
                        Tracen("[EDIT-PLAY] Play ignored: no open scene");
                }
                if (commands.exitPlayMode)
                    editorPlay.state.mode = EditorPlayMode::Edit;
                if (commands.pausePlayMode && editorPlay.state.mode == EditorPlayMode::Play)
                    editorPlay.state.mode = EditorPlayMode::PlayPaused;
                if (commands.resumePlayMode && editorPlay.state.mode == EditorPlayMode::PlayPaused)
                    editorPlay.state.mode = EditorPlayMode::Play;

                if (editorPlay.appliedMode == EditorPlayMode::Edit &&
                    editorPlay.state.mode != EditorPlayMode::Edit)
                {
                    Tracen("[EDIT-PLAY] Entering Play Mode");
                    SceneManager::Instance().SetCurrentSceneSnapshot(buildSceneSnapshot());
                    editorPlay.playStartSceneWasOpen = SceneManager::Instance().HasOpenScene();
                    editorPlay.playStartSceneDirty = SceneManager::Instance().IsDirty();
                    editorPlay.playStartSceneSnapshot = SceneManager::Instance().GetCurrentScene();
                    editorPlay.playStartScenePath = SceneManager::Instance().GetCurrentScenePath();
                    Tracenf("[EDIT-PLAY] Play Mode: starting scene = %s",
                        editorPlay.playStartScenePath.empty() ? "<unsaved>" : editorPlay.playStartScenePath.c_str());
                    editorPlay.editorCameraSnapshot = cameraController.SaveSnapshot();
                    selectedEditorObject = {};
                    editorObjectDragActive = false;
                    waterSculptStrokeActive = false;
                    editorWaterBodiesDirty = true;
                    terrain.SetWaterSculptBrush(false, 0.0f, 0.0f, 0.0f, true);
                    cameraController.SetFreeCameraEnabled(true);
                    runtimeSession->Start(SceneManager::Instance().GetCurrentScene());
                    editorPlay.state.frameCount = 0;
                    editorPlay.state.elapsedSeconds = 0.0;
                    editorPlay.appliedMode = editorPlay.state.mode;
                    Tracen("[EDIT-PLAY] Default runtime Play mode enabled (no player UI, network, or character)");
                    Tracen("[EDIT-PLAY] Play Mode active");
                }
                else if (editorPlay.appliedMode != EditorPlayMode::Edit &&
                    editorPlay.state.mode == EditorPlayMode::Edit)
                {
                    Tracenf("[EDIT-PLAY] Exiting Play Mode (after %.1fs, %d frames)",
                        editorPlay.state.elapsedSeconds,
                        editorPlay.state.frameCount);
                    runtimeUi->HideAll();
                    runtimeSession->Stop();
                    editorWaterBodiesDirty = true;
                    if (editorPlay.playStartSceneWasOpen)
                    {
                        SceneManager::Instance().RestoreSceneSnapshot(
                            editorPlay.playStartSceneSnapshot,
                            editorPlay.playStartScenePath,
                            editorPlay.playStartSceneDirty);
                        Tracenf("[EDIT-PLAY] Restored starting scene: %s",
                            editorPlay.playStartScenePath.empty() ? "<unsaved>" : editorPlay.playStartScenePath.c_str());
                    }
                    editorPlay.playStartScenePath.clear();
                    editorPlay.playStartSceneWasOpen = false;
                    editorPlay.playStartSceneDirty = false;
                    if (editorPlay.editorCameraSnapshot)
                    {
                        cameraController.RestoreSnapshot(*editorPlay.editorCameraSnapshot);
                        editorPlay.editorCameraSnapshot.reset();
                    }
                    movement.Clear();
                    editorPlay.state.frameCount = 0;
                    editorPlay.state.elapsedSeconds = 0.0;
                    editorPlay.appliedMode = EditorPlayMode::Edit;
                    Tracen("[EDIT-PLAY] Embedded server stopped");
                    Tracen("[EDIT-PLAY] Snapshot restored");
                    Tracen("[EDIT-PLAY] Edit Mode active");
                }
                else if (editorPlay.appliedMode != editorPlay.state.mode)
                {
                    editorPlay.appliedMode = editorPlay.state.mode;
                    Tracen(editorPlay.state.mode == EditorPlayMode::PlayPaused
                        ? "[EDIT-PLAY] Play paused"
                        : "[EDIT-PLAY] Play resumed");
                }
                if (editorPlay.state.mode != EditorPlayMode::Edit)
                {
                    if (editorPlay.state.mode == EditorPlayMode::Play)
                        runtimeSession->Tick(deltaSeconds);
                    commands.addWaterBody = false;
                    commands.createTerrain = false;
                    commands.addMeshEntity = false;
                    commands.addComponentToSelectedEntity = false;
                    commands.addComponentType = EditorComponentType::None;
                    commands.addComponentTypeId.clear();
                    commands.removeComponentFromSelectedEntity = false;
                    commands.removeComponentTypeId.clear();
                    commands.lodQualityCommitRequested = false;
                    commands.lodQualityCommitEntityId = 0;
                    commands.assignMeshAssetToSelectedEntity = false;
                    commands.addPointLight = false;
                    commands.addSpotLight = false;
                    commands.deleteSelectedLight = false;
                    commands.deleteSelectedWaterBody = false;
                    commands.deleteSelectedMeshEntity = false;
                    commands.selectedLightChanged = false;
                    commands.selectedWaterBodyChanged = false;
                    commands.selectedMeshEntityChanged = false;
                    commands.hierarchyDeleteEntity = false;
                    commands.hierarchyDuplicateEntity = false;
                    commands.hierarchyRenameEntity = false;
                    commands.hierarchyToggleHidden = false;
                    commands.paletteSlotChanged = false;
                    commands.save = false;
                    commands.reload = false;
                    commands.undo = false;
                }
#endif
                if (commands.hierarchySelectEntity)
                    selectHierarchyEntity(commands.hierarchyEntityType, commands.hierarchyEntityId, commands.hierarchyEntityHandle);
                if (commands.hierarchyFocusEntity)
                    focusHierarchyEntity(commands.hierarchyEntityType, commands.hierarchyEntityId);
                if (commands.hierarchyDeleteEntity)
                    deleteHierarchyEntity(commands.hierarchyEntityType, commands.hierarchyEntityId);
                if (commands.hierarchyDuplicateEntity)
                    duplicateHierarchyEntity(commands.hierarchyEntityType, commands.hierarchyEntityId);
                if (commands.hierarchyRenameEntity)
                    renameHierarchyEntity(commands.hierarchyEntityType, commands.hierarchyEntityId, commands.hierarchyRenameValue);
                if (commands.hierarchyToggleHidden)
                    toggleHierarchyHidden(commands.hierarchyEntityType, commands.hierarchyEntityId);
                auto selectedEntityPosition = [&]() {
                    if (selectedEditorObject.type == SelectedEditorObjectType::Terrain)
                    {
                        return WorldVec3{0.0f, 0.0f, 0.0f};
                    }
                    if (selectedEditorObject.type == SelectedEditorObjectType::WaterBody)
                    {
                        auto it = std::find_if(editorWaterBodies.begin(), editorWaterBodies.end(),
                            [&](const WaterBody& body) { return body.id == selectedEditorObject.id; });
                        if (it != editorWaterBodies.end())
                            return WaterBodyCenter(*it);
                    }
                    else if (selectedEditorObject.type == SelectedEditorObjectType::PointLight)
                    {
                        auto it = std::find_if(editorPointLights.begin(), editorPointLights.end(),
                            [&](const PointLight& light) { return light.id == selectedEditorObject.id; });
                        if (it != editorPointLights.end())
                            return WorldVec3{it->position[0], it->position[1], it->position[2]};
                    }
                    else if (selectedEditorObject.type == SelectedEditorObjectType::SpotLight)
                    {
                        auto it = std::find_if(editorSpotLights.begin(), editorSpotLights.end(),
                            [&](const SpotLight& light) { return light.id == selectedEditorObject.id; });
                        if (it != editorSpotLights.end())
                            return WorldVec3{it->position[0], it->position[1], it->position[2]};
                    }
                    else if (selectedEditorObject.type == SelectedEditorObjectType::MeshEntity)
                    {
                        auto it = std::find_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                            [&](const MeshSceneEntity& mesh) { return mesh.id == selectedEditorObject.id; });
                        if (it != editorMeshEntities.end())
                            return WorldVec3{it->position[0], it->position[1], it->position[2]};
                    }
                    return spawnAtCameraCenter();
                };
                auto resolveModelAsset = [&](const std::string& assetId) -> std::optional<AssetLibrary::Entry> {
                    if (assetId.empty())
                        return std::nullopt;
                    std::string error;
                    if (ProjectManager::Instance().HasProject())
                    {
                        AssetLibrary projectAssets(ProjectManager::Instance().ProjectRoot(),
                            ProjectManager::Instance().AssetRootPath());
                        if (projectAssets.Initialize())
                        {
                            auto entry = projectAssets.FindById(assetId);
                            if (entry && entry->category == AssetLibrary::Category::Model)
                            {
                                entry->originalPath = projectAssets.AssetRelativePath(*entry);
                                return entry;
                            }
                        }
                    }
                    if (auto root = assets.RootPath())
                    {
                        AssetLibrary engineAssets(*root);
                        if (engineAssets.Initialize())
                        {
                            auto entry = engineAssets.FindById(assetId);
                            if (entry && entry->category == AssetLibrary::Category::Model)
                            {
                                entry->originalPath = engineAssets.AssetRelativePath(*entry);
                                return entry;
                            }
                        }
                    }
                    return std::nullopt;
                };
                auto createMeshEntityAt = [&](const std::string& assetId, WorldVec3 spawn) {
                    auto entry = resolveModelAsset(assetId);
                    MeshSceneEntity mesh{};
                    mesh.id = nextEditorMeshEntityId++;
                    mesh.meshAssetId = assetId;
                    mesh.meshAssetPath = entry ? entry->originalPath : assetId;
                    const std::string baseName = entry
                        ? (entry->displayName.empty() ? std::filesystem::path(entry->filename).stem().string() : entry->displayName)
                        : (assetId.empty() ? std::string("Mesh Entity") : assetId);
                    mesh.name = makeUniqueSceneEntityName(baseName);
                    mesh.position[0] = spawn.x;
                    mesh.position[1] = spawn.y;
                    mesh.position[2] = spawn.z;
                    mesh.skinned = false;
                    editorMeshEntities.push_back(mesh);
                    editorMeshEntityLookup[mesh.id] = editorMeshEntities.size() - 1u;
                    syncStaticMeshSpatialEntity(editorMeshEntities.back());
                    selectedEditorObject = {SelectedEditorObjectType::MeshEntity, mesh.id};
                    editorGizmoMode = EditorGizmoMode::Translate;
                    SceneManager::Instance().MarkDirty();
                    runtimeSession->SetEditorStatus("Mesh entity spawned: " + mesh.name);
                    Tracenf("[MESH-ENTITY] Spawned: id=%u asset_id=%s path=%s position=(%.2f,%.2f,%.2f)",
                        mesh.id,
                        mesh.meshAssetId.c_str(),
                        mesh.meshAssetPath.c_str(),
                        spawn.x,
                        spawn.y,
                        spawn.z);
                };
                auto addEditorNoteComponentToSelectedMesh = [&]() {
                    if (selectedEditorObject.type != SelectedEditorObjectType::MeshEntity)
                        return false;
                    auto it = std::find_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                        [&](const MeshSceneEntity& mesh) { return mesh.id == selectedEditorObject.id; });
                    if (it == editorMeshEntities.end())
                        return false;
                    const auto existing = std::find_if(it->editorComponents.begin(), it->editorComponents.end(),
                        [](const EditorAttachedComponent& component) { return component.type == "editor.note"; });
                    if (existing != it->editorComponents.end())
                        return false;
                    EditorAttachedComponent component{};
                    component.type = "editor.note";
                    component.displayName = "Note";
                    component.category = "Editor";
                    component.note = "New note";
                    it->editorComponents.push_back(component);
                    if (selectedEditorObject.flecsEntity != 0)
                        ecs_add_id(editorHierarchyWorld.get(), static_cast<ecs_entity_t>(selectedEditorObject.flecsEntity), editorNoteComponentEntity);
                    SceneManager::Instance().MarkDirty();
                    Tracenf("[INSPECTOR-COMP] add entity=%u component=Note", it->id);
                    return true;
                };
                auto addLodComponentToSelectedMesh = [&]() {
                    if (selectedEditorObject.type != SelectedEditorObjectType::MeshEntity)
                        return false;
                    auto it = std::find_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                        [&](const MeshSceneEntity& mesh) { return mesh.id == selectedEditorObject.id; });
                    if (it == editorMeshEntities.end())
                        return false;
                    const auto existing = std::find_if(it->editorComponents.begin(), it->editorComponents.end(),
                        [](const EditorAttachedComponent& component) { return component.type == "rendering.lod"; });
                    if (existing == it->editorComponents.end())
                        it->editorComponents.push_back({"rendering.lod", "LOD Group", "Rendering", {}});
                    const std::optional<LodConfig> assetDefault = editorImGui.FindModelLodDefault(it->meshAssetId);
                    it->lod.enabled = true;
                    it->lod.overrideAssetDefault = false;
                    it->lod.config = assetDefault.value_or(LodConfig{});
                    SceneManager::Instance().MarkDirty();
                    Tracenf("[INSPECTOR-COMP] add entity=%u component=LOD Group", it->id);
                    if (LodLogsEnabled())
                    {
                        Tracenf("[LOD] component added entity=%u asset=%s source=%s",
                            it->id,
                            it->meshAssetId.empty() ? it->meshAssetPath.c_str() : it->meshAssetId.c_str(),
                            assetDefault ? "assetDefault" : "engineDefault");
                    }
                    return true;
                };
                auto removeEditorComponentFromSelectedMesh = [&](const std::string& componentType) {
                    if (selectedEditorObject.type != SelectedEditorObjectType::MeshEntity)
                        return false;
                    auto it = std::find_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                        [&](const MeshSceneEntity& mesh) { return mesh.id == selectedEditorObject.id; });
                    if (it == editorMeshEntities.end())
                        return false;
                    const std::size_t oldSize = it->editorComponents.size();
                    it->editorComponents.erase(std::remove_if(it->editorComponents.begin(), it->editorComponents.end(),
                        [&](const EditorAttachedComponent& component) { return component.type == componentType; }),
                        it->editorComponents.end());
                    if (it->editorComponents.size() == oldSize)
                        return false;
                    if (componentType == "editor.note" && selectedEditorObject.flecsEntity != 0)
                        ecs_remove_id(editorHierarchyWorld.get(), static_cast<ecs_entity_t>(selectedEditorObject.flecsEntity), editorNoteComponentEntity);
                    if (componentType == "rendering.lod")
                        it->lod = {};
                    SceneManager::Instance().MarkDirty();
                    Tracenf("[INSPECTOR-COMP] remove entity=%u component=%s",
                        it->id,
                        componentType == "editor.note" ? "Note" : (componentType == "rendering.lod" ? "LOD Group" : componentType.c_str()));
                    return true;
                };
                if (commands.addComponentToSelectedEntity)
                {
                    if (commands.addComponentTypeId == "editor.note")
                    {
                        if (addEditorNoteComponentToSelectedMesh())
                            runtimeSession->SetEditorStatus("Added Note component");
                    }
                    else if (commands.addComponentTypeId == "rendering.lod")
                    {
                        if (addLodComponentToSelectedMesh())
                            runtimeSession->SetEditorStatus("Added LOD Group component");
                    }
                    else
                    {
                    const WorldVec3 spawn = selectedEntityPosition();
                    if (commands.addComponentType == EditorComponentType::WaterBody &&
                        selectedEditorObject.type != SelectedEditorObjectType::WaterBody)
                    {
                        if (!terrainOk || !terrain.HasTerrain())
                        {
                            runtimeSession->SetEditorStatus("Create a terrain before adding water");
                        }
                        else
                        {
                        WaterBody body{};
                        body.id = nextEditorWaterBodyId++;
                        body.name = makeUniqueSceneEntityName("Water Body");
                        body.materialId = "watermat_Default_Water";
                        body.waterLevelY = spawn.y;
                        body.bboxMin[0] = spawn.x - 5.0f;
                        body.bboxMax[0] = spawn.x + 5.0f;
                        body.bboxMin[1] = spawn.z - 5.0f;
                        body.bboxMax[1] = spawn.z + 5.0f;
                        RegenerateCircularWaterMask(body);
                        editorWaterBodies.push_back(body);
                        selectedEditorObject = {SelectedEditorObjectType::WaterBody, body.id};
                        editorGizmoMode = EditorGizmoMode::Translate;
                        editorWaterBodiesDirty = true;
                        SceneManager::Instance().MarkDirty();
                        runtimeSession->SetEditorStatus("Added Water Body component");
                        }
                    }
                    else if (commands.addComponentType == EditorComponentType::PointLight &&
                        selectedEditorObject.type != SelectedEditorObjectType::PointLight)
                    {
                        if (editorPointLights.size() >= kMaxDynamicPointLights)
                        {
                            runtimeSession->SetEditorStatus("Maximum point lights reached (16)");
                        }
                        else
                        {
                            PointLight light{};
                            light.id = nextEditorLightId++;
                            light.name = makeUniqueSceneEntityName("Point Light");
                            light.position[0] = spawn.x;
                            light.position[1] = spawn.y + 1.8f;
                            light.position[2] = spawn.z;
                            editorPointLights.push_back(light);
                            selectedEditorObject = {SelectedEditorObjectType::PointLight, light.id};
                            editorGizmoMode = EditorGizmoMode::Translate;
                            SceneManager::Instance().MarkDirty();
                            runtimeSession->SetEditorStatus("Added Point Light component");
                        }
                    }
                    else if (commands.addComponentType == EditorComponentType::SpotLight &&
                        selectedEditorObject.type != SelectedEditorObjectType::SpotLight)
                    {
                        if (editorSpotLights.size() >= kMaxDynamicSpotLights)
                        {
                            runtimeSession->SetEditorStatus("Maximum spot lights reached (16)");
                        }
                        else
                        {
                            SpotLight light{};
                            light.id = nextEditorLightId++;
                            light.name = makeUniqueSceneEntityName("Spot Light");
                            light.position[0] = spawn.x;
                            light.position[1] = spawn.y + 4.0f;
                            light.position[2] = spawn.z;
                            light.rotation[0] = -1.5708f;
                            editorSpotLights.push_back(light);
                            selectedEditorObject = {SelectedEditorObjectType::SpotLight, light.id};
                            editorGizmoMode = EditorGizmoMode::Translate;
                            SceneManager::Instance().MarkDirty();
                            runtimeSession->SetEditorStatus("Added Spot Light component");
                        }
                    }
                    else if (commands.addComponentType == EditorComponentType::MeshRenderer &&
                        selectedEditorObject.type != SelectedEditorObjectType::MeshEntity)
                    {
                        createMeshEntityAt(commands.assignMeshAssetId.empty() ? commands.meshAssetId : commands.assignMeshAssetId, spawn);
                    }
                    }
                }
                if (commands.removeComponentFromSelectedEntity)
                {
                    if (commands.removeComponentTypeId == "builtin.transform")
                    {
                        Tracen("[INSPECTOR-COMP] remove ignored component=Transform reason=not-removable");
                    }
                    else if (removeEditorComponentFromSelectedMesh(commands.removeComponentTypeId))
                    {
                        runtimeSession->SetEditorStatus("Removed component");
                    }
                }
                if (commands.addMeshEntity)
                {
                    const WorldVec3 spawn = commands.meshDropScreenPositionValid
                        ? spawnAtScreenPosition(commands.meshDropScreenPosition[0], commands.meshDropScreenPosition[1])
                        : spawnAtCameraCenter();
                    if (commands.meshDropScreenPositionValid)
                    {
                        Tracenf("[DND] drop -> spawn at raycast pos=(%.2f,%.2f,%.2f)",
                            spawn.x,
                            spawn.y,
                            spawn.z);
                    }
                    createMeshEntityAt(commands.meshAssetId, spawn);
                }
                if (commands.createTerrain && terrainOk)
                {
                    TerrainSceneData next = commands.terrainCreate;
                    next.exists = true;
                    if (next.name.empty())
                        next.name = makeUniqueSceneEntityName("Terrain");
                    next.cellSizeMeters = std::max(0.01f, next.cellSizeMeters);
                    next.cellsX = std::max(1u, next.cellsX);
                    next.cellsZ = std::max(1u, next.cellsZ);
                    next.widthMeters = static_cast<float>(next.cellsX) * next.cellSizeMeters;
                    next.depthMeters = static_cast<float>(next.cellsZ) * next.cellSizeMeters;
                    if (terrain.CreateFlatTerrain(device, next))
                    {
                        editorWaterBodies.clear();
                        editorWaterBodiesDirty = true;
                        selectedEditorObject = {SelectedEditorObjectType::Terrain, 1u};
                        editorGizmoMode = EditorGizmoMode::Translate;
                        SceneManager::Instance().MarkDirty();
                        runtimeSession->SetEditorStatus("Terrain created");
                    }
                    else
                    {
                        runtimeSession->SetEditorStatus("Terrain creation failed");
                    }
                }
                if (commands.addWaterBody)
                {
                    if (!terrainOk || !terrain.HasTerrain())
                    {
                        runtimeSession->SetEditorStatus("Create a terrain before adding water");
                    }
                    else
                    {
                    const WorldVec3 spawn = spawnAtCameraCenter();
                    WaterBody body{};
                    body.id = nextEditorWaterBodyId++;
                    body.name = makeUniqueSceneEntityName("Water Body");
                    body.materialId = "watermat_Default_Water";
                    body.waterLevelY = spawn.y;
                    body.bboxMin[0] = spawn.x - 5.0f;
                    body.bboxMax[0] = spawn.x + 5.0f;
                    body.bboxMin[1] = spawn.z - 5.0f;
                    body.bboxMax[1] = spawn.z + 5.0f;
                    RegenerateCircularWaterMask(body);
                    editorWaterBodies.push_back(body);
                    selectedEditorObject = {SelectedEditorObjectType::WaterBody, body.id};
                    editorGizmoMode = EditorGizmoMode::Translate;
                    editorWaterBodiesDirty = true;
                    SceneManager::Instance().MarkDirty();
                    runtimeSession->SetEditorStatus("Water body spawned: id=" + std::to_string(body.id) +
                        " name=" + body.name);
                    Tracenf("[EDITOR-3D-SPAWN] Spawn at cursor: type=water position=(%.2f,%.2f,%.2f)",
                        spawn.x, spawn.y, spawn.z);
                    }
                }
                if (commands.addPointLight)
                {
                    if (editorPointLights.size() >= kMaxDynamicPointLights)
                    {
                        runtimeSession->SetEditorStatus("Maximum point lights reached (16)");
                    }
                    else
                    {
                        PointLight light{};
                        light.id = nextEditorLightId++;
                        light.name = makeUniqueSceneEntityName("Point Light");
                        const WorldVec3 spawn = spawnAtCameraCenter();
                        light.position[0] = spawn.x;
                        light.position[1] = spawn.y + 1.8f;
                        light.position[2] = spawn.z;
                        editorPointLights.push_back(light);
                        selectedEditorObject = {SelectedEditorObjectType::PointLight, light.id};
                        editorGizmoMode = EditorGizmoMode::Translate;
                        SceneManager::Instance().MarkDirty();
                        runtimeSession->SetEditorStatus("Added point light #" + std::to_string(light.id));
                        Tracenf("[EDITOR-3D-SPAWN] Spawn at cursor: type=point_light position=(%.2f,%.2f,%.2f)",
                            spawn.x, spawn.y, spawn.z);
                    }
                }
                if (commands.addSpotLight)
                {
                    if (editorSpotLights.size() >= kMaxDynamicSpotLights)
                    {
                        runtimeSession->SetEditorStatus("Maximum spot lights reached (16)");
                    }
                    else
                    {
                        SpotLight light{};
                        light.id = nextEditorLightId++;
                        light.name = makeUniqueSceneEntityName("Spot Light");
                        const WorldVec3 spawn = spawnAtCameraCenter();
                        light.position[0] = spawn.x;
                        light.position[1] = spawn.y + 4.0f;
                        light.position[2] = spawn.z;
                        light.rotation[0] = -1.5708f;
                        editorSpotLights.push_back(light);
                        selectedEditorObject = {SelectedEditorObjectType::SpotLight, light.id};
                        editorGizmoMode = EditorGizmoMode::Translate;
                        SceneManager::Instance().MarkDirty();
                        runtimeSession->SetEditorStatus("Added spot light #" + std::to_string(light.id));
                        Tracenf("[EDITOR-3D-SPAWN] Spawn at cursor: type=spot_light position=(%.2f,%.2f,%.2f)",
                            spawn.x, spawn.y, spawn.z);
                    }
                }
                if (commands.selectedLightChanged)
                {
                    if (commands.selectedLight.type == DynamicLightType::Point)
                    {
                        auto it = std::find_if(editorPointLights.begin(), editorPointLights.end(),
                            [&](const PointLight& light) { return light.id == commands.selectedLight.point.id; });
                        if (it != editorPointLights.end())
                        {
                            *it = commands.selectedLight.point;
                            selectedEditorObject = {SelectedEditorObjectType::PointLight, it->id};
                            SceneManager::Instance().MarkDirty();
                        }
                    }
                    else if (commands.selectedLight.type == DynamicLightType::Spot)
                    {
                        auto it = std::find_if(editorSpotLights.begin(), editorSpotLights.end(),
                            [&](const SpotLight& light) { return light.id == commands.selectedLight.spot.id; });
                        if (it != editorSpotLights.end())
                        {
                            SpotLight spot = commands.selectedLight.spot;
                            spot.outerConeDegrees = std::clamp(spot.outerConeDegrees, 1.0f, 90.0f);
                            spot.innerConeDegrees = std::clamp(spot.innerConeDegrees, 1.0f, spot.outerConeDegrees);
                            *it = spot;
                            selectedEditorObject = {SelectedEditorObjectType::SpotLight, it->id};
                            SceneManager::Instance().MarkDirty();
                        }
                    }
                }
                if (commands.selectedMeshEntityChanged)
                {
                    auto it = std::find_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                        [&](const MeshSceneEntity& mesh) { return mesh.id == commands.selectedMeshEntity.id; });
                    if (it != editorMeshEntities.end())
                    {
                        const bool transformChanged =
                            !std::equal(std::begin(it->position), std::end(it->position), std::begin(commands.selectedMeshEntity.position)) ||
                            !std::equal(std::begin(it->rotation), std::end(it->rotation), std::begin(commands.selectedMeshEntity.rotation)) ||
                            !std::equal(std::begin(it->scale), std::end(it->scale), std::begin(commands.selectedMeshEntity.scale));
                        const bool meshAssetChanged =
                            it->meshAssetId != commands.selectedMeshEntity.meshAssetId ||
                            it->meshAssetPath != commands.selectedMeshEntity.meshAssetPath ||
                            it->skinned != commands.selectedMeshEntity.skinned;
                        ApplyMeshRendererEditorState(*it, commands.selectedMeshEntity);
                        if (transformChanged || meshAssetChanged)
                            syncStaticMeshSpatialEntity(*it);
                        selectedEditorObject = {SelectedEditorObjectType::MeshEntity, it->id};
                        SceneManager::Instance().MarkDirty();
                    }
                }
                if (commands.lodQualityCommitRequested)
                {
                    MeshSceneEntity* mesh = findMeshEntityById(commands.lodQualityCommitEntityId);
                    if (mesh && mesh->lod.enabled)
                    {
                        LodConfig qualityConfig = commands.lodQualityCommitConfig;
                        qualityConfig.levelCount = std::clamp(qualityConfig.levelCount, 1u, LodConfig::MaxLevels);
                        qualityConfig.targetRatios[0] = 1.0f;
                        qualityConfig.distances[0] = 0.0f;
                        const std::uint64_t qualityHash = HashLodConfig(qualityConfig);
                        if (StaticMeshRenderer* renderer = getStaticMeshRenderer(resolveMeshRuntimePath(*mesh)))
                            renderer->RequestLodQualityBuild(qualityConfig, qualityHash, mesh->id);
                    }
                }
                if (commands.deleteSelectedLight)
                {
                    if (selectedEditorObject.type == SelectedEditorObjectType::PointLight)
                    {
                        editorPointLights.erase(std::remove_if(editorPointLights.begin(), editorPointLights.end(),
                            [&](const PointLight& light) { return light.id == selectedEditorObject.id; }), editorPointLights.end());
                        runtimeSession->SetEditorStatus("Deleted point light #" + std::to_string(selectedEditorObject.id));
                        selectedEditorObject = {};
                        SceneManager::Instance().MarkDirty();
                    }
                    else if (selectedEditorObject.type == SelectedEditorObjectType::SpotLight)
                    {
                        editorSpotLights.erase(std::remove_if(editorSpotLights.begin(), editorSpotLights.end(),
                            [&](const SpotLight& light) { return light.id == selectedEditorObject.id; }), editorSpotLights.end());
                        runtimeSession->SetEditorStatus("Deleted spot light #" + std::to_string(selectedEditorObject.id));
                        selectedEditorObject = {};
                        SceneManager::Instance().MarkDirty();
                    }
                }
                if (commands.deleteSelectedMeshEntity &&
                    selectedEditorObject.type == SelectedEditorObjectType::MeshEntity)
                {
                    removeStaticMeshSpatialEntity(selectedEditorObject.id);
                    editorMeshEntities.erase(std::remove_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                        [&](const MeshSceneEntity& mesh) { return mesh.id == selectedEditorObject.id; }), editorMeshEntities.end());
                    rebuildMeshEntityLookup();
                    runtimeSession->SetEditorStatus("Mesh entity deleted: id=" + std::to_string(selectedEditorObject.id));
                    selectedEditorObject = {};
                    SceneManager::Instance().MarkDirty();
                }
                if (commands.selectedWaterBodyChanged)
                {
                    auto it = std::find_if(editorWaterBodies.begin(), editorWaterBodies.end(),
                        [&](const WaterBody& body) { return body.id == commands.selectedWaterBody.id; });
                    if (it != editorWaterBodies.end())
                    {
                        ApplyWaterBodyEditorStateToBody(*it, commands.selectedWaterBody);
                        selectedEditorObject = {SelectedEditorObjectType::WaterBody, it->id};
                        editorWaterBodiesDirty = true;
                        SceneManager::Instance().MarkDirty();
                    }
                }
                if (commands.openSelectedWaterMaterialEditor)
                {
                    const std::string materialId = commands.selectedWaterBody.materialId.empty()
                        ? std::string("watermat_Default_Water")
                        : commands.selectedWaterBody.materialId;
                    if (editorImGui.OpenWaterMaterialEditor(materialId))
                        runtimeSession->SetEditorStatus("Editing water material: " + materialId);
                }
                if (commands.waterMaterialDeleted)
                {
                    for (WaterBody& body : editorWaterBodies)
                    {
                        if (body.materialId == commands.deletedWaterMaterialId)
                        {
                            body.materialId = "watermat_Default_Water";
                            editorWaterBodiesDirty = true;
                            SceneManager::Instance().MarkDirty();
                        }
                    }
                    if (selectedEditorObject.type == SelectedEditorObjectType::WaterBody)
                    {
                        auto selectedIt = std::find_if(editorWaterBodies.begin(), editorWaterBodies.end(),
                            [&](const WaterBody& body) { return body.id == selectedEditorObject.id; });
                        if (selectedIt != editorWaterBodies.end() &&
                            selectedIt->materialId == "watermat_Default_Water")
                        {
                            runtimeSession->SetEditorStatus("Deleted material replaced with default on selected water body");
                        }
                    }
                }
                if (commands.deleteSelectedWaterBody &&
                    selectedEditorObject.type == SelectedEditorObjectType::WaterBody)
                {
                    editorWaterBodies.erase(std::remove_if(editorWaterBodies.begin(), editorWaterBodies.end(),
                        [&](const WaterBody& body) { return body.id == selectedEditorObject.id; }), editorWaterBodies.end());
                    runtimeSession->SetEditorStatus("Water body deleted: id=" + std::to_string(selectedEditorObject.id));
                    selectedEditorObject = {};
                    editorWaterBodiesDirty = true;
                    SceneManager::Instance().MarkDirty();
                }

                logStaticMeshSpatialMutations();

                DynamicLightEditorState dynamicLightState{};
                dynamicLightState.pointCount = static_cast<std::uint32_t>(std::min<std::size_t>(editorPointLights.size(), kMaxDynamicPointLights));
                dynamicLightState.spotCount = static_cast<std::uint32_t>(std::min<std::size_t>(editorSpotLights.size(), kMaxDynamicSpotLights));
                if (selectedEditorObject.type == SelectedEditorObjectType::PointLight)
                {
                    auto it = std::find_if(editorPointLights.begin(), editorPointLights.end(),
                        [&](const PointLight& light) { return light.id == selectedEditorObject.id; });
                    if (it != editorPointLights.end())
                    {
                        dynamicLightState.type = DynamicLightType::Point;
                        dynamicLightState.id = it->id;
                        dynamicLightState.point = *it;
                    }
                }
                else if (selectedEditorObject.type == SelectedEditorObjectType::SpotLight)
                {
                    auto it = std::find_if(editorSpotLights.begin(), editorSpotLights.end(),
                        [&](const SpotLight& light) { return light.id == selectedEditorObject.id; });
                    if (it != editorSpotLights.end())
                    {
                        dynamicLightState.type = DynamicLightType::Spot;
                        dynamicLightState.id = it->id;
                        dynamicLightState.spot = *it;
                    }
                }
                editorImGui.SetDynamicLightEditorState(dynamicLightState);
                WaterBodyEditorState waterBodyState = BuildWaterBodyEditorState(editorWaterBodies,
                    selectedEditorObject.type == SelectedEditorObjectType::WaterBody ? selectedEditorObject.id : 0u);
                runtimeSession->SetWaterBodyEditorState(waterBodyState);
                editorImGui.SetWaterBodyEditorState(waterBodyState);
                MeshRendererEditorState meshRendererState = BuildMeshRendererEditorState(editorMeshEntities,
                    selectedEditorObject.type == SelectedEditorObjectType::MeshEntity ? selectedEditorObject.id : 0u);
                if (meshRendererState.selected)
                {
                    if (auto entry = resolveModelAsset(meshRendererState.meshAssetId))
                        meshRendererState.meshDisplayName = entry->displayName.empty() ? entry->filename : entry->displayName;
                    auto meshIt = std::find_if(editorMeshEntities.begin(), editorMeshEntities.end(),
                        [&](const MeshSceneEntity& mesh) { return mesh.id == meshRendererState.id; });
                    if (meshIt != editorMeshEntities.end())
                    {
                        const std::string runtimePath = resolveMeshRuntimePath(*meshIt);
                        if (StaticMeshRenderer* renderer = getStaticMeshRenderer(runtimePath))
                            meshRendererState.materialSlotCount = std::max<std::uint32_t>(1u, renderer->MaterialSlotCount());
                    }
                    meshRendererState.selectedMaterialSlot = std::min(meshRendererState.selectedMaterialSlot,
                        meshRendererState.materialSlotCount > 0 ? meshRendererState.materialSlotCount - 1u : 0u);
                }
                editorImGui.SetMeshRendererEditorState(meshRendererState);
                TerrainEditorState terrainState{};
                if (terrainOk && terrain.HasTerrain())
                {
                    const TerrainSceneData terrainData = terrain.GetTerrainSceneData();
                    terrainState.exists = true;
                    terrainState.selected = selectedEditorObject.type == SelectedEditorObjectType::Terrain;
                    terrainState.name = EditorDisplayName(terrainData);
                    terrainState.widthMeters = terrainData.widthMeters;
                    terrainState.depthMeters = terrainData.depthMeters;
                    terrainState.cellSizeMeters = terrainData.cellSizeMeters;
                    terrainState.cellsX = terrainData.cellsX;
                    terrainState.cellsZ = terrainData.cellsZ;
                    terrainState.triplanarEnabled = terrainData.triplanarEnabled;
                    terrainState.triplanarSharpness = terrainData.triplanarSharpness;
                    terrainState.triplanarSlopeThreshold = terrainData.triplanarSlopeThreshold;
                    terrainState.triplanarSlopeTransition = terrainData.triplanarSlopeTransition;
                }
                editorImGui.SetTerrainEditorState(terrainState);
                auto hierarchyState = buildHierarchyEntities();
                editorImGui.SetHierarchySceneState(
                    static_cast<std::uint64_t>(editorSceneRootEntity),
                    std::move(hierarchyState.first),
                    std::move(hierarchyState.second));

                LightingState lightingState = editorImGui.GetLightingState();
                const bool editorHideEntities = editorPlay.state.mode == EditorPlayMode::Edit;
                lightingState.numPointLights = 0;
                for (const PointLight& light : editorPointLights)
                {
                    if (editorHideEntities && light.editorHidden)
                        continue;
                    if (lightingState.numPointLights >= kMaxDynamicPointLights)
                        break;
                    lightingState.pointLights[lightingState.numPointLights++] = light;
                }
                lightingState.numSpotLights = 0;
                for (const SpotLight& light : editorSpotLights)
                {
                    if (editorHideEntities && light.editorHidden)
                        continue;
                    if (lightingState.numSpotLights >= kMaxDynamicSpotLights)
                        break;
                    lightingState.spotLights[lightingState.numSpotLights++] = light;
                }
                editorImGui.SetLightingState(lightingState);
                terrain.SetLightingState(lightingState);
                std::unordered_map<std::string, std::uint32_t> waterMaterialUsageMap;
                for (const WaterBody& body : editorWaterBodies)
                {
                    if (!body.materialId.empty())
                        ++waterMaterialUsageMap[body.materialId];
                }
                std::vector<std::pair<std::string, std::uint32_t>> waterMaterialUsageCounts;
                waterMaterialUsageCounts.reserve(waterMaterialUsageMap.size());
                for (const auto& usage : waterMaterialUsageMap)
                    waterMaterialUsageCounts.push_back(usage);
                editorImGui.SetWaterMaterialUsageCounts(std::move(waterMaterialUsageCounts));

                const auto waterMaterials = editorImGui.GetWaterMaterialsSnapshot();
                editorImGui.SetWaterMaterials(waterMaterials);
                terrain.SetWaterMaterials(waterMaterials);
                if (editorWaterBodiesDirty)
                {
                    std::vector<WaterBody> terrainWaterBodies = editorWaterBodies;
                    const bool hasHiddenWaterBody = editorHideEntities && std::any_of(terrainWaterBodies.begin(),
                        terrainWaterBodies.end(),
                        [](const WaterBody& body) { return body.editorHidden; });
                    if (hasHiddenWaterBody)
                    {
                        terrainWaterBodies.erase(std::remove_if(terrainWaterBodies.begin(),
                            terrainWaterBodies.end(),
                            [](const WaterBody& body) { return body.editorHidden; }), terrainWaterBodies.end());
                        terrain.SetWaterBodies(device, terrainWaterBodies);
                    }
                    else
                    {
                        terrain.SetWaterBodies(device, terrainWaterBodies);
                        editorWaterBodies = terrain.GetWaterBodies();
                    }
                    if (waterSculptMeshRegenPending &&
                        selectedEditorObject.type == SelectedEditorObjectType::WaterBody)
                    {
                        auto bodyIt = std::find_if(editorWaterBodies.begin(), editorWaterBodies.end(),
                            [&](const WaterBody& body) { return body.id == selectedEditorObject.id; });
                        if (bodyIt != editorWaterBodies.end())
                        {
                            const std::uint32_t activeCells = static_cast<std::uint32_t>(
                                std::count_if(bodyIt->shapeMask.begin(), bodyIt->shapeMask.end(),
                                    [](std::uint8_t value) { return value != 0; }));
                            Tracenf("[WATER-OBJ-5] Mesh regenerated: body_id=%u cells_active=%u mask=%ux%u",
                                bodyIt->id,
                                activeCells,
                                bodyIt->maskWidth,
                                bodyIt->maskHeight);
                        }
                    }
                    waterSculptMeshRegenPending = false;
                    editorWaterBodiesDirty = false;
                }
                terrain.SetSelectedWaterBodyHighlight(device, 0u);
                if (skinnedMeshOk)
                {
                    skinnedMesh.SetLightingState(lightingState);
                }
                if (commands.paletteSlotChanged)
                {
                    syncTerrainAssetRoots();
                    if (!terrain.ApplyPaletteSlotChange(device, commands.paletteSlotData))
                    {
                        Tracenf("[MAIN] failed to apply terrain palette slot %u", commands.paletteSlot);
                    }
                    else
                    {
                        editorImGui.SetPaletteSlots(terrain.GetPaletteSlots());
                        SceneManager::Instance().MarkDirty();
                    }
                }
                if (commands.paletteSlotParamsChanged)
                {
                    if (!terrain.ApplyPaletteSlotParams(commands.paletteSlotData))
                    {
                        Tracenf("[MAIN] failed to apply terrain material params for layer %u", commands.paletteSlot);
                    }
                    else
                    {
                        editorImGui.SetPaletteSlots(terrain.GetPaletteSlots());
                        SceneManager::Instance().MarkDirty();
                    }
                }
                if (commands.terrainTriplanarChanged)
                {
                    if (!terrain.SetTriplanarSettings(commands.terrainTriplanarEnabled,
                            commands.terrainTriplanarSharpness,
                            commands.terrainTriplanarSlopeThreshold,
                            commands.terrainTriplanarSlopeTransition))
                    {
                        Tracen("[MAIN] failed to apply terrain triplanar settings");
                    }
                    else
                    {
                        SceneManager::Instance().MarkDirty();
                    }
                }
                if (commands.save)
                {
                    terrain.RequestEditorSave();
                    SceneManager::Instance().MarkDirty();
                }
                if (commands.reload)
                {
                    terrain.RequestEditorReload();
                    selectedEditorObject = {};
                }
                if (commands.undo)
                    terrain.RequestEditorUndo();
                terrain.UpdateEditor(device, deltaSeconds, frameCamera, renderSize.width, renderSize.height);
                if (commands.reload)
                {
                    editorWaterBodies = terrain.GetWaterBodies();
                    editorWaterBodiesDirty = false;
                    for (const WaterBody& body : editorWaterBodies)
                        nextEditorWaterBodyId = std::max(nextEditorWaterBodyId, body.id + 1u);
                }
                SceneManager::Instance().SetCurrentSceneSnapshot(buildSceneSnapshot());
            }
#endif
        }

        device.BeginFrame();
        if (device.IsFrameActive())
        {
            const uint64_t frameNumber = device.GetFrameNumber();
            bool frameRmlUiRenderCalled = false;
            bool frameImGuiRenderCalled = false;
            bool frameSceneRenderCalled = false;
            size_t frameSceneEntityCount = 0;
            size_t frameStaticMeshEntityCount = 0;
            size_t frameStaticMeshSubmitted = 0;
            size_t frameStaticMeshDrawCalls = 0;
            size_t frameStaticMeshTriangles = 0;
            size_t frameStaticMeshUniformUpdates = 0;
            size_t frameStaticMeshOverrideActiveDraws = 0;
            size_t frameStaticMeshFrustumCulled = 0;
            SpatialIndex::QueryStats frameStaticMeshSpatialStats{};
            size_t frameStaticMeshBatches = 0;
            size_t frameStaticMeshMaxBatchSize = 0;
            size_t frameStaticMeshInstanceBufferBytes = 0;
            bool frameStaticMeshInstanceBufferRebuilt = false;
#if defined(IXTREEME_WITH_EDITOR)
            editorImGui.SetEditorPlayModeState(editorPlay.state);
            editorImGui.BeginFrame(runtimeSession->IsMapEditorOpen());
#endif
            const bool isInWorld = runtimeSession->IsInWorld();
            const bool hasSceneTerrain = terrainOk && terrain.HasTerrain();
            std::vector<WorldRenderEntity> entities;
            WorldCamera camera{};
            if (isInWorld)
            {
                entities = frameEntities;
                frameSceneEntityCount = entities.size();
                if (runtimeSession->IsMapEditorOpen())
                    frameSceneEntityCount += editorMeshEntities.size();
                camera = hasFrameCamera ? frameCamera : cameraController.BuildCamera(renderSize.width, renderSize.height);

                if (skinnedMeshOk)
                {
                    uint32_t skinSlot = 0;
                    for (const auto& entity : entities)
                    {
                        if (skinSlot >= SkinnedMeshRenderer::MaxSkinSlots())
                            break;
                        skinnedMesh.SkinInstance(device,
                            skinSlot,
                            ToSkinnedMeshMotion(entity.moveState),
                            static_cast<float>(seconds));
                        ++skinSlot;
                    }
                    if (runtimeSession->IsMapEditorOpen())
                    {
                        const size_t editorVisualRenderCount =
                            editorPointLights.size() + editorSpotLights.size() + editorMeshEntities.size();
                        for (size_t visualIndex = 0; visualIndex < editorVisualRenderCount; ++visualIndex)
                        {
                            if (skinSlot >= SkinnedMeshRenderer::MaxSkinSlots())
                                break;
                            skinnedMesh.SkinInstance(device,
                                skinSlot,
                                SkinnedMeshRenderer::MotionState::Idle,
                                static_cast<float>(seconds));
                            ++skinSlot;
                        }
                    }
                }
            }
            else if (runtimeSession->IsLobbyActive() && skinnedMeshOk)
            {
                skinnedMesh.Skin(device, seconds);
            }

            if (isInWorld && hasSceneTerrain && hasFrameCamera)
                terrain.RenderSunShadowMap(device, frameCamera);
            if (isInWorld && hasSceneTerrain && hasFrameCamera)
            {
                terrain.RenderWaterReflection(device,
                    frameCamera,
                    seconds,
                    [&](const WorldCamera& mirrorCamera,
                        VkExtent2D reflectionExtent,
                        VkRenderPass reflectionRenderPass,
                        float waterLevelY)
                    {
                        if (!skinnedMeshOk)
                            return;

                        uint32_t skinSlot = 0;
                        for (const auto& entity : entities)
                        {
                            if (skinSlot >= SkinnedMeshRenderer::MaxSkinSlots())
                                break;
                            auto position = ServerMetersToDisplay(entity.position);
                            position.y += skinnedMesh.GroundOffsetY();
                            const std::array<float, 4> tint = entity.visualClassId == 0
                                ? std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f}
                                : (entity.visualClassId == 1
                                      ? std::array<float, 4>{1.35f, 0.55f, 0.55f, 1.0f}
                                      : std::array<float, 4>{0.65f, 0.95f, 1.35f, 1.0f});
                            skinnedMesh.RenderInWorldReflection(device,
                                mirrorCamera,
                                reflectionExtent,
                                reflectionRenderPass,
                                waterLevelY,
                                position,
                                HeadingFromQuantized(entity.heading),
                                skinSlot,
                                tint);
                            ++skinSlot;
                        }

                        if (runtimeSession->IsMapEditorOpen())
                        {
                            for (const auto& light : editorPointLights)
                            {
                                if (editorPlay.state.mode == EditorPlayMode::Edit && light.editorHidden)
                                    continue;
                                if (skinSlot >= SkinnedMeshRenderer::MaxSkinSlots())
                                    break;
                                WorldVec3 position{light.position[0], light.position[1] + skinnedMesh.GroundOffsetY(), light.position[2]};
                                const bool selected =
                                    selectedEditorObject.type == SelectedEditorObjectType::PointLight &&
                                    selectedEditorObject.id == light.id;
                                skinnedMesh.RenderInWorldReflection(device,
                                    mirrorCamera,
                                    reflectionExtent,
                                    reflectionRenderPass,
                                    waterLevelY,
                                    position,
                                    0.0f,
                                    skinSlot,
                                    selected
                                        ? std::array<float, 4>{2.0f, 1.55f, 0.25f, 1.0f}
                                        : std::array<float, 4>{1.6f, 1.05f, 0.35f, 1.0f});
                                ++skinSlot;
                            }
                            for (const auto& light : editorSpotLights)
                            {
                                if (editorPlay.state.mode == EditorPlayMode::Edit && light.editorHidden)
                                    continue;
                                if (skinSlot >= SkinnedMeshRenderer::MaxSkinSlots())
                                    break;
                                WorldVec3 position{light.position[0], light.position[1] + skinnedMesh.GroundOffsetY(), light.position[2]};
                                const bool selected =
                                    selectedEditorObject.type == SelectedEditorObjectType::SpotLight &&
                                    selectedEditorObject.id == light.id;
                                skinnedMesh.RenderInWorldReflection(device,
                                    mirrorCamera,
                                    reflectionExtent,
                                    reflectionRenderPass,
                                    waterLevelY,
                                    position,
                                    light.rotation[1],
                                    skinSlot,
                                    selected
                                        ? std::array<float, 4>{0.35f, 1.7f, 2.0f, 1.0f}
                                        : std::array<float, 4>{0.35f, 1.25f, 1.65f, 1.0f});
                                ++skinSlot;
                            }
                        }
                    });
            }

            const bool useOffscreenScene = offscreenSceneOk && device.IsFrameActive();
            if (useOffscreenScene)
                offscreenScene.BeginMainPass(device);
            else
                device.BeginSwapchainRenderPass("direct");

            std::vector<WorldLabelRenderer::Label> plates;
            if (isInWorld)
            {
                if (hasSceneTerrain)
                {
                    frameSceneRenderCalled = true;
                    terrain.Render(device, camera);
                }

                plates.reserve(entities.size());
                uint32_t skinSlot = 0;
                static bool loggedTerrainAlignment = false;
                for (const auto& entity : entities)
                {
                    auto position = ServerMetersToDisplay(entity.position);
                    const float terrainY = hasSceneTerrain ? terrain.SampleHeight(position) : position.y;
                    const float groundOffsetY = skinnedMeshOk ? skinnedMesh.GroundOffsetY() : 0.0f;
                    if (!loggedTerrainAlignment && hasSceneTerrain)
                    {
                        Tracenf("[WORLD] terrain align: net_id=%u server=(%.3f,%.3f,%.3f) displayY=%.3f terrainY=%.3f delta=%.3f modelGroundOffset=%.3f",
                            entity.netId,
                            entity.position.x,
                            entity.position.y,
                            entity.position.z,
                            position.y,
                            terrainY,
                            position.y - terrainY,
                            groundOffsetY);
                        loggedTerrainAlignment = true;
                    }
                    if (skinnedMeshOk && skinSlot < SkinnedMeshRenderer::MaxSkinSlots())
                    {
                        position.y += groundOffsetY;
                        const std::array<float, 4> tint = entity.visualClassId == 0
                            ? std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f}
                            : (entity.visualClassId == 1
                                  ? std::array<float, 4>{1.35f, 0.55f, 0.55f, 1.0f}
                                  : std::array<float, 4>{0.65f, 0.95f, 1.35f, 1.0f});
                        skinnedMesh.RenderInWorld(device,
                            seconds,
                            camera,
                            position,
                            HeadingFromQuantized(entity.heading),
                            skinSlot,
                            tint);
                    }
                    plates.push_back(WorldLabelRenderer::Label{
                        WorldAdd(position, {0.0f, 2.2f, 0.0f}),
                        entity.name,
                        entity.netId == selectedTargetNetId
                            ? std::array<float, 4>{1.0f, 0.86f, 0.32f, 1.0f}
                            : std::array<float, 4>{0.92f, 0.96f, 1.0f, 1.0f},
                        entity.netId == selectedTargetNetId});
                    ++skinSlot;
                }
                if (skinnedMeshOk && runtimeSession->IsMapEditorOpen())
                {
                    for (const auto& light : editorPointLights)
                    {
                        if (editorPlay.state.mode == EditorPlayMode::Edit && light.editorHidden)
                            continue;
                        if (skinSlot >= SkinnedMeshRenderer::MaxSkinSlots())
                            break;
                        WorldVec3 position{light.position[0], light.position[1] + skinnedMesh.GroundOffsetY(), light.position[2]};
                        const bool selected =
                            selectedEditorObject.type == SelectedEditorObjectType::PointLight &&
                            selectedEditorObject.id == light.id;
                        skinnedMesh.RenderInWorld(device,
                            seconds,
                            camera,
                            position,
                            0.0f,
                            skinSlot,
                            selected
                                ? std::array<float, 4>{2.0f, 1.55f, 0.25f, 1.0f}
                                : std::array<float, 4>{1.6f, 1.05f, 0.35f, 1.0f});
                        ++skinSlot;
                    }
                    for (const auto& light : editorSpotLights)
                    {
                        if (editorPlay.state.mode == EditorPlayMode::Edit && light.editorHidden)
                            continue;
                        if (skinSlot >= SkinnedMeshRenderer::MaxSkinSlots())
                            break;
                        WorldVec3 position{light.position[0], light.position[1] + skinnedMesh.GroundOffsetY(), light.position[2]};
                        const bool selected =
                            selectedEditorObject.type == SelectedEditorObjectType::SpotLight &&
                            selectedEditorObject.id == light.id;
                        skinnedMesh.RenderInWorld(device,
                            seconds,
                            camera,
                            position,
                            light.rotation[1],
                            skinSlot,
                            selected
                                ? std::array<float, 4>{0.35f, 1.7f, 2.0f, 1.0f}
                                : std::array<float, 4>{0.35f, 1.25f, 1.65f, 1.0f});
                        ++skinSlot;
                    }
                }
                if (runtimeSession->IsMapEditorOpen())
                {
                    std::unordered_map<StaticMeshLodBatchKey, StaticMeshLodBatch, StaticMeshLodBatchKeyHash> staticMeshBatches;
                    std::array<std::uint32_t, LodConfig::MaxLevels> frameLodSelection{};
                    std::uint32_t frameLodActiveInstances = 0;
                    std::uint32_t frameNonLodEntities = 0;
                    const std::vector<std::uint32_t> spatialCandidates =
                        staticMeshSpatialIndex.QueryFrustum(SpatialFrustumFromCamera(camera), &frameStaticMeshSpatialStats);
                    frameStaticMeshEntityCount = editorMeshEntities.size();
                    frameStaticMeshFrustumCulled =
                        frameStaticMeshSpatialStats.totalObjects > frameStaticMeshSpatialStats.candidates
                            ? static_cast<std::size_t>(frameStaticMeshSpatialStats.totalObjects - frameStaticMeshSpatialStats.candidates)
                            : 0u;
                    auto logLodDisposition = [&](const StaticMeshLodBatch::LodDispositionRecord& record) {
                        const char* disposition = "DRAWN";
                        if (record.fullResFallback && record.submitted)
                            disposition = "DRAWN";
                        else if (record.selectedLevel >= record.levelCount)
                            disposition = "LEVEL_OUT_OF_RANGE";
                        else if (!record.bufferValid)
                            disposition = "INVALID_BUFFER";
                        else if (record.selectedIndices == 0)
                            disposition = "EMPTY_BUFFER";
                        else if (record.culled)
                            disposition = "CULLED";
                        else if (!record.submitted)
                            disposition = "NOT_SUBMITTED";

                        LodDispositionState& state = staticMeshLodDispositionStates[record.entityId];
                        const bool changed =
                            state.selectedLevel != record.selectedLevel ||
                            state.bufferValid != record.bufferValid ||
                            state.culled != record.culled ||
                            state.submitted != record.submitted ||
                            state.fullResFallback != record.fullResFallback ||
                            state.disposition != disposition;
                        const bool heartbeat = (frameNumber % 60u) == 0u;
                        if (!changed && !heartbeat)
                            return;

                        if (LodLogsEnabled())
                        {
                            Tracenf("[LOD-DISP] entity=%u dist=%.3f prevLevel=%u -> level=%u levelCount=%u cfg.distances=[%.2f,%.2f,%.2f,%.2f] cfg.ratios=[%.5f,%.5f,%.5f,%.5f] override=%u bufferValid=%s bufferSource=%s selVerts=%zu selIndices=%zu bbox.min=(%.3f,%.3f,%.3f) bbox.max=(%.3f,%.3f,%.3f) bboxSource=%s culled=%s cullReason=%s submitted=%s drawIndexCount=%u disposition=%s",
                                record.entityId,
                                record.distance,
                                record.previousLevel,
                                record.selectedLevel,
                                record.levelCount,
                                record.configSnapshot.distances[0],
                                record.configSnapshot.distances[1],
                                record.configSnapshot.distances[2],
                                record.configSnapshot.distances[3],
                                record.configSnapshot.targetRatios[0],
                                record.configSnapshot.targetRatios[1],
                                record.configSnapshot.targetRatios[2],
                                record.configSnapshot.targetRatios[3],
                                record.overrideEnabled ? 1u : 0u,
                                record.bufferValid ? "y" : "n",
                                record.bufferSource,
                                record.selectedVertices,
                                record.selectedIndices,
                                record.bounds.min.x, record.bounds.min.y, record.bounds.min.z,
                                record.bounds.max.x, record.bounds.max.y, record.bounds.max.z,
                                record.bboxSource,
                                record.culled ? "y" : "n",
                                record.cullReason,
                                record.submitted ? "y" : "n",
                                record.drawIndexCount,
                                disposition);
                        }

                        state.selectedLevel = record.selectedLevel;
                        state.bufferValid = record.bufferValid;
                        state.culled = record.culled;
                        state.submitted = record.submitted;
                        state.fullResFallback = record.fullResFallback;
                        state.disposition = disposition;
                    };
                    auto lodBufferSourceForDiag = [](const StaticMeshRenderer::LodDiagnostics& diag) -> const char* {
                        if (diag.pendingUpload)
                            return "pending";
                        if (std::strcmp(diag.source, "cache") == 0)
                            return "cache";
                        if (std::strcmp(diag.source, "preview") == 0 ||
                            std::strcmp(diag.source, "commit") == 0 ||
                            std::strcmp(diag.source, "fullres") == 0)
                        {
                            return "generated";
                        }
                        return "none";
                    };
                    std::unordered_set<std::uint32_t> currentCulledMeshLogSet;
                    for (std::uint32_t meshId : spatialCandidates)
                    {
                        MeshSceneEntity* meshPtr = findMeshEntityById(meshId);
                        if (!meshPtr)
                            continue;
                        const MeshSceneEntity& mesh = *meshPtr;
                        if (editorPlay.state.mode == EditorPlayMode::Edit && mesh.editorHidden)
                            continue;
                        const bool selected =
                            selectedEditorObject.type == SelectedEditorObjectType::MeshEntity &&
                            selectedEditorObject.id == mesh.id;
                        const std::string runtimePath = resolveMeshRuntimePath(mesh);
                        if (StaticMeshRenderer* renderer = getStaticMeshRenderer(runtimePath))
                        {
                            const SpatialIndex::Aabb worldBounds = StaticMeshWorldAabb(mesh, *renderer);
                            if (renderer->IsLoaded() &&
                                WorldAabbOutsideCameraFrustum(camera, SpatialAabbCorners(worldBounds)))
                            {
                                ++frameStaticMeshFrustumCulled;
                                currentCulledMeshLogSet.insert(mesh.id);
                                if (mesh.lod.enabled)
                                {
                                    LodConfig cullLodConfig{};
                                    const std::optional<LodConfig> assetDefault = editorImGui.FindModelLodDefault(mesh.meshAssetId);
                                    cullLodConfig = (!mesh.lod.overrideAssetDefault && assetDefault)
                                        ? *assetDefault
                                        : mesh.lod.config;
                                    cullLodConfig.levelCount = std::clamp(cullLodConfig.levelCount, 1u, LodConfig::MaxLevels);
                                    cullLodConfig.targetRatios[0] = 1.0f;
                                    cullLodConfig.distances[0] = 0.0f;
                                    const std::uint64_t cullConfigHash = HashLodConfig(cullLodConfig);
                                    const float cullDistance = DistanceToAabb(camera.eye, worldBounds);
                                    const std::uint32_t previousLevel = staticMeshSelectedLods.count(mesh.id) > 0
                                        ? staticMeshSelectedLods[mesh.id]
                                        : 0u;
                                    const std::uint32_t cullLevel = SelectLodLevel(cullLodConfig, cullDistance, previousLevel);
                                    const StaticMeshRenderer::LodDiagnostics lodDiag =
                                        renderer->GetLodDiagnostics(cullLevel == 0 ? 0 : cullConfigHash);
                                    StaticMeshLodBatch::LodDispositionRecord record{};
                                    record.entityId = mesh.id;
                                    record.distance = cullDistance;
                                    record.previousLevel = previousLevel;
                                    record.selectedLevel = cullLevel;
                                    record.levelCount = lodDiag.levelCount;
                                    record.configSnapshot = cullLodConfig;
                                    record.overrideEnabled = mesh.lod.overrideAssetDefault;
                                    record.bufferValid = cullLevel < lodDiag.levelCount && lodDiag.bufferValid;
                                    record.bufferSource = lodBufferSourceForDiag(lodDiag);
                                    record.selectedVertices = lodDiag.vertexCount;
                                    record.selectedIndices = cullLevel < lodDiag.levelIndices.size() ? lodDiag.levelIndices[cullLevel] : 0u;
                                    record.bounds = worldBounds;
                                    record.bboxSource =
                                        (worldBounds.min.x == worldBounds.max.x ||
                                            worldBounds.min.y == worldBounds.max.y ||
                                            worldBounds.min.z == worldBounds.max.z)
                                        ? "degenerate"
                                        : "entity";
                                    record.culled = true;
                                    record.cullReason = "frustum";
                                    record.submitted = false;
                                    record.drawIndexCount = 0;
                                    logLodDisposition(record);
                                }
                                const bool cullLogChanged = previousCulledMeshLogSet.find(mesh.id) == previousCulledMeshLogSet.end();
                                if ((!QuietLogsForLodDiag() && (frameNumber < 3 || (frameNumber % 60u) == 0u)) ||
                                    (QuietLogsForLodDiag() && cullLogChanged))
                                {
                                    Tracenf("[MESH-CULL] culled id=%u name=%s path=%s",
                                        mesh.id,
                                        mesh.name.c_str(),
                                        runtimePath.c_str());
                                }
                                continue;
                            }
                            renderer->SetLightingState(runtimeSession->GetLightingState());
                            StaticMeshRenderer::Instance instance{};
                            instance.entityId = mesh.id;
                            instance.position = {mesh.position[0], mesh.position[1], mesh.position[2]};
                            instance.rotation[0] = mesh.rotation[0];
                            instance.rotation[1] = mesh.rotation[1];
                            instance.rotation[2] = mesh.rotation[2];
                            instance.scale[0] = mesh.scale[0];
                            instance.scale[1] = mesh.scale[1];
                            instance.scale[2] = mesh.scale[2];
                            instance.tint = selected
                                ? std::array<float, 4>{1.25f, 1.05f, 0.45f, 1.0f}
                                : std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f};
                            instance.materialOverrides = mesh.materialOverrides;
                            LodConfig effectiveLodConfig{};
                            std::uint64_t lodConfigHash = 0;
                            std::uint32_t lodLevel = 0;
                            bool lodDispositionActive = false;
                            StaticMeshLodBatch::LodDispositionRecord lodDispositionRecord{};
                            if (mesh.lod.enabled)
                            {
                                ++frameLodActiveInstances;
                                const std::optional<LodConfig> assetDefault = editorImGui.FindModelLodDefault(mesh.meshAssetId);
                                effectiveLodConfig = (!mesh.lod.overrideAssetDefault && assetDefault)
                                    ? *assetDefault
                                    : mesh.lod.config;
                                effectiveLodConfig.levelCount = std::clamp(effectiveLodConfig.levelCount, 1u, LodConfig::MaxLevels);
                                effectiveLodConfig.targetRatios[0] = 1.0f;
                                effectiveLodConfig.distances[0] = 0.0f;
                                lodConfigHash = HashLodConfig(effectiveLodConfig);
                                const float distance = DistanceToAabb(camera.eye, worldBounds);
                                const std::uint32_t previousLevel = staticMeshSelectedLods.count(mesh.id) > 0
                                    ? staticMeshSelectedLods[mesh.id]
                                    : 0u;
                                lodLevel = SelectLodLevel(effectiveLodConfig, distance, previousLevel);
                                staticMeshSelectedLods[mesh.id] = lodLevel;
                                if (lodLevel < frameLodSelection.size())
                                    ++frameLodSelection[lodLevel];
                                const StaticMeshRenderer::LodDiagnostics lodDiag =
                                    renderer->GetLodDiagnostics(lodLevel == 0 ? 0 : lodConfigHash);
                                const std::size_t selectedTris =
                                    lodLevel < lodDiag.levelTris.size() ? lodDiag.levelTris[lodLevel] : 0u;
                                const bool selectedBufferValid =
                                    lodLevel < lodDiag.levelCount && lodDiag.bufferValid;
                                lodDispositionActive = true;
                                lodDispositionRecord.entityId = mesh.id;
                                lodDispositionRecord.distance = distance;
                                lodDispositionRecord.previousLevel = previousLevel;
                                lodDispositionRecord.selectedLevel = lodLevel;
                                lodDispositionRecord.levelCount = lodDiag.levelCount;
                                lodDispositionRecord.configSnapshot = effectiveLodConfig;
                                lodDispositionRecord.overrideEnabled = mesh.lod.overrideAssetDefault;
                                lodDispositionRecord.bufferValid = selectedBufferValid;
                                lodDispositionRecord.bufferSource = lodBufferSourceForDiag(lodDiag);
                                lodDispositionRecord.selectedVertices = lodDiag.vertexCount;
                                lodDispositionRecord.selectedIndices = lodLevel < lodDiag.levelIndices.size() ? lodDiag.levelIndices[lodLevel] : 0u;
                                lodDispositionRecord.bounds = worldBounds;
                                lodDispositionRecord.bboxSource =
                                    (worldBounds.min.x == worldBounds.max.x ||
                                        worldBounds.min.y == worldBounds.max.y ||
                                        worldBounds.min.z == worldBounds.max.z)
                                    ? "degenerate"
                                    : "entity";
                                lodDispositionRecord.culled = false;
                                lodDispositionRecord.cullReason = "none";
                                std::array<float, LodConfig::MaxLevels> cfgRatios{};
                                std::array<float, LodConfig::MaxLevels> cfgDistances{};
                                for (std::size_t i = 0; i < LodConfig::MaxLevels; ++i)
                                {
                                    cfgRatios[i] = effectiveLodConfig.targetRatios[i];
                                    cfgDistances[i] = effectiveLodConfig.distances[i];
                                }
                                LodCfgLogState& cfgLogState = lodCfgLogStates[mesh.id];
                                const bool cfgChanged =
                                    !cfgLogState.initialized ||
                                    cfgLogState.levelCount != effectiveLodConfig.levelCount ||
                                    cfgLogState.ratios != cfgRatios ||
                                    cfgLogState.distances != cfgDistances ||
                                    cfgLogState.overrideEnabled != mesh.lod.overrideAssetDefault;
                                if (LodLogsEnabled() && (!QuietLogsForLodDiag() || cfgChanged))
                                {
                                    Tracenf("[LOD-CFG] entity=%u levelCount=%u ratios=[%.5f,%.5f,%.5f,%.5f] distances=[%.2f,%.2f,%.2f,%.2f] override=%u",
                                        mesh.id,
                                        effectiveLodConfig.levelCount,
                                        effectiveLodConfig.targetRatios[0],
                                        effectiveLodConfig.targetRatios[1],
                                        effectiveLodConfig.targetRatios[2],
                                        effectiveLodConfig.targetRatios[3],
                                        effectiveLodConfig.distances[0],
                                        effectiveLodConfig.distances[1],
                                        effectiveLodConfig.distances[2],
                                        effectiveLodConfig.distances[3],
                                        mesh.lod.overrideAssetDefault ? 1u : 0u);
                                }
                                cfgLogState.levelCount = effectiveLodConfig.levelCount;
                                cfgLogState.ratios = cfgRatios;
                                cfgLogState.distances = cfgDistances;
                                cfgLogState.overrideEnabled = mesh.lod.overrideAssetDefault;
                                cfgLogState.initialized = true;

                                std::array<std::size_t, LodConfig::MaxLevels> pickLevelTris{};
                                for (std::size_t i = 0; i < LodConfig::MaxLevels; ++i)
                                    pickLevelTris[i] = lodDiag.levelTris[i];
                                LodPickLogState& pickLogState = lodPickLogStates[mesh.id];
                                const bool pickCritical =
                                    lodLevel > 0 &&
                                    (lodLevel >= lodDiag.levelCount ||
                                        !selectedBufferValid ||
                                        selectedTris == 0);
                                const bool pickChanged =
                                    !pickLogState.initialized ||
                                    pickLogState.selectedLevel != lodLevel ||
                                    pickLogState.levelCount != lodDiag.levelCount ||
                                    pickLogState.levelTris != pickLevelTris ||
                                    pickLogState.selectedTris != selectedTris ||
                                    pickLogState.selectedBufferValid != selectedBufferValid ||
                                    pickLogState.source != lodDiag.source;
                                if (LodLogsEnabled() && (!QuietLogsForLodDiag() || pickChanged || pickCritical))
                                {
                                    Tracenf("[LOD-PICK] entity=%u dist=%.3f selectedLevel=%u levelCount=%u levelTris=[%zu,%zu,%zu,%zu] selectedTris=%zu selectedBufferValid=%s source=%s",
                                        mesh.id,
                                        distance,
                                        lodLevel,
                                        lodDiag.levelCount,
                                        lodDiag.levelTris[0],
                                        lodDiag.levelTris[1],
                                        lodDiag.levelTris[2],
                                        lodDiag.levelTris[3],
                                        selectedTris,
                                        selectedBufferValid ? "y" : "n",
                                        lodDiag.source);
                                }
                                pickLogState.selectedLevel = lodLevel;
                                pickLogState.levelCount = lodDiag.levelCount;
                                pickLogState.levelTris = pickLevelTris;
                                pickLogState.selectedTris = selectedTris;
                                pickLogState.selectedBufferValid = selectedBufferValid;
                                pickLogState.source = lodDiag.source;
                                pickLogState.initialized = true;
                                if (lodLevel > 0)
                                {
                                    const char* emptyReason = nullptr;
                                    if (!lodDiag.bufferKnown || !lodDiag.bufferValid)
                                        emptyReason = "buffer-null";
                                    else if (lodLevel >= lodDiag.levelCount)
                                        emptyReason = "level-out-of-range";
                                    else if (selectedTris == 0)
                                        emptyReason = std::strcmp(lodDiag.source, "preview") == 0 ? "preview-empty" : "zero-tris";
                                    if (emptyReason)
                                    {
                                        const char* fallbackReason = "buffer-not-ready";
                                        if (std::strcmp(emptyReason, "level-out-of-range") == 0)
                                            fallbackReason = "no-levels";
                                        else if (std::strcmp(emptyReason, "zero-tris") == 0 ||
                                            std::strcmp(emptyReason, "preview-empty") == 0)
                                            fallbackReason = "zero-tris";
                                        if (LodLogsEnabled())
                                        {
                                            Tracenf("[LOD-PICK] EMPTY entity=%u reason=%s", mesh.id, emptyReason);
                                            Tracenf("[LOD-PICK] FALLBACK entity=%u reason=%s drawing=full-res",
                                                mesh.id,
                                                fallbackReason);
                                        }
                                    }
                                }
                            }
                            else
                            {
                                ++frameNonLodEntities;
                                staticMeshSelectedLods.erase(mesh.id);
                            }
                            StaticMeshLodBatchKey key{renderer, lodConfigHash, lodLevel};
                            StaticMeshLodBatch& batch = staticMeshBatches[key];
                            batch.config = effectiveLodConfig;
                            batch.instances.push_back(std::move(instance));
                            if (lodDispositionActive)
                                batch.lodDispositionRecords.push_back(lodDispositionRecord);
                        }
                    }
                    for (auto& [key, batch] : staticMeshBatches)
                    {
                        StaticMeshRenderer* renderer = key.renderer;
                        std::vector<StaticMeshRenderer::Instance>& instances = batch.instances;
                        if (!renderer || instances.empty())
                            continue;
                        if (key.configHash != 0)
                            renderer->RenderLodBatchInWorld(device, seconds, camera, instances, batch.config, key.configHash, key.lodLevel);
                        else
                            renderer->RenderBatchInWorld(device, seconds, camera, instances);
                        const std::uint32_t submittedDrawCalls = renderer->LastSubmittedDrawCalls();
                        const std::uint32_t submittedInstances = renderer->LastSubmittedInstances();
                        const std::uint32_t submittedIndexCount = renderer->LastSubmittedIndexCount();
                        for (StaticMeshLodBatch::LodDispositionRecord& record : batch.lodDispositionRecords)
                        {
                            record.submitted = submittedDrawCalls > 0 && submittedInstances > 0;
                            record.fullResFallback = renderer->LastUsedFullResFallback();
                            record.drawIndexCount = record.submitted ? submittedIndexCount : 0u;
                            logLodDisposition(record);
                        }
                        if (submittedDrawCalls == 0 || submittedInstances == 0)
                            continue;
                        frameStaticMeshBatches += submittedDrawCalls;
                        frameStaticMeshMaxBatchSize = std::max(frameStaticMeshMaxBatchSize, instances.size());
                        frameStaticMeshSubmitted += submittedInstances;
                        frameStaticMeshDrawCalls += submittedDrawCalls;
                        frameStaticMeshTriangles += (renderer->LastUsedFullResFallback()
                            ? renderer->TriangleCount()
                            : renderer->TriangleCountForLod(key.configHash, key.lodLevel)) * submittedInstances;
                        frameStaticMeshUniformUpdates += renderer->LastMaterialUniformUpdates();
                        frameStaticMeshOverrideActiveDraws += renderer->LastOverrideActiveDraws();
                        frameStaticMeshInstanceBufferBytes += renderer->LastInstanceBufferBytes();
                        frameStaticMeshInstanceBufferRebuilt = frameStaticMeshInstanceBufferRebuilt || renderer->LastInstanceBufferRebuilt();
                        const bool meshSubmitDetailChanged =
                            !previousMeshSubmitDetailInitialized ||
                            previousMeshSubmitDetailInstances != submittedInstances ||
                            previousMeshSubmitDetailDrawCalls != submittedDrawCalls;
                        if ((!QuietLogsForLodDiag() && (frameNumber < 3 || (frameNumber % 60u) == 0u)) ||
                            (QuietLogsForLodDiag() && meshSubmitDetailChanged))
                        {
                            const auto& bmin = renderer->BoundsMin();
                            const auto& bmax = renderer->BoundsMax();
                            Tracenf("[MESH] Static instanced submit detail: instances=%u drawcalls=%u verts=%zu indices=%zu bbox_min=(%.3f,%.3f,%.3f) bbox_max=(%.3f,%.3f,%.3f)",
                                submittedInstances,
                                submittedDrawCalls,
                                renderer->VertexCount(),
                                renderer->IndexCount(),
                                bmin[0], bmin[1], bmin[2],
                                bmax[0], bmax[1], bmax[2]);
                        }
                        previousMeshSubmitDetailInstances = submittedInstances;
                        previousMeshSubmitDetailDrawCalls = submittedDrawCalls;
                        previousMeshSubmitDetailInitialized = true;
                    }
                    const bool lodSelectionChanged =
                        !previousFrameLodSelectionInitialized ||
                        previousFrameLodSelection != frameLodSelection;
                    const bool nonLodChanged =
                        !previousFrameNonLodInitialized ||
                        previousFrameNonLodEntities != frameNonLodEntities;
                    if (LodLogsEnabled() &&
                        ((!QuietLogsForLodDiag() && (frameNumber < 3 || (frameNumber % 60u) == 0u)) ||
                        (QuietLogsForLodDiag() && (lodSelectionChanged || nonLodChanged))))
                    {
                        Tracenf("[LOD] selection lod0=%u lod1=%u lod2=%u lod3=%u (LOD-active instances)",
                            frameLodSelection[0],
                            frameLodSelection[1],
                            frameLodSelection[2],
                            frameLodSelection[3]);
                        Tracenf("[LOD] non-lod entities=%u (always full-res)", frameNonLodEntities);
                    }
                    previousFrameLodSelection = frameLodSelection;
                    previousFrameLodSelectionInitialized = true;
                    previousFrameNonLodEntities = frameNonLodEntities;
                    previousFrameNonLodInitialized = true;
                    previousCulledMeshLogSet = std::move(currentCulledMeshLogSet);
                }
                if (!useOffscreenScene && hasSceneTerrain)
                {
                    terrain.RenderWater(device, camera, seconds);
                }
                if (!useOffscreenScene && worldLabelsOk)
                    worldLabels.Render(device, camera, plates);
            }
            else if (runtimeSession->IsLobbyActive() && skinnedMeshOk)
            {
                frameSceneRenderCalled = true;
                frameSceneEntityCount = 1;
                skinnedMesh.Render(device, seconds);
            }

            if (useOffscreenScene)
            {
                offscreenScene.EndMainPass(device);
                if (isInWorld && hasSceneTerrain)
                {
                    offscreenScene.SnapshotScene(device);
                    terrain.SetWaterRefractionInputs(offscreenScene.GetSceneColorSnapshotView(),
                        offscreenScene.GetSceneDepthSnapshotView(),
                        offscreenScene.GetLinearSampler(),
                        offscreenScene.GetExtent());
                    offscreenScene.BeginMainPass(device, false);
                    terrain.RenderWater(device, camera, seconds);
                    offscreenScene.EndMainPass(device);
                }
                device.BeginSwapchainRenderPass("composite");
                offscreenScene.RenderComposite(device);
                if (isInWorld && worldLabelsOk)
                    worldLabels.Render(device, camera, plates);
            }
            frameRmlUiRenderCalled = true;
            rmlUi.Render(device);
#if defined(IXTREEME_WITH_EDITOR)
            frameImGuiRenderCalled = true;
            engineStats.swapchainWidth = renderSize.width;
            engineStats.swapchainHeight = renderSize.height;
            engineStats.frameNumber = frameNumber;
            engineStats.sceneEntityCount = frameSceneEntityCount;
            engineStats.staticMeshSubmitted = frameStaticMeshSubmitted;
            engineStats.staticMeshDrawCalls = frameStaticMeshDrawCalls;
            editorImGui.SetEngineStats(engineStats);
            editorImGui.Render(device);
#else
            frameImGuiRenderCalled = false;
#endif
            const bool frameHeartbeatLog = QuietLogsForLodDiag()
                ? ((frameNumber % 60u) == 0u)
                : (frameNumber < 3 || (frameNumber % 60u) == 0u);
            if (frameHeartbeatLog)
            {
                if (!QuietLogsForLodDiag())
                {
                    Tracenf("[FRAME] static_mesh entities=%zu submitted=%zu drawcalls=%zu",
                        frameStaticMeshEntityCount,
                        frameStaticMeshSubmitted,
                        frameStaticMeshDrawCalls);
                }
                const bool mperfMainChanged =
                    !previousMperfMain.initialized ||
                    previousMperfMain.drawcalls != frameStaticMeshDrawCalls ||
                    previousMperfMain.tris != frameStaticMeshTriangles;
                if (!QuietLogsForLodDiag() || mperfMainChanged)
                {
                    Tracenf("[MPERF] pass=main drawcalls=%zu tris=%zu",
                        frameStaticMeshDrawCalls,
                        frameStaticMeshTriangles);
                }
                previousMperfMain.drawcalls = frameStaticMeshDrawCalls;
                previousMperfMain.tris = frameStaticMeshTriangles;
                previousMperfMain.initialized = true;

                if (!QuietLogsForLodDiag())
                {
                    for (int cascade = 0; cascade < 4; ++cascade)
                        Tracenf("[MPERF] pass=shadow-cascade%d drawcalls=0 tris=0", cascade);
                    Tracen("[MPERF] pass=water-reflection drawcalls=0 tris=0");
                }

                const bool mperfOverrideChanged =
                    !previousMperfOverride.initialized ||
                    previousMperfOverride.uniformUpdates != frameStaticMeshUniformUpdates ||
                    previousMperfOverride.activeOverrideDraws != frameStaticMeshOverrideActiveDraws;
                if (!QuietLogsForLodDiag() || mperfOverrideChanged)
                {
                    Tracenf("[MPERF] mmat overrideUpdatesThisFrame=%zu activeOverrideDraws=%zu mode=every-frame descriptorAllocPerDraw=no bufferMapPerDraw=yes queueWaitPerDraw=no",
                        frameStaticMeshUniformUpdates,
                        frameStaticMeshOverrideActiveDraws);
                }
                previousMperfOverride.uniformUpdates = frameStaticMeshUniformUpdates;
                previousMperfOverride.activeOverrideDraws = frameStaticMeshOverrideActiveDraws;
                previousMperfOverride.initialized = true;

                const bool mperfMeshesChanged =
                    !previousMperfMeshes.initialized ||
                    previousMperfMeshes.total != frameStaticMeshEntityCount ||
                    previousMperfMeshes.culled != frameStaticMeshFrustumCulled ||
                    previousMperfMeshes.drawn != frameStaticMeshSubmitted;
                if (!QuietLogsForLodDiag() || mperfMeshesChanged)
                {
                    Tracenf("[MPERF] meshes total=%zu frustumCulled=%zu drawn=%zu cullEnabled=yes",
                        frameStaticMeshEntityCount,
                        frameStaticMeshFrustumCulled,
                        frameStaticMeshSubmitted);
                }
                previousMperfMeshes.total = frameStaticMeshEntityCount;
                previousMperfMeshes.culled = frameStaticMeshFrustumCulled;
                previousMperfMeshes.drawn = frameStaticMeshSubmitted;
                previousMperfMeshes.initialized = true;

                const bool instSummaryChanged =
                    !previousInstSummary.initialized ||
                    previousInstSummary.batches != frameStaticMeshBatches ||
                    previousInstSummary.draws != frameStaticMeshDrawCalls ||
                    previousInstSummary.instances != frameStaticMeshSubmitted ||
                    previousInstSummary.maxBatch != frameStaticMeshMaxBatchSize;
                if (!QuietLogsForLodDiag() || instSummaryChanged)
                {
                    Tracenf("[INST] batches=%zu instancedDraws=%zu instancesTotal=%zu maxBatchSize=%zu",
                        frameStaticMeshBatches,
                        frameStaticMeshDrawCalls,
                        frameStaticMeshSubmitted,
                        frameStaticMeshMaxBatchSize);
                }
                previousInstSummary.batches = frameStaticMeshBatches;
                previousInstSummary.draws = frameStaticMeshDrawCalls;
                previousInstSummary.instances = frameStaticMeshSubmitted;
                previousInstSummary.maxBatch = frameStaticMeshMaxBatchSize;
                previousInstSummary.initialized = true;

                const bool instBufferChanged =
                    !previousInstBuffer.initialized ||
                    previousInstBuffer.bytes != frameStaticMeshInstanceBufferBytes ||
                    frameStaticMeshInstanceBufferRebuilt;
                if (!QuietLogsForLodDiag() || instBufferChanged)
                {
                    Tracenf("[INST] instanceBufferBytes=%zu rebuiltThisFrame=%s",
                        frameStaticMeshInstanceBufferBytes,
                        frameStaticMeshInstanceBufferRebuilt ? "yes" : "no");
                }
                previousInstBuffer.bytes = frameStaticMeshInstanceBufferBytes;
                previousInstBuffer.initialized = true;

                if (!QuietLogsForLodDiag())
                {
                    Tracenf("[SPATIAL] frustumQuery nodesVisited=%u candidates=%u total=%u",
                        frameStaticMeshSpatialStats.nodesVisited,
                        frameStaticMeshSpatialStats.candidates,
                        frameStaticMeshSpatialStats.totalObjects);
                    const VkExtent2D mperfExtent = useOffscreenScene ? offscreenScene.GetExtent() : renderSize;
                    Tracenf("[MPERF] offscreen=%ux%u halfResTestFps=n/a boundHint=unknown",
                        mperfExtent.width,
                        mperfExtent.height);
                }
                Tracenf("[FRAME] summary frame=%llu imgui_render called=%s rmlui_render called=%s scene_render called=%s entity_count=%zu clear_color=(0.04,0.05,0.09,1.00) in_world=%d lobby=%d editor_open=%d swapchain=%ux%u",
                    static_cast<unsigned long long>(frameNumber),
                    frameImGuiRenderCalled ? "yes" : "no",
                    frameRmlUiRenderCalled ? "yes" : "no",
                    frameSceneRenderCalled ? "yes" : "no",
                    frameSceneEntityCount,
                    isInWorld ? 1 : 0,
                    runtimeSession->IsLobbyActive() ? 1 : 0,
                    runtimeSession->IsMapEditorOpen() ? 1 : 0,
                    renderSize.width,
                    renderSize.height);
            }
        }
        else
        {
            static uint32_t inactiveFrameLogs = 0;
            if (inactiveFrameLogs < 3)
            {
                ++inactiveFrameLogs;
                Tracenf("[FRAME] inactive: imgui_render called=no rmlui_render called=no scene_render called=no clear_color=(0.04,0.05,0.09,1.00) swapchain=%ux%u",
                    renderSize.width,
                    renderSize.height);
            }
        }
        device.EndFrame();
    }

    device.WaitIdle();
    if (worldLabelsOk)
        worldLabels.Destroy();
    for (auto& [path, entry] : staticMeshCache)
    {
        (void)path;
        if (entry.renderer)
            entry.renderer->Destroy();
    }
    if (offscreenSceneOk)
        offscreenScene.Destroy();
    if (terrainOk)
        terrain.Destroy();
    if (skinnedMeshOk)
        skinnedMesh.Destroy();
#if defined(IXTREEME_WITH_EDITOR)
    editorImGui.Destroy();
#if defined(_WIN32)
    if (NativeWindow_Win32* cleanupWin32Window = dynamic_cast<NativeWindow_Win32*>(&window))
        cleanupWin32Window->SetMessageCallback({});
#endif
#endif
    rmlUi.Destroy();
    runtimeSession->Destroy();
    device.Destroy();
    return 0;
}
}

#if defined(_WIN32)
int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int showCommand)
{
    (void)showCommand;

    uint32_t windowWidth = 1280;
    uint32_t windowHeight = 720;
    if (NativeWindow_Win32::GetPrimaryMonitorResolution(windowWidth, windowHeight))
    {
        Tracenf("[BOOT] native monitor resolution = %ux%u", windowWidth, windowHeight);
    }
    else
    {
        Tracenf("[BOOT] native monitor resolution unavailable -> fallback = %ux%u", windowWidth, windowHeight);
    }

    NativeWindow_Win32 window;
    if (!window.Create(instance, "IxtreemeWorld Engine - Editor", windowWidth, windowHeight))
    {
        ShowFatal("Failed to create Win32 window.");
        return 1;
    }

    client::asset::FileAssetReader assets(ResolveEngineAssetRoot());
    const int result = RunGame(window, assets);
    window.Destroy();
    return result;
}

int main()
{
    return WinMain(GetModuleHandleA(nullptr), nullptr, nullptr, SW_SHOWNORMAL);
}
#endif

#if defined(__ANDROID__)
android_app* g_androidApp = nullptr;

extern "C" void android_main(android_app* state)
{
    g_androidApp = state;
    NativeWindow_Android window(state);
    if (!window.WaitForWindow())
    {
        g_androidApp = nullptr;
        return;
    }

    client::asset::AAssetManagerAssetReader assets(state->activity->assetManager);
    (void)RunGame(window, assets);
    g_androidApp = nullptr;
}
#endif

