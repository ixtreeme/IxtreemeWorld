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
#include "GameClientLayer.h"
#include "OffscreenSceneRenderer.h"
#include "RmlUiLayer.h"
#include "TerrainRenderer.h"
#include "VulkanDevice.h"
#include "WarriorRenderer.h"
#include "Debug.h"
#include "asset/IAssetReader.h"
#include "network/ClientSession.h"

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

void MergeMapEditorCommands(MapEditorCommands& target, const MapEditorCommands& source)
{
    target.save = target.save || source.save;
    target.reload = target.reload || source.reload;
    target.undo = target.undo || source.undo;
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
#else
    Tracen("[BUILD] Editor: DISABLED");
#endif

    GameClientLayer gameClient;
    VkExtent2D renderSize = device.GetSwapchainExtent();
    if (!gameClient.Create(device, assets, renderSize.width, renderSize.height))
    {
        ShowFatal("Failed to create gameClient layer. See debug output/stderr.");
        device.Destroy();
        return 1;
    }
    client::net::ClientSession clientSession(gameClient);
    clientSession.SetDebugSpawnOverride(debugSpawnOverride);
    gameClient.SetClientSession(&clientSession);

    RmlUiLayer rmlUi;
    if (!rmlUi.Create(device, assets, renderSize.width, renderSize.height))
    {
        ShowFatal("Failed to create RmlUi layer. See debug output/stderr.");
        gameClient.Destroy();
        device.Destroy();
        return 1;
    }
    rmlUi.SetLoginSubmitCallback([&gameClient](const std::string& username,
                                            const std::string& password,
                                            bool remember) {
        gameClient.SubmitLogin(username, password, remember);
    });
    gameClient.SetLoginCallbacks(
        [&rmlUi]() {
            rmlUi.SetLoginError("");
            rmlUi.HideLogin();
        },
        [&gameClient, &rmlUi](const std::string& message) {
            if (!gameClient.IsLobbyActive() && !gameClient.IsInWorld())
                rmlUi.ShowLogin();
            rmlUi.SetLoginError(message);
        });
    rmlUi.SetLobbyCallbacks(
        [&gameClient](std::uint64_t characterId) {
            gameClient.EnterWorldWithCharacter(characterId);
        },
        [&rmlUi]() {
            rmlUi.ShowCharacterCreation();
        },
        [&rmlUi](std::uint64_t) {
            rmlUi.SetLobbyStatus("Character delete is not available yet");
        },
        [&gameClient, &rmlUi]() {
            gameClient.LogoutToLogin();
            rmlUi.HideLobby();
            rmlUi.ShowLogin();
        });
    gameClient.SetLobbyCallbacks(
        [&rmlUi]() {
            rmlUi.ShowLobby();
        },
        [&rmlUi](const std::vector<client::net::CharacterListItem>& characters) {
            rmlUi.SetLobbyCharacters(characters);
        },
        [&rmlUi](const std::string& message) {
            rmlUi.SetLobbyStatus(message);
        },
        [&rmlUi]() {
            rmlUi.HideLobby();
            rmlUi.ShowHud();
        });
    rmlUi.SetInGameMenuCallbacks(
        []() {},
        [&gameClient, &rmlUi]() {
            gameClient.LogoutToLogin();
            rmlUi.HideHud();
            rmlUi.HideInventory();
            rmlUi.HideSettings();
            rmlUi.HideInGameMenu();
            rmlUi.ShowLogin();
        },
        [&window]() {
            window.RequestClose();
        });

    EditorImGui editorImGui;
#if defined(IXTREEME_WITH_EDITOR)
#if defined(_WIN32)
    NativeWindow_Win32* win32Window = dynamic_cast<NativeWindow_Win32*>(&window);
    if (!win32Window || !editorImGui.Create(device, win32Window->GetHwnd()))
    {
        ShowFatal("Failed to create ImGui editor layer. See debug output/stderr.");
        gameClient.Destroy();
        device.Destroy();
        return 1;
    }
    win32Window->SetMessageCallback([&editorImGui](HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result)
    {
        return editorImGui.HandleWin32Message(hwnd, message, wParam, lParam, result);
    });
#else
    if (!editorImGui.Create(device, nullptr))
    {
        ShowFatal("Failed to create ImGui editor layer. See debug output/stderr.");
        gameClient.Destroy();
        device.Destroy();
        return 1;
    }
#endif
    editorImGui.SetMapEditorSettings(gameClient.GetMapEditorSettings());
    editorImGui.SetLightingState(gameClient.GetLightingState());
    if (auto assetRoot = assets.RootPath())
        editorImGui.InitializeAssetLibrary(*assetRoot);
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
        gameClient.InitializeAssetLibrary("assets/Maps/test_zone", terrain.GetPaletteSlots());
#if defined(IXTREEME_WITH_EDITOR)
        editorImGui.SetPaletteSlots(gameClient.GetPaletteSlots());
        editorImGui.SetWaterMaterials(editorImGui.GetWaterMaterialsSnapshot());
#endif
        if (!terrain.ApplyPaletteSlots(device, gameClient.GetPaletteSlots()))
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

    gameClient.SetQuitCallback([&window]()
    {
        window.RequestClose();
    });

    MovementInputState movement;
    CameraController cameraController;
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
    bool waterSculptStrokeActive = false;
    std::uint32_t waterSculptStrokeBodyId = 0;
    std::uint32_t waterSculptStrokeModifiedCells = 0;
    bool waterSculptMeshRegenPending = false;

    window.SetInputCallback([&gameClient,
                             &rmlUi,
                             &editorImGui,
                             &movement,
                             &cameraController,
                             &terrain,
                             &terrainOk,
                             &clientSession,
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
                             &renderSize](const InputEvent& event)
    {
        if (editorImGui.WantsInputCapture(event))
        {
            movement.Clear();
            return;
        }

        if (gameClient.IsInWorld() && event.type == InputEvent::KeyDown && event.key == Key_Escape)
        {
            if (rmlUi.IsSettingsVisible())
                rmlUi.HideSettings();
            else
                rmlUi.ToggleInGameMenu();
            movement.Clear();
            return;
        }
        if (gameClient.IsInWorld() && event.type == InputEvent::KeyDown && event.key == Key_I)
        {
            rmlUi.ToggleInventory();
            movement.Clear();
            return;
        }

        if (rmlUi.OnInput(event))
        {
            movement.Clear();
            return;
        }

        const bool editorTextInputFocused = gameClient.IsMapEditorOpen() && gameClient.IsTextInputFocused();
        if (editorTextInputFocused)
            movement.Clear();
        else
            movement.Apply(event);

        if (gameClient.IsInWorld() && event.type == InputEvent::MouseDown && event.button == MouseButton_Left &&
            hasLastPickCamera && !gameClient.IsMapEditorOpen())
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
        if (gameClient.IsInWorld() && !gameClient.IsMapEditorOpen() &&
            event.type == InputEvent::KeyDown && event.key == Key_F)
        {
            if (selectedTargetNetId != 0)
                clientSession.SendAttackTarget(selectedTargetNetId);
            return;
        }
#if defined(IXTREEME_WITH_EDITOR)
        if (event.type == InputEvent::KeyDown && event.key == Key_F2 && gameClient.IsMapEditorOpen())
        {
            if (gameClient.OnInput(event))
                return;
        }
        if (event.type == InputEvent::KeyDown && event.key == Key_F2 && terrainOk)
        {
            terrain.ToggleWalkabilityDebug();
            if (!terrain.IsWalkabilityDebugEnabled() && gameClient.IsMapEditorOpen())
            {
                gameClient.ToggleMapEditor();
                terrain.SetMapEditorOpen(false);
                cameraController.SetEditorFlyMode(false);
            }
            return;
        }
        if (event.type == InputEvent::KeyDown && event.key == Key_F4 && terrainOk &&
            gameClient.IsInWorld())
        {
            gameClient.ToggleMapEditor();
            gameClient.ClearKeyboardFocus();
            terrain.SetMapEditorOpen(gameClient.IsMapEditorOpen());
            cameraController.SetEditorFlyMode(gameClient.IsMapEditorOpen());
            if (!gameClient.IsMapEditorOpen())
            {
                selectedEditorObject = {};
                editorObjectDragActive = false;
                gameClient.SetEditorStatus("Editor closed");
            }
            else
            {
                gameClient.SetEditorStatus("Editor fly camera active: RMB look, WASD move, Space/Ctrl up/down");
            }
            return;
        }

        if (terrainOk && gameClient.IsMapEditorOpen())
        {
            if (editorTextInputFocused &&
                (event.type == InputEvent::KeyDown ||
                 event.type == InputEvent::KeyUp ||
                 event.type == InputEvent::Char))
            {
                gameClient.OnInput(event);
                if (event.type == InputEvent::KeyDown && event.key == Key_Enter)
                    gameClient.ClearKeyboardFocus();
                return;
            }

            if (event.type == InputEvent::KeyDown || event.type == InputEvent::KeyUp)
            {
                if (event.type == InputEvent::KeyDown)
                {
                    if (event.key == Key_W)
                    {
                        editorGizmoMode = EditorGizmoMode::Translate;
                        gameClient.SetEditorStatus("Gizmo: translate");
                    }
                    else if (event.key == Key_E)
                    {
                        editorGizmoMode = EditorGizmoMode::Rotate;
                        gameClient.SetEditorStatus("Gizmo: rotate");
                    }
                    else if (event.key == Key_R)
                    {
                        editorGizmoMode = EditorGizmoMode::Scale;
                        gameClient.SetEditorStatus("Gizmo: scale");
                    }
                    else if (event.key == Key_Delete && selectedEditorObject.type == SelectedEditorObjectType::WaterBody)
                    {
                        editorWaterBodies.erase(std::remove_if(editorWaterBodies.begin(), editorWaterBodies.end(),
                            [&](const WaterBody& body) { return body.id == selectedEditorObject.id; }), editorWaterBodies.end());
                        gameClient.SetEditorStatus("Deleted water body #" + std::to_string(selectedEditorObject.id));
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
                consumedByEditorUi = gameClient.OnInput(event);
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
                            gameClient.SetEditorStatus("Water sculpt stroke: " +
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
                        gameClient.ClearKeyboardFocus();

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
                            gameClient.SetEditorStatus("Selected point light #" + std::to_string(*pointId));
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
                            gameClient.SetEditorStatus("Selected spot light #" + std::to_string(*spotId));
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
                            gameClient.SetEditorStatus("Selected water body #" + std::to_string(*waterId));
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
                if (!consumedByEditorUi && gameClient.IsInWorld() && cameraMouse)
                    cameraController.HandleInput(event);
                return;
            }
        }

        if (terrainOk && terrain.HandleEditorInput(event))
            return;
#endif
        if (gameClient.IsInWorld() && cameraController.HandleInput(event))
            return;

        if (!gameClient.OnInput(event))
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
                gameClient.OnRenderPassChanged(device);
                rmlUi.OnRenderPassChanged(device);
#if defined(IXTREEME_WITH_EDITOR)
                editorImGui.OnRenderPassChanged(device);
#endif
                renderSize = device.GetSwapchainExtent();
                gameClient.Resize(renderSize.width, renderSize.height);
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
            gameClient.ImportDroppedFiles(dropped);
            editorImGui.RefreshAssetLibrary();
        }
#endif

        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(now - startTime).count();
        const double deltaSeconds = seconds - previousSeconds;
        previousSeconds = seconds;
        cameraController.Update(deltaSeconds, movement);
        clientSession.Update();
        clientSession.SendMoveInput(
            movement.DirectionAngle(cameraController.MovementYaw()),
            cameraController.IsFlyMode() ? client::net::MoveState::Idle : movement.State());
        gameClient.Update(seconds);
        rmlUi.Update();

        std::vector<WorldRenderEntity> frameEntities;
        WorldCamera frameCamera{};
        bool hasFrameCamera = false;
        if (gameClient.IsInWorld())
        {
            frameEntities = gameClient.GetWorldEntities();
            WorldVec3 cameraTarget{};
            bool hasOwn = false;
            for (const auto& entity : frameEntities)
            {
                if (entity.netId == gameClient.GetOwnNetId())
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
                if (entity.netId == gameClient.GetOwnNetId())
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
            rmlUi.UpdateHud(hudData);

#if defined(IXTREEME_WITH_EDITOR)
            if (terrainOk)
            {
                terrain.SetMapEditorOpen(gameClient.IsMapEditorOpen());
                const MapEditorSettings editorSettings = editorImGui.GetMapEditorSettings();
                terrain.SetMapEditorSettings(editorSettings);
                MapEditorCommands commands = gameClient.ConsumeMapEditorCommands();
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
                    gameClient.SetEditorStatus("Water body spawned: id=" + std::to_string(body.id) +
                        " name=" + body.name);
                    Tracenf("[EDITOR-3D-SPAWN] Spawn at cursor: type=water position=(%.2f,%.2f,%.2f)",
                        spawn.x, spawn.y, spawn.z);
                }
                if (commands.addPointLight)
                {
                    if (editorPointLights.size() >= kMaxDynamicPointLights)
                    {
                        gameClient.SetEditorStatus("Maximum point lights reached (16)");
                    }
                    else
                    {
                        PointLight light{};
                        light.id = nextEditorLightId++;
                        const WorldVec3 spawn = spawnAtCameraCenter();
                        light.position[0] = spawn.x;
                        light.position[1] = spawn.y + 1.8f;
                        light.position[2] = spawn.z;
                        editorPointLights.push_back(light);
                        selectedEditorObject = {SelectedEditorObjectType::PointLight, light.id};
                        editorGizmoMode = EditorGizmoMode::Translate;
                        gameClient.SetEditorStatus("Added point light #" + std::to_string(light.id));
                        Tracenf("[EDITOR-3D-SPAWN] Spawn at cursor: type=point_light position=(%.2f,%.2f,%.2f)",
                            spawn.x, spawn.y, spawn.z);
                    }
                }
                if (commands.addSpotLight)
                {
                    if (editorSpotLights.size() >= kMaxDynamicSpotLights)
                    {
                        gameClient.SetEditorStatus("Maximum spot lights reached (16)");
                    }
                    else
                    {
                        SpotLight light{};
                        light.id = nextEditorLightId++;
                        const WorldVec3 spawn = spawnAtCameraCenter();
                        light.position[0] = spawn.x;
                        light.position[1] = spawn.y + 4.0f;
                        light.position[2] = spawn.z;
                        light.rotation[0] = -1.5708f;
                        editorSpotLights.push_back(light);
                        selectedEditorObject = {SelectedEditorObjectType::SpotLight, light.id};
                        editorGizmoMode = EditorGizmoMode::Translate;
                        gameClient.SetEditorStatus("Added spot light #" + std::to_string(light.id));
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
                        }
                    }
                }
                if (commands.deleteSelectedLight)
                {
                    if (selectedEditorObject.type == SelectedEditorObjectType::PointLight)
                    {
                        editorPointLights.erase(std::remove_if(editorPointLights.begin(), editorPointLights.end(),
                            [&](const PointLight& light) { return light.id == selectedEditorObject.id; }), editorPointLights.end());
                        gameClient.SetEditorStatus("Deleted point light #" + std::to_string(selectedEditorObject.id));
                        selectedEditorObject = {};
                    }
                    else if (selectedEditorObject.type == SelectedEditorObjectType::SpotLight)
                    {
                        editorSpotLights.erase(std::remove_if(editorSpotLights.begin(), editorSpotLights.end(),
                            [&](const SpotLight& light) { return light.id == selectedEditorObject.id; }), editorSpotLights.end());
                        gameClient.SetEditorStatus("Deleted spot light #" + std::to_string(selectedEditorObject.id));
                        selectedEditorObject = {};
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
                    }
                }
                if (commands.openSelectedWaterMaterialEditor)
                {
                    const std::string materialId = commands.selectedWaterBody.materialId.empty()
                        ? std::string("watermat_Default_Water")
                        : commands.selectedWaterBody.materialId;
                    if (editorImGui.OpenWaterMaterialEditor(materialId))
                        gameClient.SetEditorStatus("Editing water material: " + materialId);
                }
                if (commands.waterMaterialDeleted)
                {
                    for (WaterBody& body : editorWaterBodies)
                    {
                        if (body.materialId == commands.deletedWaterMaterialId)
                        {
                            body.materialId = "watermat_Default_Water";
                            editorWaterBodiesDirty = true;
                        }
                    }
                    if (selectedEditorObject.type == SelectedEditorObjectType::WaterBody)
                    {
                        auto selectedIt = std::find_if(editorWaterBodies.begin(), editorWaterBodies.end(),
                            [&](const WaterBody& body) { return body.id == selectedEditorObject.id; });
                        if (selectedIt != editorWaterBodies.end() &&
                            selectedIt->materialId == "watermat_Default_Water")
                        {
                            gameClient.SetEditorStatus("Deleted material replaced with default on selected water body");
                        }
                    }
                }
                if (commands.deleteSelectedWaterBody &&
                    selectedEditorObject.type == SelectedEditorObjectType::WaterBody)
                {
                    editorWaterBodies.erase(std::remove_if(editorWaterBodies.begin(), editorWaterBodies.end(),
                        [&](const WaterBody& body) { return body.id == selectedEditorObject.id; }), editorWaterBodies.end());
                    gameClient.SetEditorStatus("Water body deleted: id=" + std::to_string(selectedEditorObject.id));
                    selectedEditorObject = {};
                    editorWaterBodiesDirty = true;
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
                gameClient.SetWaterBodyEditorState(waterBodyState);
                editorImGui.SetWaterBodyEditorState(waterBodyState);

                LightingState lightingState = editorImGui.GetLightingState();
                lightingState.numPointLights = 0;
                for (const PointLight& light : editorPointLights)
                {
                    if (lightingState.numPointLights >= kMaxDynamicPointLights)
                        break;
                    lightingState.pointLights[lightingState.numPointLights++] = light;
                }
                lightingState.numSpotLights = 0;
                for (const SpotLight& light : editorSpotLights)
                {
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
                    terrain.SetWaterBodies(device, editorWaterBodies);
                    editorWaterBodies = terrain.GetWaterBodies();
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
                    }
                }
                if (commands.save)
                    terrain.RequestEditorSave();
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
            }
#endif
        }

        device.BeginFrame();
        if (device.IsFrameActive())
        {
#if defined(IXTREEME_WITH_EDITOR)
            editorImGui.BeginFrame(gameClient.IsMapEditorOpen());
#endif
            const bool isInWorld = gameClient.IsInWorld();
            std::vector<WorldRenderEntity> entities;
            WorldCamera camera{};

            if (isInWorld)
            {
                entities = frameEntities;
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
                    if (gameClient.IsMapEditorOpen())
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
            else if (gameClient.IsLobbyActive() && warriorOk)
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

                        if (gameClient.IsMapEditorOpen())
                        {
                            for (const auto& light : editorPointLights)
                            {
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
                    terrain.Render(device, camera);

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
                if (warriorOk && gameClient.IsMapEditorOpen())
                {
                    for (const auto& light : editorPointLights)
                    {
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
                            "Point Light " + std::to_string(light.id),
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
                            "Spot Light " + std::to_string(light.id),
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
            else if (gameClient.IsLobbyActive() && warriorOk)
                warrior.Render(device, seconds);

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
            rmlUi.Render(device);
#if defined(IXTREEME_WITH_EDITOR)
            editorImGui.Render(device);
#endif
        }
        device.EndFrame();
    }

    device.WaitIdle();
    clientSession.Disconnect();
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
    gameClient.Destroy();
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
    if (!window.Create(instance, "Standalone Vulkan Clear - gameClient Overlay", 1280, 720))
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

