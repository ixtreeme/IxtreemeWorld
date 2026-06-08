#if defined(_WIN32)
#include <winsock2.h>
#include <windows.h>
#endif

#include "NameplateRenderer.h"
#include "NativeWindow.h"
#include "EditorImGui.h"
#if defined(_WIN32)
#include "NativeWindow_Win32.h"
#endif
#if defined(__ANDROID__)
#include "NativeWindow_Android.h"
#endif
#include "OffscreenSceneRenderer.h"
#include "RmlUiLayer.h"
#include "RuntimeSession.h"
#include "RuntimeUiAdapter.h"
#include "SceneManager.h"
#include "TerrainRenderer.h"
#include "VulkanDevice.h"
#include "WarriorRenderer.h"
#include "Debug.h"
#include "asset/IAssetReader.h"

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
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
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

WorldVec3 ServerMetersToDisplay(client::net::Vec3 position)
{
    return {position.x, position.z, -position.y};
}

client::net::Vec3 DisplayToServerMeters(WorldVec3 position)
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

std::uint32_t PickMobTarget(const std::vector<WorldRenderEntity>& entities,
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
        if (entity.mobTypeId == 0)
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
    PointLight,
    SpotLight,
    WaterBody
};

struct SelectedEditorObject
{
    SelectedEditorObjectType type = SelectedEditorObjectType::None;
    std::uint32_t id = 0;
};

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

WarriorRenderer::MotionState ToWarriorMotion(client::net::MoveState state)
{
    switch (state)
    {
    case client::net::MoveState::Walking:
        return WarriorRenderer::MotionState::Walk;
    case client::net::MoveState::Running:
        return WarriorRenderer::MotionState::Run;
    default:
        return WarriorRenderer::MotionState::Idle;
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

    client::net::MoveState State() const
    {
        if (!HasDirection())
            return client::net::MoveState::Idle;
        return shift ? client::net::MoveState::Running : client::net::MoveState::Walking;
    }
};

std::optional<client::net::DebugSpawnOverride> ParseDebugSpawnOverride(const std::string& commandLine)
{
    const std::string flag = "--debug-spawn=";
    const std::size_t begin = commandLine.find(flag);
    if (begin == std::string::npos)
        return std::nullopt;

    std::size_t valueBegin = begin + flag.size();
    while (valueBegin < commandLine.size() &&
           (commandLine[valueBegin] == '"' || commandLine[valueBegin] == '\'')) {
        ++valueBegin;
    }
    std::size_t valueEnd = valueBegin;
    while (valueEnd < commandLine.size() &&
           !std::isspace(static_cast<unsigned char>(commandLine[valueEnd])) &&
           commandLine[valueEnd] != '"' &&
           commandLine[valueEnd] != '\'') {
        ++valueEnd;
    }

    const std::string value = commandLine.substr(valueBegin, valueEnd - valueBegin);
    const std::size_t comma = value.find(',');
    if (comma == std::string::npos) {
        Tracenf("[ARGS] invalid --debug-spawn value: %s", value.c_str());
        return std::nullopt;
    }

    char* endX = nullptr;
    char* endY = nullptr;
    const std::string xText = value.substr(0, comma);
    const std::string yText = value.substr(comma + 1);
    const float x = std::strtof(xText.c_str(), &endX);
    const float y = std::strtof(yText.c_str(), &endY);
    if (endX == xText.c_str() || *endX != '\0' || endY == yText.c_str() || *endY != '\0' ||
        !std::isfinite(x) || !std::isfinite(y)) {
        Tracenf("[ARGS] invalid --debug-spawn value: %s", value.c_str());
        return std::nullopt;
    }

    Tracenf("[ARGS] debug spawn override requested: %.2f, %.2f", x, y);
    return client::net::DebugSpawnOverride{x, y};
}

class CameraController
{
public:
    struct Snapshot
    {
        bool flyMode = false;
        float yaw = 0.0f;
        float pitch = 0.0f;
        float distance = 0.0f;
        WorldVec3 lastEye{};
        WorldVec3 flyEye{};
        float flyYaw = 0.0f;
        float flyPitch = 0.0f;
    };

    void SetEditorFlyMode(bool enabled)
    {
        if (enabled == flyMode_)
            return;

        flyMode_ = enabled;
        dragActive_ = false;
        if (flyMode_)
        {
            flyEye_ = lastEye_;
            flyYaw_ = yaw_;
            flyPitch_ = pitch_;
            Tracen("[CAMERA] mode=editor-fly");
        }
        else
        {
            yaw_ = flyYaw_;
            pitch_ = std::clamp(flyPitch_, -kMaxPitch, kMaxPitch);
            Tracen("[CAMERA] mode=tps");
        }
    }

    bool HandleInput(const InputEvent& event)
    {
        if (event.type == InputEvent::KeyUp && event.key == Key_F1)
        {
            f1Down_ = false;
            return true;
        }

        if (event.type == InputEvent::KeyDown && event.key == Key_F1)
        {
            if (f1Down_)
                return true;
            f1Down_ = true;
            SetEditorFlyMode(!flyMode_);
            return true;
        }

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
            zoomVelocity_ -= static_cast<float>(event.wheelDelta) / 120.0f * 1.2f;
            return true;
        default:
            return false;
        }
    }

    void Update(double dt, const MovementInputState& movement)
    {
        const float frameDt = static_cast<float>(std::clamp(dt, 0.0, 0.05));
        yaw_ += yawVelocity_;
        pitch_ = std::clamp(pitch_ + pitchVelocity_, -kMaxPitch, kMaxPitch);
        distance_ = std::clamp(distance_ + zoomVelocity_, kMinDistance, kMaxDistance);

        yawVelocity_ *= 0.85f;
        pitchVelocity_ *= 0.85f;
        zoomVelocity_ *= 0.85f;
        if (std::fabs(yawVelocity_) < 0.00001f)
            yawVelocity_ = 0.0f;
        if (std::fabs(pitchVelocity_) < 0.00001f)
            pitchVelocity_ = 0.0f;
        if (std::fabs(zoomVelocity_) < 0.001f)
            zoomVelocity_ = 0.0f;

        if (flyMode_)
            UpdateFly(frameDt, movement);
    }

    WorldCamera BuildCamera(uint32_t width, uint32_t height, WorldVec3 target)
    {
        if (flyMode_)
            return BuildFlyCamera(width, height);

        WorldCamera camera = BuildOrbitCamera(width, height, target, yaw_, pitch_, distance_);
        lastEye_ = camera.eye;
        return camera;
    }

    bool IsFlyMode() const { return flyMode_; }
    float MovementYaw() const { return flyMode_ ? flyYaw_ : yaw_; }
    Snapshot SaveSnapshot() const
    {
        return Snapshot{flyMode_, yaw_, pitch_, distance_, lastEye_, flyEye_, flyYaw_, flyPitch_};
    }
    void RestoreSnapshot(const Snapshot& snapshot)
    {
        flyMode_ = snapshot.flyMode;
        yaw_ = snapshot.yaw;
        pitch_ = snapshot.pitch;
        distance_ = snapshot.distance;
        lastEye_ = snapshot.lastEye;
        flyEye_ = snapshot.flyEye;
        flyYaw_ = snapshot.flyYaw;
        flyPitch_ = snapshot.flyPitch;
        yawVelocity_ = 0.0f;
        pitchVelocity_ = 0.0f;
        zoomVelocity_ = 0.0f;
        dragActive_ = false;
    }

    void FocusOn(WorldVec3 target, float distance = 15.0f)
    {
        flyMode_ = true;
        const WorldVec3 eye = {target.x, target.y + 5.0f, target.z - distance};
        const WorldVec3 forward = WorldNormalize(WorldSub(target, eye));
        flyEye_ = eye;
        lastEye_ = eye;
        flyYaw_ = std::atan2(forward.x, forward.z);
        flyPitch_ = std::clamp(std::asin(std::clamp(forward.y, -1.0f, 1.0f)), -kMaxPitch, kMaxPitch);
        yaw_ = flyYaw_;
        pitch_ = flyPitch_;
        distance_ = std::clamp(distance, kMinDistance, kMaxDistance);
        yawVelocity_ = 0.0f;
        pitchVelocity_ = 0.0f;
        zoomVelocity_ = 0.0f;
        dragActive_ = false;
        Tracenf("[HIERARCHY] Focused camera on target: %.2f, %.2f, %.2f", target.x, target.y, target.z);
    }

private:
    static constexpr float kMaxPitch = 80.0f * 3.1415926535f / 180.0f;
    static constexpr float kMinDistance = 4.0f;
    static constexpr float kMaxDistance = 32.0f;

    void ApplyMouseDelta(float dx, float dy)
    {
        constexpr float kSensitivity = 0.0045f;
        if (flyMode_)
        {
            flyYaw_ += dx * kSensitivity;
            flyPitch_ = std::clamp(flyPitch_ + dy * kSensitivity, -kMaxPitch, kMaxPitch);
            return;
        }

        yawVelocity_ = dx * kSensitivity;
        pitchVelocity_ = -dy * kSensitivity;
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

        const float cosPitch = std::cos(flyPitch_);
        const WorldVec3 forward = {std::sin(flyYaw_) * cosPitch, std::sin(flyPitch_), std::cos(flyYaw_) * cosPitch};
        const WorldVec3 right = {std::cos(flyYaw_), 0.0f, -std::sin(flyYaw_)};
        WorldVec3 delta = WorldAdd(WorldAdd(WorldScale(forward, localZ), WorldScale(right, localX)),
            {0.0f, localY, 0.0f});
        if (WorldDot(delta, delta) > 0.0001f)
            delta = WorldNormalize(delta);

        const float speed = movement.shift ? 22.0f : 9.0f;
        flyEye_ = WorldAdd(flyEye_, WorldScale(delta, speed * dt));
    }

    WorldCamera BuildFlyCamera(uint32_t width, uint32_t height) const
    {
        const float aspect = height != 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
        const float cosPitch = std::cos(flyPitch_);
        const WorldVec3 forward = {std::sin(flyYaw_) * cosPitch, std::sin(flyPitch_), std::cos(flyYaw_) * cosPitch};
        WorldCamera camera{};
        camera.eye = flyEye_;
        camera.target = WorldAdd(flyEye_, forward);
        const WorldMat4 view = WorldLookAt(camera.eye, camera.target, {0.0f, 1.0f, 0.0f});
        camera.nearPlane = 0.1f;
        camera.farPlane = 1000.0f;
        const WorldMat4 projection = WorldPerspective(45.0f * 3.1415926535f / 180.0f, aspect, camera.nearPlane, camera.farPlane);
        camera.viewProjection = WorldMultiply(view, projection);
        return camera;
    }

    bool flyMode_ = false;
    bool f1Down_ = false;
    bool dragActive_ = false;
    int lastMouseX_ = 0;
    int lastMouseY_ = 0;
    float yaw_ = 3.1415926535f;
    float pitch_ = 25.0f * 3.1415926535f / 180.0f;
    float distance_ = 18.0f;
    float yawVelocity_ = 0.0f;
    float pitchVelocity_ = 0.0f;
    float zoomVelocity_ = 0.0f;
    WorldVec3 lastEye_ = {0.0f, 8.0f, -18.0f};
    WorldVec3 flyEye_ = {0.0f, 8.0f, -18.0f};
    float flyYaw_ = 3.1415926535f;
    float flyPitch_ = 25.0f * 3.1415926535f / 180.0f;
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
    constexpr const char* kFallbackStartupScene = "scenes/Login.scene";
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
        Tracenf("[BOOT] config missing/empty -> fallback = %s", kFallbackStartupScene);
        return kFallbackStartupScene;
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

    Tracenf("[BOOT] config missing/empty -> fallback = %s", kFallbackStartupScene);
    return kFallbackStartupScene;
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
    std::optional<CameraController::Snapshot> editorCameraSnapshot;
    std::string playStartScenePath;
    SceneData playStartSceneSnapshot;
    bool playStartSceneWasOpen = false;
    bool playStartSceneDirty = false;
    bool directGameplayDevCharacter = false;
    client::net::Vec3 playerPosition{};
    std::uint16_t playerHeading = 0;
};

enum class RuntimeImplementation
{
    Empty,
    Auriga
};

constexpr RuntimeImplementation kActiveRuntimeImplementation = RuntimeImplementation::Auriga;

std::unique_ptr<RuntimeSession> CreateRuntimeSession(RuntimeImplementation implementation)
{
    switch (implementation)
    {
    case RuntimeImplementation::Empty:
        Tracen("[RUNTIME] active session = EmptyRuntimeSession");
        return CreateEmptyRuntimeSession();
    case RuntimeImplementation::Auriga:
    default:
        Tracen("[RUNTIME] active session = AurigaRuntimeSession");
        return CreateAurigaRuntimeSession();
    }
}

std::unique_ptr<RuntimeUiAdapter> CreateRuntimeUiAdapter(RuntimeImplementation implementation,
                                                        RmlUiLayer& rmlUi,
                                                        client::asset::IAssetReader& assets,
                                                        std::function<bool()> isRuntimeFlowActive)
{
    switch (implementation)
    {
    case RuntimeImplementation::Empty:
        Tracen("[RUNTIME] active UI adapter = NullRuntimeUiAdapter");
        return CreateNullRuntimeUiAdapter(rmlUi);
    case RuntimeImplementation::Auriga:
    default:
        Tracen("[RUNTIME] active UI adapter = AurigaRuntimeUiAdapter");
        return CreateAurigaRuntimeUiAdapter(rmlUi, assets, std::move(isRuntimeFlowActive));
    }
}

int RunGame(NativeWindow& window,
            client::asset::IAssetReader& assets,
            std::optional<client::net::DebugSpawnOverride> debugSpawnOverride = std::nullopt)
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
    Tracen("[BOOT] entry state = release boot, loading startup_scene from app_config");
#endif
    Tracenf("[BOOT] window size = %ux%u", window.GetWidth(), window.GetHeight());

    VkExtent2D renderSize = device.GetSwapchainExtent();
    Tracenf("[BOOT] swapchain size = %ux%u", renderSize.width, renderSize.height);
    std::unique_ptr<RuntimeSession> runtimeSession = CreateRuntimeSession(kActiveRuntimeImplementation);
    runtimeSession->SetDebugSpawnOverride(debugSpawnOverride);
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
#if defined(IXTREEME_WITH_EDITOR)
    bool editorRuntimeFlowActive = false;
#endif
    auto isRuntimeFlowActive = [&]() {
#if defined(IXTREEME_WITH_EDITOR)
        return editorRuntimeFlowActive;
#else
        return true;
#endif
    };
    std::unique_ptr<RuntimeUiAdapter> runtimeUi =
        CreateRuntimeUiAdapter(kActiveRuntimeImplementation, rmlUi, assets, isRuntimeFlowActive);
    runtimeUi->SetQuitCallback([&window]() {
        window.RequestClose();
    });
    runtimeUi->InstallSceneRouting(SceneManager::Instance());
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
    LoadRuntimeScene(assets, StartupSceneFromConfig(assets));
#endif
    const std::string warriorModelPath = "assets/Character/KicsiK.glb";

    WarriorRenderer warrior;
    bool warriorOk = warrior.Create(device, assets, warriorModelPath);
    if (!warriorOk)
    {
        Tracenf("[MAIN] WarriorRenderer failed to initialize - 3D warrior preview will not be available");
        warrior.Destroy();
    }

    TerrainRenderer terrain;
    bool terrainOk = terrain.Create(device, assets);
    if (!terrainOk)
    {
        Tracenf("[MAIN] TerrainRenderer failed to initialize - terrain will not be available");
        terrain.Destroy();
    }
    else
    {
        runtimeSession->InitializeAssetLibrary("assets/Maps/test_zone", terrain.GetPaletteSlots());
#if defined(IXTREEME_WITH_EDITOR)
        editorImGui.SetPaletteSlots(runtimeSession->GetPaletteSlots());
        editorImGui.SetWaterMaterials(editorImGui.GetWaterMaterialsSnapshot());
#endif
        if (!terrain.ApplyPaletteSlots(device, runtimeSession->GetPaletteSlots()))
            Tracenf("[MAIN] world palette could not be applied; keeping initial terrain palette");
    }

    NameplateRenderer nameplates;
    bool nameplatesOk = nameplates.Create(device, assets);
    if (!nameplatesOk)
    {
        Tracenf("[MAIN] NameplateRenderer failed to initialize - nameplates will not be available");
        nameplates.Destroy();
    }

    OffscreenSceneRenderer offscreenScene;
    bool offscreenSceneOk = offscreenScene.Create(device, assets);
    if (offscreenSceneOk)
    {
        if (warriorOk)
        {
            warrior.SetMainRenderPass(offscreenScene.GetRenderPass());
            warrior.RecreatePipeline(device);
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
    }

    runtimeSession->SetQuitCallback([&window]()
    {
        window.RequestClose();
    });

    MovementInputState movement;
    CameraController cameraController;
#if defined(IXTREEME_WITH_EDITOR)
    EditorPlayRuntime editorPlay;
    runtimeSession->SetMapEditorOpen(true);
    if (terrainOk)
        terrain.SetMapEditorOpen(true);
    cameraController.SetEditorFlyMode(true);
    runtimeSession->SetEditorStatus("Editor opened at boot");
    Tracen("[BOOT] editor_open forced = 1 (editor build boot)");
#endif
    std::vector<WorldRenderEntity> lastPickEntities;
    WorldCamera lastPickCamera{};
    bool hasLastPickCamera = false;
    std::uint32_t selectedTargetNetId = 0;
    std::vector<PointLight> editorPointLights;
    std::vector<SpotLight> editorSpotLights;
    std::vector<WaterBody> editorWaterBodies = terrainOk ? terrain.GetWaterBodies() : std::vector<WaterBody>{};
    bool editorWaterBodiesDirty = false;
#if defined(IXTREEME_WITH_EDITOR)
    std::uint32_t nextEditorLightId = 1;
#endif
    std::uint32_t nextEditorWaterBodyId = 1;
    for (const WaterBody& body : editorWaterBodies)
        nextEditorWaterBodyId = std::max(nextEditorWaterBodyId, body.id + 1u);
    SelectedEditorObject selectedEditorObject;
    EditorGizmoMode editorGizmoMode = EditorGizmoMode::Translate;
    bool editorGizmoSnapEnabled = false;
    float editorGizmoSnapValue = 1.0f;
    bool editorObjectDragActive = false;
    int editorObjectDragLastX = 0;
    int editorObjectDragLastY = 0;
#if defined(IXTREEME_WITH_EDITOR)
    auto buildSceneSnapshot = [&]() {
        SceneData scene;
        scene.lighting = editorImGui.GetLightingState();
        scene.waterBodies = editorWaterBodies;
        scene.pointLights = editorPointLights;
        scene.spotLights = editorSpotLights;
        scene.paletteSlots = terrainOk ? terrain.GetPaletteSlots() : runtimeSession->GetPaletteSlots();
        return scene;
    };
    auto applySceneData = [&](const SceneData& scene) {
        editorWaterBodies = scene.waterBodies;
        editorPointLights = scene.pointLights;
        editorSpotLights = scene.spotLights;
        selectedEditorObject = {};
        nextEditorWaterBodyId = 1;
        for (const WaterBody& body : editorWaterBodies)
            nextEditorWaterBodyId = std::max(nextEditorWaterBodyId, body.id + 1u);
        nextEditorLightId = 1;
        for (const PointLight& light : editorPointLights)
            nextEditorLightId = std::max(nextEditorLightId, light.id + 1u);
        for (const SpotLight& light : editorSpotLights)
            nextEditorLightId = std::max(nextEditorLightId, light.id + 1u);

        editorImGui.SetLightingState(scene.lighting);
        runtimeSession->SetDynamicLightEditorState({});
        runtimeSession->SetWaterBodyEditorState({});
        if (terrainOk)
        {
            terrain.SetLightingState(scene.lighting);
            terrain.SetWaterBodies(device, editorWaterBodies);
            editorWaterBodies = terrain.GetWaterBodies();
            if (terrain.ApplyPaletteSlots(device, scene.paletteSlots))
                editorImGui.SetPaletteSlots(terrain.GetPaletteSlots());
        }
        editorWaterBodiesDirty = false;
        Tracenf("[SCENE] Applied editor scene state: water=%zu point=%zu spot=%zu",
            editorWaterBodies.size(),
            editorPointLights.size(),
            editorSpotLights.size());
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
#endif
                             &renderSize](const InputEvent& event)
    {
#if defined(IXTREEME_WITH_EDITOR)
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
            return;
        }
        if (runtimeSession->IsMapEditorOpen() && event.type == InputEvent::KeyDown && event.key == Key_F6)
        {
            if (editorPlay.state.mode == EditorPlayMode::Play)
                editorPlay.state.mode = EditorPlayMode::PlayPaused;
            else if (editorPlay.state.mode == EditorPlayMode::PlayPaused)
                editorPlay.state.mode = EditorPlayMode::Play;
            movement.Clear();
            return;
        }
#endif
        if (editorImGui.WantsInputCapture(event))
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
            selectedTargetNetId = PickMobTarget(lastPickEntities,
                                               lastPickCamera,
                                               renderSize.width,
                                               renderSize.height,
                                               event.x,
                                               event.y);
            Tracenf("[COMBAT] selected target net_id=%u", selectedTargetNetId);
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
                cameraController.SetEditorFlyMode(false);
            }
            return;
        }
        if (event.type == InputEvent::KeyDown && event.key == Key_F4 && terrainOk &&
            runtimeSession->IsInWorld())
        {
            runtimeSession->ToggleMapEditor();
            runtimeSession->ClearKeyboardFocus();
            terrain.SetMapEditorOpen(runtimeSession->IsMapEditorOpen());
            cameraController.SetEditorFlyMode(runtimeSession->IsMapEditorOpen());
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
                }
                if (terrain.HandleEditorInput(event))
                    return;
            }

            bool consumedByEditorUi = false;
            if (event.type == InputEvent::MouseMove ||
                event.type == InputEvent::MouseDown ||
                event.type == InputEvent::MouseUp ||
                event.type == InputEvent::MouseWheel)
            {
                consumedByEditorUi = runtimeSession->OnInput(event);
                const MapEditorSettings editorSettings = editorImGui.GetMapEditorSettings();
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
                        event.x,
                        event.y);
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
                if (!consumedByEditorUi && terrainToolActive &&
                    (event.type == InputEvent::MouseMove ||
                     event.type == InputEvent::MouseDown ||
                     event.type == InputEvent::MouseUp))
                {
                    editorObjectDragActive = false;
                    terrain.HandleEditorInput(event);
                    return;
                }

                if (event.type == InputEvent::MouseUp)
                {
                    if (event.button == MouseButton_Left)
                        editorObjectDragActive = false;
                    terrain.HandleEditorInput(event);
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
                                event.x,
                                event.y))
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
                                event.x,
                                event.y))
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
                                event.x,
                                event.y))
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
                    terrain.HandleEditorInput(event);
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
            && terrain.HandleEditorInput(event))
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
                        if (warriorOk)
                            warrior.SetMainRenderPass(offscreenScene.GetRenderPass());
                        if (terrainOk)
                        {
                            terrain.SetMainRenderPass(offscreenScene.GetRenderPass());
                            terrain.SetWaterRefractionInputs(offscreenScene.GetSceneColorSnapshotView(),
                                offscreenScene.GetSceneDepthSnapshotView(),
                                offscreenScene.GetLinearSampler(),
                                offscreenScene.GetExtent());
                        }
                    }
                }
                if (warriorOk)
                    warrior.RecreatePipeline(device);
                if (terrainOk)
                    terrain.RecreatePipeline(device);
                if (nameplatesOk)
                    nameplates.RecreatePipeline(device);
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
        cameraController.Update(deltaSeconds, movement);
#if defined(IXTREEME_WITH_EDITOR)
        if (runtimeSession->IsLocalPlayMode())
        {
            if (editorPlay.state.mode == EditorPlayMode::Play)
            {
                const float frameDt = static_cast<float>(std::clamp(deltaSeconds, 0.0, 0.05));
                const float yaw = cameraController.MovementYaw();
                WorldVec3 localMove{};
                if (movement.w)
                {
                    localMove.x += std::sin(yaw);
                    localMove.z += std::cos(yaw);
                }
                if (movement.s)
                {
                    localMove.x -= std::sin(yaw);
                    localMove.z -= std::cos(yaw);
                }
                if (movement.d)
                {
                    localMove.x += std::cos(yaw);
                    localMove.z -= std::sin(yaw);
                }
                if (movement.a)
                {
                    localMove.x -= std::cos(yaw);
                    localMove.z += std::sin(yaw);
                }
                if (WorldDot(localMove, localMove) > 0.0001f)
                {
                    localMove = WorldNormalize(localMove);
                    WorldVec3 displayPos = ServerMetersToDisplay(editorPlay.playerPosition);
                    const float speed = movement.shift ? 9.0f : 4.5f;
                    displayPos = WorldAdd(displayPos, WorldScale(localMove, speed * frameDt));
                    if (terrainOk)
                        displayPos.y = terrain.SampleHeight(displayPos);
                    editorPlay.playerPosition = DisplayToServerMeters(displayPos);
                    constexpr float kTwoPi = 6.28318530717958647692f;
                    float heading = std::atan2(localMove.x, localMove.z);
                    if (heading < 0.0f)
                        heading += kTwoPi;
                    editorPlay.playerHeading = static_cast<std::uint16_t>((heading / kTwoPi) * 65535.0f);
                }
                runtimeSession->UpdateLocalPlayPlayer(editorPlay.playerPosition,
                    editorPlay.playerHeading,
                    movement.State());
                editorPlay.state.elapsedSeconds += deltaSeconds;
                ++editorPlay.state.frameCount;
            }
        }
        else
#endif
        {
            runtimeSession->UpdateNetwork();
            runtimeSession->SendMoveInput(
                movement.DirectionAngle(cameraController.MovementYaw()),
                cameraController.IsFlyMode() ? client::net::MoveState::Idle : movement.State());
        }
        runtimeSession->Update(seconds);
        rmlUi.Update();

        std::vector<WorldRenderEntity> frameEntities;
        WorldCamera frameCamera{};
        bool hasFrameCamera = false;
        if (runtimeSession->IsInWorld())
        {
            frameEntities = runtimeSession->GetWorldEntities();
            WorldVec3 cameraTarget{};
            bool hasOwn = false;
            for (const auto& entity : frameEntities)
            {
                if (entity.netId == runtimeSession->GetOwnNetId())
                {
                    cameraTarget = ServerMetersToDisplay(entity.position);
                    hasOwn = true;
                    break;
                }
            }
            if (!hasOwn && !frameEntities.empty())
                cameraTarget = ServerMetersToDisplay(frameEntities.front().position);
            frameCamera = cameraController.BuildCamera(renderSize.width, renderSize.height, cameraTarget);
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
                if (entity.netId == runtimeSession->GetOwnNetId())
                    ownEntity = &entity;
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
#if defined(IXTREEME_WITH_EDITOR)
                if (runtimeSession->IsLocalPlayMode())
                {
                    hudData.currentMp = 500.0f;
                    hudData.maxMp = 500.0f;
                    hudData.currentXp = 0;
                    hudData.xpForNextLevel = 1000;
                }
#endif
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
                auto hasOwnRuntimeCharacter = [&]() {
                    if (!runtimeSession->IsInWorld())
                        return false;
                    const std::uint32_t ownNetId = runtimeSession->GetOwnNetId();
                    if (ownNetId == 0)
                        return false;
                    const std::vector<WorldRenderEntity> worldEntities = runtimeSession->GetWorldEntities();
                    return std::any_of(worldEntities.begin(), worldEntities.end(), [ownNetId](const WorldRenderEntity& entity) {
                        return entity.netId == ownNetId;
                    });
                };
                auto injectDirectGameplayDevCharacter = [&]() {
                    WorldVec3 spawn = spawnAtCameraCenter();
                    if (terrainOk)
                        spawn.y = terrain.SampleHeight(spawn);
                    editorPlay.playerPosition = DisplayToServerMeters(spawn);
                    editorPlay.playerHeading = 0;

                    WorldRenderEntity player{};
                    player.netId = 1;
                    player.name = "DevPlayer";
                    player.position = editorPlay.playerPosition;
                    player.heading = editorPlay.playerHeading;
                    player.moveState = client::net::MoveState::Idle;
                    player.mobTypeId = 0;
                    player.level = 50;
                    player.hpCurrent = 1000.0f;
                    player.hpMax = 1000.0f;
                    player.hpDisplayed = 1000.0f;
                    runtimeSession->EnterLocalPlayMode(player);
                    selectedTargetNetId = 0;
                    editorPlay.directGameplayDevCharacter = true;
                    Tracen("[EDIT-PLAY-2] Direct gameplay scene Play: dev character injected");
                };
                auto selectHierarchyEntity = [&](HierarchyEntityType type, std::uint32_t id) {
                    switch (type)
                    {
                    case HierarchyEntityType::WaterBody:
                        selectedEditorObject = {SelectedEditorObjectType::WaterBody, id};
                        runtimeSession->SetEditorStatus("Selected water body #" + std::to_string(id));
                        break;
                    case HierarchyEntityType::PointLight:
                        selectedEditorObject = {SelectedEditorObjectType::PointLight, id};
                        runtimeSession->SetEditorStatus("Selected point light #" + std::to_string(id));
                        break;
                    case HierarchyEntityType::SpotLight:
                        selectedEditorObject = {SelectedEditorObjectType::SpotLight, id};
                        runtimeSession->SetEditorStatus("Selected spot light #" + std::to_string(id));
                        break;
                    default:
                        break;
                    }
                    Tracenf("[HIERARCHY] Selected entity: id=%u type=%d", id, static_cast<int>(type));
                };
                auto focusHierarchyEntity = [&](HierarchyEntityType type, std::uint32_t id) {
                    std::optional<WorldVec3> target;
                    if (type == HierarchyEntityType::WaterBody)
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
                    if ((type == HierarchyEntityType::WaterBody && selectedEditorObject.type == SelectedEditorObjectType::WaterBody && selectedEditorObject.id == id) ||
                        (type == HierarchyEntityType::PointLight && selectedEditorObject.type == SelectedEditorObjectType::PointLight && selectedEditorObject.id == id) ||
                        (type == HierarchyEntityType::SpotLight && selectedEditorObject.type == SelectedEditorObjectType::SpotLight && selectedEditorObject.id == id))
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
                        copy.name = (copy.name.empty() ? "Water Body" : copy.name) + " (Copy)";
                        copy.bboxMin[0] += 5.0f;
                        copy.bboxMax[0] += 5.0f;
                        copy.editorHidden = false;
                        editorWaterBodies.push_back(copy);
                        selectedEditorObject = {SelectedEditorObjectType::WaterBody, copy.id};
                        editorWaterBodiesDirty = true;
                        SceneManager::Instance().MarkDirty();
                        Tracenf("[HIERARCHY] Duplicated entity: original=%u new=%u", id, copy.id);
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
                        copy.name = (copy.name.empty() ? "Point Light" : copy.name) + " (Copy)";
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
                        copy.name = (copy.name.empty() ? "Spot Light" : copy.name) + " (Copy)";
                        copy.position[0] += 5.0f;
                        copy.editorHidden = false;
                        editorSpotLights.push_back(copy);
                        selectedEditorObject = {SelectedEditorObjectType::SpotLight, copy.id};
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
                    const std::string playSceneType = SceneManager::Instance().GetCurrentSceneType();
                    Tracenf("[EDIT-PLAY] Play Mode: starting scene = %s scene_type=%s",
                        editorPlay.playStartScenePath.empty() ? "<unsaved>" : editorPlay.playStartScenePath.c_str(),
                        playSceneType.c_str());
                    editorPlay.editorCameraSnapshot = cameraController.SaveSnapshot();
                    editorPlay.directGameplayDevCharacter = false;
                    selectedEditorObject = {};
                    editorObjectDragActive = false;
                    waterSculptStrokeActive = false;
                    editorWaterBodiesDirty = true;
                    terrain.SetWaterSculptBrush(false, 0.0f, 0.0f, 0.0f, true);
                    cameraController.SetEditorFlyMode(false);
                    runtimeSession->Start(SceneManager::Instance().GetCurrentScene());
                    editorRuntimeFlowActive = true;
                    SceneManager::Instance().ActivateCurrentSceneType();
                    if ((playSceneType == "world" || playSceneType == "gameplay") && !hasOwnRuntimeCharacter())
                        injectDirectGameplayDevCharacter();
                    editorPlay.state.frameCount = 0;
                    editorPlay.state.elapsedSeconds = 0.0;
                    editorPlay.appliedMode = editorPlay.state.mode;
                    Tracen("[EDIT-PLAY-2] Runtime UI/scene flow enabled for Play mode");
                    Tracen("[EDIT-PLAY] Play Mode active");
                }
                else if (editorPlay.appliedMode != EditorPlayMode::Edit &&
                    editorPlay.state.mode == EditorPlayMode::Edit)
                {
                    Tracenf("[EDIT-PLAY] Exiting Play Mode (after %.1fs, %d frames)",
                        editorPlay.state.elapsedSeconds,
                        editorPlay.state.frameCount);
                    editorRuntimeFlowActive = false;
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
                    editorPlay.directGameplayDevCharacter = false;
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
                    commands.addPointLight = false;
                    commands.addSpotLight = false;
                    commands.deleteSelectedLight = false;
                    commands.deleteSelectedWaterBody = false;
                    commands.selectedLightChanged = false;
                    commands.selectedWaterBodyChanged = false;
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
                    selectHierarchyEntity(commands.hierarchyEntityType, commands.hierarchyEntityId);
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
                if (commands.addWaterBody)
                {
                    const WorldVec3 spawn = spawnAtCameraCenter();
                    WaterBody body{};
                    body.id = nextEditorWaterBodyId++;
                    body.name = "Water_" + std::to_string(body.id);
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
                        light.name = "Point Light " + std::to_string(light.id);
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
                        light.name = "Spot Light " + std::to_string(light.id);
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
                editorImGui.SetHierarchySceneState(editorWaterBodies, editorPointLights, editorSpotLights);

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
                if (warriorOk)
                {
                    warrior.SetLightingState(lightingState);
                }
                if (commands.paletteSlotChanged)
                {
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
            bool frameRmlUiRenderCalled = false;
            bool frameImGuiRenderCalled = false;
            bool frameSceneRenderCalled = false;
            size_t frameSceneEntityCount = 0;
#if defined(IXTREEME_WITH_EDITOR)
            editorImGui.SetEditorPlayModeState(editorPlay.state);
            editorImGui.BeginFrame(runtimeSession->IsMapEditorOpen());
#endif
            const bool isInWorld = runtimeSession->IsInWorld();
            std::vector<WorldRenderEntity> entities;
            WorldCamera camera{};

            if (isInWorld)
            {
                entities = frameEntities;
                frameSceneEntityCount = entities.size();
                camera = hasFrameCamera ? frameCamera : cameraController.BuildCamera(renderSize.width, renderSize.height, {});

                if (warriorOk)
                {
                    uint32_t skinSlot = 0;
                    for (const auto& entity : entities)
                    {
                        if (skinSlot >= WarriorRenderer::MaxSkinSlots())
                            break;
                        warrior.SkinInstance(device,
                            skinSlot,
                            ToWarriorMotion(entity.moveState),
                            static_cast<float>(seconds));
                        ++skinSlot;
                    }
                    if (runtimeSession->IsMapEditorOpen())
                    {
                        const size_t editorVisualRenderCount =
                            editorPointLights.size() + editorSpotLights.size();
                        for (size_t visualIndex = 0; visualIndex < editorVisualRenderCount; ++visualIndex)
                        {
                            if (skinSlot >= WarriorRenderer::MaxSkinSlots())
                                break;
                            warrior.SkinInstance(device,
                                skinSlot,
                                WarriorRenderer::MotionState::Idle,
                                static_cast<float>(seconds));
                            ++skinSlot;
                        }
                    }
                }
            }
            else if (runtimeSession->IsLobbyActive() && warriorOk)
            {
                warrior.Skin(device, seconds);
            }

            if (isInWorld && terrainOk && hasFrameCamera)
                terrain.RenderSunShadowMap(device, frameCamera);
            if (isInWorld && terrainOk && hasFrameCamera)
            {
                terrain.RenderWaterReflection(device,
                    frameCamera,
                    seconds,
                    [&](const WorldCamera& mirrorCamera,
                        VkExtent2D reflectionExtent,
                        VkRenderPass reflectionRenderPass,
                        float waterLevelY)
                    {
                        if (!warriorOk)
                            return;

                        uint32_t skinSlot = 0;
                        for (const auto& entity : entities)
                        {
                            if (skinSlot >= WarriorRenderer::MaxSkinSlots())
                                break;
                            auto position = ServerMetersToDisplay(entity.position);
                            position.y += warrior.GroundOffsetY();
                            const std::array<float, 4> tint = entity.mobTypeId == 0
                                ? std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f}
                                : (entity.mobTypeId == 1
                                      ? std::array<float, 4>{1.35f, 0.55f, 0.55f, 1.0f}
                                      : std::array<float, 4>{0.65f, 0.95f, 1.35f, 1.0f});
                            warrior.RenderInWorldReflection(device,
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
                                if (skinSlot >= WarriorRenderer::MaxSkinSlots())
                                    break;
                                WorldVec3 position{light.position[0], light.position[1] + warrior.GroundOffsetY(), light.position[2]};
                                const bool selected =
                                    selectedEditorObject.type == SelectedEditorObjectType::PointLight &&
                                    selectedEditorObject.id == light.id;
                                warrior.RenderInWorldReflection(device,
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
                                if (skinSlot >= WarriorRenderer::MaxSkinSlots())
                                    break;
                                WorldVec3 position{light.position[0], light.position[1] + warrior.GroundOffsetY(), light.position[2]};
                                const bool selected =
                                    selectedEditorObject.type == SelectedEditorObjectType::SpotLight &&
                                    selectedEditorObject.id == light.id;
                                warrior.RenderInWorldReflection(device,
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
                device.BeginSwapchainRenderPass();

            std::vector<NameplateRenderer::Nameplate> plates;
            if (isInWorld)
            {
                if (terrainOk)
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
                    const float terrainY = terrainOk ? terrain.SampleHeight(position) : position.y;
                    const float groundOffsetY = warriorOk ? warrior.GroundOffsetY() : 0.0f;
                    if (!loggedTerrainAlignment && terrainOk)
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
                    if (warriorOk && skinSlot < WarriorRenderer::MaxSkinSlots())
                    {
                        position.y += groundOffsetY;
                        const std::array<float, 4> tint = entity.mobTypeId == 0
                            ? std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f}
                            : (entity.mobTypeId == 1
                                  ? std::array<float, 4>{1.35f, 0.55f, 0.55f, 1.0f}
                                  : std::array<float, 4>{0.65f, 0.95f, 1.35f, 1.0f});
                        warrior.RenderInWorld(device,
                            seconds,
                            camera,
                            position,
                            HeadingFromQuantized(entity.heading),
                            skinSlot,
                            tint);
                    }
                    plates.push_back(NameplateRenderer::Nameplate{
                        WorldAdd(position, {0.0f, 2.2f, 0.0f}),
                        entity.name,
                        entity.level,
                        entity.mobTypeId == 0 ? 0 : 1,
                        entity.hpCurrent,
                        entity.hpMax,
                        entity.hpDisplayed,
                        entity.netId == selectedTargetNetId});
                    ++skinSlot;
                }
                if (warriorOk && runtimeSession->IsMapEditorOpen())
                {
                    for (const auto& light : editorPointLights)
                    {
                        if (editorPlay.state.mode == EditorPlayMode::Edit && light.editorHidden)
                            continue;
                        if (skinSlot >= WarriorRenderer::MaxSkinSlots())
                            break;
                        WorldVec3 position{light.position[0], light.position[1] + warrior.GroundOffsetY(), light.position[2]};
                        const bool selected =
                            selectedEditorObject.type == SelectedEditorObjectType::PointLight &&
                            selectedEditorObject.id == light.id;
                        warrior.RenderInWorld(device,
                            seconds,
                            camera,
                            position,
                            0.0f,
                            skinSlot,
                            selected
                                ? std::array<float, 4>{2.0f, 1.55f, 0.25f, 1.0f}
                                : std::array<float, 4>{1.6f, 1.05f, 0.35f, 1.0f});
                        plates.push_back(NameplateRenderer::Nameplate{
                            WorldAdd({light.position[0], light.position[1], light.position[2]}, {0.0f, 1.4f, 0.0f}),
                            light.name.empty() ? "Point Light " + std::to_string(light.id) : light.name,
                            1,
                            selected ? 2u : 0u,
                            1.0f,
                            1.0f,
                            1.0f,
                            selected});
                        ++skinSlot;
                    }
                    for (const auto& light : editorSpotLights)
                    {
                        if (editorPlay.state.mode == EditorPlayMode::Edit && light.editorHidden)
                            continue;
                        if (skinSlot >= WarriorRenderer::MaxSkinSlots())
                            break;
                        WorldVec3 position{light.position[0], light.position[1] + warrior.GroundOffsetY(), light.position[2]};
                        const bool selected =
                            selectedEditorObject.type == SelectedEditorObjectType::SpotLight &&
                            selectedEditorObject.id == light.id;
                        warrior.RenderInWorld(device,
                            seconds,
                            camera,
                            position,
                            light.rotation[1],
                            skinSlot,
                            selected
                                ? std::array<float, 4>{0.35f, 1.7f, 2.0f, 1.0f}
                                : std::array<float, 4>{0.35f, 1.25f, 1.65f, 1.0f});
                        plates.push_back(NameplateRenderer::Nameplate{
                            WorldAdd({light.position[0], light.position[1], light.position[2]}, {0.0f, 1.4f, 0.0f}),
                            light.name.empty() ? "Spot Light " + std::to_string(light.id) : light.name,
                            1,
                            selected ? 2u : 0u,
                            1.0f,
                            1.0f,
                            1.0f,
                            selected});
                        ++skinSlot;
                    }
                }
                if (!useOffscreenScene && terrainOk)
                {
                    terrain.RenderWater(device, camera, seconds);
                }
                if (!useOffscreenScene && nameplatesOk)
                    nameplates.Render(device, camera, plates);
            }
            else if (runtimeSession->IsLobbyActive() && warriorOk)
            {
                frameSceneRenderCalled = true;
                frameSceneEntityCount = 1;
                warrior.Render(device, seconds);
            }

            if (useOffscreenScene)
            {
                offscreenScene.EndMainPass(device);
                if (isInWorld && terrainOk)
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
                device.BeginSwapchainRenderPass();
                offscreenScene.RenderComposite(device);
                if (isInWorld && nameplatesOk)
                    nameplates.Render(device, camera, plates);
            }
            frameRmlUiRenderCalled = true;
            rmlUi.Render(device);
#if defined(IXTREEME_WITH_EDITOR)
            frameImGuiRenderCalled = true;
            editorImGui.Render(device);
#else
            frameImGuiRenderCalled = false;
#endif
            const uint64_t frameNumber = device.GetFrameNumber();
            if (frameNumber < 3 || (frameNumber % 60u) == 0u)
            {
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
    if (nameplatesOk)
        nameplates.Destroy();
    if (offscreenSceneOk)
        offscreenScene.Destroy();
    if (terrainOk)
        terrain.Destroy();
    if (warriorOk)
        warrior.Destroy();
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
    const auto debugSpawnOverride = ParseDebugSpawnOverride(GetCommandLineA());

    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        ShowFatal("Failed to initialize WinSock.");
        return 1;
    }

    NativeWindow_Win32 window;
    if (!window.Create(instance, "AURIGA GLOBAL — Editor", 1280, 720))
    {
        ShowFatal("Failed to create Win32 window.");
        WSACleanup();
        return 1;
    }

    client::asset::FileAssetReader assets(ExecutableDirectory());
    const int result = RunGame(window, assets, debugSpawnOverride);
    window.Destroy();
    WSACleanup();
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

