#if defined(_WIN32)
#include <winsock2.h>
#include <windows.h>
#endif

#include "NameplateRenderer.h"
#include "NativeWindow.h"
#if defined(_WIN32)
#include "NativeWindow_Win32.h"
#endif
#if defined(__ANDROID__)
#include "NativeWindow_Android.h"
#endif
#include "NoesisLayer.h"
#include "OffscreenSceneRenderer.h"
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
    std::snprintf(buffer, sizeof(buffer), "Input not consumed by Noesis: %s\n",
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

enum class EditorGizmoMode
{
    Translate,
    Rotate,
    Scale
};

struct EditorTestMarker
{
    std::uint32_t id = 0;
    WorldVec3 position;
    WorldVec3 rotation;
    WorldVec3 scale = {1.0f, 1.0f, 1.0f};
};

enum class SelectedEditorObjectType
{
    None,
    Marker,
    PointLight,
    SpotLight
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

std::optional<std::uint32_t> PickEditorMarker(const std::vector<EditorTestMarker>& markers,
                                              const WorldCamera& camera,
                                              uint32_t width,
                                              uint32_t height,
                                              int mouseX,
                                              int mouseY)
{
    const WorldVec3 rayDir = ScreenRayDirection(camera, width, height, mouseX, mouseY);
    std::optional<std::uint32_t> bestId;
    float bestT = 1000000.0f;
    for (const EditorTestMarker& marker : markers)
    {
        const WorldVec3 toMarker = WorldSub(marker.position, camera.eye);
        const float t = WorldDot(toMarker, rayDir);
        if (t <= 0.0f || t >= bestT)
            continue;
        const WorldVec3 closest = WorldAdd(camera.eye, WorldScale(rayDir, t));
        const WorldVec3 delta = WorldSub(marker.position, closest);
        const float radius = std::max({marker.scale.x, marker.scale.y, marker.scale.z, 1.0f}) * 0.9f;
        if (WorldDot(delta, delta) <= radius * radius)
        {
            bestT = t;
            bestId = marker.id;
        }
    }
    return bestId;
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

    NoesisLayer noesis;
    VkExtent2D renderSize = device.GetSwapchainExtent();
    if (!noesis.Create(device, assets, renderSize.width, renderSize.height))
    {
        ShowFatal("Failed to create Noesis layer. See debug output/stderr.");
        device.Destroy();
        return 1;
    }
    client::net::ClientSession clientSession(noesis);
    clientSession.SetDebugSpawnOverride(debugSpawnOverride);
    noesis.SetClientSession(&clientSession);

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
        noesis.InitializeAssetLibrary("assets/Maps/test_zone", terrain.GetPaletteSlots());
        if (!terrain.ApplyPaletteSlots(device, noesis.GetPaletteSlots()))
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

    noesis.SetQuitCallback([&window]()
    {
        window.RequestClose();
    });

    MovementInputState movement;
    CameraController cameraController;
    std::vector<WorldRenderEntity> lastPickEntities;
    WorldCamera lastPickCamera{};
    bool hasLastPickCamera = false;
    std::uint32_t selectedTargetNetId = 0;
    std::vector<EditorTestMarker> editorMarkers;
    std::vector<PointLight> editorPointLights;
    std::vector<SpotLight> editorSpotLights;
    std::uint32_t nextEditorMarkerId = 1;
    std::uint32_t nextEditorLightId = 1;
    std::uint32_t selectedEditorMarkerId = 0;
    SelectedEditorObject selectedEditorObject;
    EditorGizmoMode editorGizmoMode = EditorGizmoMode::Translate;
    bool editorMarkerDragActive = false;
    int editorMarkerDragLastX = 0;
    int editorMarkerDragLastY = 0;

    window.SetInputCallback([&noesis,
                             &movement,
                             &cameraController,
                             &terrain,
                             &terrainOk,
                             &clientSession,
                             &lastPickEntities,
                             &lastPickCamera,
                             &hasLastPickCamera,
                             &selectedTargetNetId,
                             &editorMarkers,
                             &editorPointLights,
                             &editorSpotLights,
                             &selectedEditorMarkerId,
                             &selectedEditorObject,
                             &editorGizmoMode,
                             &editorMarkerDragActive,
                             &editorMarkerDragLastX,
                             &editorMarkerDragLastY,
                             &renderSize](const InputEvent& event)
    {
        const bool editorTextInputFocused = noesis.IsMapEditorOpen() && noesis.IsTextInputFocused();
        if (editorTextInputFocused)
            movement.Clear();
        else
            movement.Apply(event);

        if (noesis.IsInWorld() && event.type == InputEvent::MouseDown && event.button == MouseButton_Left &&
            hasLastPickCamera && !noesis.IsMapEditorOpen())
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
        if (noesis.IsInWorld() && !noesis.IsMapEditorOpen() &&
            event.type == InputEvent::KeyDown && event.key == Key_F)
        {
            if (selectedTargetNetId != 0)
                clientSession.SendAttackTarget(selectedTargetNetId);
            return;
        }
        if (event.type == InputEvent::KeyDown && event.key == Key_F2 && noesis.IsMapEditorOpen())
        {
            if (noesis.OnInput(event))
                return;
        }
        if (event.type == InputEvent::KeyDown && event.key == Key_F2 && terrainOk)
        {
            terrain.ToggleWalkabilityDebug();
            if (!terrain.IsWalkabilityDebugEnabled() && noesis.IsMapEditorOpen())
            {
                noesis.ToggleMapEditor();
                terrain.SetMapEditorOpen(false);
                cameraController.SetEditorFlyMode(false);
            }
            return;
        }
        if (event.type == InputEvent::KeyDown && event.key == Key_F4 && terrainOk &&
            noesis.IsInWorld())
        {
            noesis.ToggleMapEditor();
            noesis.ClearKeyboardFocus();
            terrain.SetMapEditorOpen(noesis.IsMapEditorOpen());
            cameraController.SetEditorFlyMode(noesis.IsMapEditorOpen());
            if (!noesis.IsMapEditorOpen())
            {
                selectedEditorMarkerId = 0;
                selectedEditorObject = {};
                editorMarkerDragActive = false;
                noesis.SetEditorStatus("Editor closed");
            }
            else
            {
                noesis.SetEditorStatus("Editor fly camera active: RMB look, WASD move, Space/Ctrl up/down");
            }
            return;
        }

        if (terrainOk && noesis.IsMapEditorOpen())
        {
            if (editorTextInputFocused &&
                (event.type == InputEvent::KeyDown ||
                 event.type == InputEvent::KeyUp ||
                 event.type == InputEvent::Char))
            {
                noesis.OnInput(event);
                if (event.type == InputEvent::KeyDown && event.key == Key_Enter)
                    noesis.ClearKeyboardFocus();
                return;
            }

            if (event.type == InputEvent::KeyDown || event.type == InputEvent::KeyUp)
            {
                if (event.type == InputEvent::KeyDown)
                {
                    if (event.key == Key_W)
                    {
                        editorGizmoMode = EditorGizmoMode::Translate;
                        noesis.SetEditorStatus("Gizmo: translate");
                    }
                    else if (event.key == Key_E)
                    {
                        editorGizmoMode = EditorGizmoMode::Rotate;
                        noesis.SetEditorStatus("Gizmo: rotate");
                    }
                    else if (event.key == Key_R)
                    {
                        editorGizmoMode = EditorGizmoMode::Scale;
                        noesis.SetEditorStatus("Gizmo: scale");
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
                consumedByEditorUi = noesis.OnInput(event);
                if (event.type == InputEvent::MouseUp)
                {
                    if (event.button == MouseButton_Left)
                        editorMarkerDragActive = false;
                    terrain.HandleEditorInput(event);
                }
                else if (!consumedByEditorUi)
                {
                    if (event.type == InputEvent::MouseDown)
                        noesis.ClearKeyboardFocus();

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
                            selectedEditorMarkerId = 0;
                            editorMarkerDragActive = true;
                            editorMarkerDragLastX = event.x;
                            editorMarkerDragLastY = event.y;
                            noesis.SetEditorStatus("Selected point light #" + std::to_string(*pointId));
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
                            selectedEditorMarkerId = 0;
                            editorMarkerDragActive = true;
                            editorMarkerDragLastX = event.x;
                            editorMarkerDragLastY = event.y;
                            noesis.SetEditorStatus("Selected spot light #" + std::to_string(*spotId));
                            return;
                        }
                        if (auto markerId = PickEditorMarker(editorMarkers,
                                lastPickCamera,
                                renderSize.width,
                                renderSize.height,
                                event.x,
                                event.y))
                        {
                            selectedEditorMarkerId = *markerId;
                            selectedEditorObject = {SelectedEditorObjectType::Marker, *markerId};
                            editorMarkerDragActive = true;
                            editorMarkerDragLastX = event.x;
                            editorMarkerDragLastY = event.y;
                            noesis.SetEditorStatus("Selected marker #" + std::to_string(selectedEditorMarkerId));
                            return;
                        }
                    }
                    if (event.type == InputEvent::MouseMove && editorMarkerDragActive && selectedEditorMarkerId != 0)
                    {
                        const int dx = event.x - editorMarkerDragLastX;
                        const int dy = event.y - editorMarkerDragLastY;
                        editorMarkerDragLastX = event.x;
                        editorMarkerDragLastY = event.y;
                        auto markerIt = std::find_if(editorMarkers.begin(), editorMarkers.end(),
                            [selectedEditorMarkerId](const EditorTestMarker& marker) {
                                return marker.id == selectedEditorMarkerId;
                            });
                        if (markerIt != editorMarkers.end())
                        {
                            const float scale = 0.025f * std::max(1.0f, std::sqrt(WorldDot(WorldSub(markerIt->position, lastPickCamera.eye),
                                WorldSub(markerIt->position, lastPickCamera.eye))));
                            if (editorGizmoMode == EditorGizmoMode::Translate)
                            {
                                markerIt->position = WorldAdd(markerIt->position,
                                    WorldAdd(WorldScale(CameraRight(lastPickCamera), static_cast<float>(dx) * scale),
                                             WorldScale(CameraUp(lastPickCamera), static_cast<float>(-dy) * scale)));
                            }
                            else if (editorGizmoMode == EditorGizmoMode::Rotate)
                            {
                                markerIt->rotation.y += static_cast<float>(dx) * 0.01f;
                                markerIt->rotation.x += static_cast<float>(dy) * 0.01f;
                            }
                            else
                            {
                                const float delta = static_cast<float>(dx - dy) * 0.01f;
                                markerIt->scale.x = std::max(0.1f, markerIt->scale.x + delta);
                                markerIt->scale.y = std::max(0.1f, markerIt->scale.y + delta);
                                markerIt->scale.z = std::max(0.1f, markerIt->scale.z + delta);
                            }
                            return;
                        }
                    }
                    if (event.type == InputEvent::MouseMove && editorMarkerDragActive &&
                        selectedEditorObject.type != SelectedEditorObjectType::None &&
                        selectedEditorObject.type != SelectedEditorObjectType::Marker)
                    {
                        const int dx = event.x - editorMarkerDragLastX;
                        const int dy = event.y - editorMarkerDragLastY;
                        editorMarkerDragLastX = event.x;
                        editorMarkerDragLastY = event.y;
                        auto moveLight = [&](auto& light) {
                            WorldVec3 position{light.position[0], light.position[1], light.position[2]};
                            const float scale = 0.025f * std::max(1.0f, std::sqrt(WorldDot(WorldSub(position, lastPickCamera.eye),
                                WorldSub(position, lastPickCamera.eye))));
                            if (editorGizmoMode == EditorGizmoMode::Translate)
                            {
                                position = WorldAdd(position,
                                    WorldAdd(WorldScale(CameraRight(lastPickCamera), static_cast<float>(dx) * scale),
                                             WorldScale(CameraUp(lastPickCamera), static_cast<float>(-dy) * scale)));
                                light.position[0] = position.x;
                                light.position[1] = position.y;
                                light.position[2] = position.z;
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
                    terrain.HandleEditorInput(event);
                }

                const bool cameraMouse =
                    event.type == InputEvent::MouseMove ||
                    ((event.type == InputEvent::MouseDown || event.type == InputEvent::MouseUp) &&
                     event.button == MouseButton_Right);
                if (!consumedByEditorUi && noesis.IsInWorld() && cameraMouse)
                    cameraController.HandleInput(event);
                return;
            }
        }

        if (terrainOk && terrain.HandleEditorInput(event))
            return;
        if (noesis.IsInWorld() && cameraController.HandleInput(event))
            return;

        if (!noesis.OnInput(event))
        {
            // TODO: forward unconsumed events to the game/3D scene input path.
            //LogUnhandledInput(event);
        }
    });

    window.SetFileDropCallback([&noesis](const std::vector<std::string>& paths)
    {
        noesis.ImportDroppedFiles(paths);
    });

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
                noesis.OnRenderPassChanged(device);
                renderSize = device.GetSwapchainExtent();
                noesis.Resize(renderSize.width, renderSize.height);
            }
            else
            {
                Tracen("[MAIN] device.Resize() returned false (unchanged), skipping pipeline recreate");
            }
        }

        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(now - startTime).count();
        const double deltaSeconds = seconds - previousSeconds;
        previousSeconds = seconds;
        cameraController.Update(deltaSeconds, movement);
        clientSession.Update();
        clientSession.SendMoveInput(
            movement.DirectionAngle(cameraController.MovementYaw()),
            cameraController.IsFlyMode() ? client::net::MoveState::Idle : movement.State());
        noesis.Update(seconds);

        std::vector<WorldRenderEntity> frameEntities;
        WorldCamera frameCamera{};
        bool hasFrameCamera = false;
        if (noesis.IsInWorld())
        {
            frameEntities = noesis.GetWorldEntities();
            WorldVec3 cameraTarget{};
            bool hasOwn = false;
            for (const auto& entity : frameEntities)
            {
                if (entity.netId == noesis.GetOwnNetId())
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

            if (terrainOk)
            {
                terrain.SetMapEditorOpen(noesis.IsMapEditorOpen());
                terrain.SetMapEditorSettings(noesis.GetMapEditorSettings());
                const MapEditorCommands commands = noesis.ConsumeMapEditorCommands();
                if (commands.addTestMarker)
                {
                    EditorTestMarker marker{};
                    marker.id = nextEditorMarkerId++;
                    marker.position = WorldAdd(frameCamera.eye, WorldScale(CameraForward(frameCamera), 5.0f));
                    marker.position.y = terrain.SampleHeight(marker.position) + 0.6f;
                    editorMarkers.push_back(marker);
                    selectedEditorMarkerId = marker.id;
                    selectedEditorObject = {SelectedEditorObjectType::Marker, marker.id};
                    editorGizmoMode = EditorGizmoMode::Translate;
                    noesis.SetEditorStatus("Added and selected marker #" + std::to_string(marker.id));
                }
                if (commands.addPointLight)
                {
                    if (editorPointLights.size() >= kMaxDynamicPointLights)
                    {
                        noesis.SetEditorStatus("Maximum point lights reached (16)");
                    }
                    else
                    {
                        PointLight light{};
                        light.id = nextEditorLightId++;
                        const WorldVec3 spawn = WorldAdd(frameCamera.eye, WorldScale(CameraForward(frameCamera), 5.0f));
                        light.position[0] = spawn.x;
                        light.position[1] = terrain.SampleHeight(spawn) + 1.8f;
                        light.position[2] = spawn.z;
                        editorPointLights.push_back(light);
                        selectedEditorObject = {SelectedEditorObjectType::PointLight, light.id};
                        selectedEditorMarkerId = 0;
                        editorGizmoMode = EditorGizmoMode::Translate;
                        noesis.SetEditorStatus("Added point light #" + std::to_string(light.id));
                    }
                }
                if (commands.addSpotLight)
                {
                    if (editorSpotLights.size() >= kMaxDynamicSpotLights)
                    {
                        noesis.SetEditorStatus("Maximum spot lights reached (16)");
                    }
                    else
                    {
                        SpotLight light{};
                        light.id = nextEditorLightId++;
                        const WorldVec3 spawn = WorldAdd(frameCamera.eye, WorldScale(CameraForward(frameCamera), 5.0f));
                        light.position[0] = spawn.x;
                        light.position[1] = terrain.SampleHeight(spawn) + 4.0f;
                        light.position[2] = spawn.z;
                        light.rotation[0] = -1.5708f;
                        editorSpotLights.push_back(light);
                        selectedEditorObject = {SelectedEditorObjectType::SpotLight, light.id};
                        selectedEditorMarkerId = 0;
                        editorGizmoMode = EditorGizmoMode::Translate;
                        noesis.SetEditorStatus("Added spot light #" + std::to_string(light.id));
                    }
                }
                if (commands.selectedLightChanged)
                {
                    if (commands.selectedLight.type == DynamicLightType::Point)
                    {
                        auto it = std::find_if(editorPointLights.begin(), editorPointLights.end(),
                            [&](const PointLight& light) { return light.id == commands.selectedLight.point.id; });
                        if (it != editorPointLights.end())
                            *it = commands.selectedLight.point;
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
                        }
                    }
                }
                if (commands.deleteSelectedLight)
                {
                    if (selectedEditorObject.type == SelectedEditorObjectType::PointLight)
                    {
                        editorPointLights.erase(std::remove_if(editorPointLights.begin(), editorPointLights.end(),
                            [&](const PointLight& light) { return light.id == selectedEditorObject.id; }), editorPointLights.end());
                        noesis.SetEditorStatus("Deleted point light #" + std::to_string(selectedEditorObject.id));
                        selectedEditorObject = {};
                    }
                    else if (selectedEditorObject.type == SelectedEditorObjectType::SpotLight)
                    {
                        editorSpotLights.erase(std::remove_if(editorSpotLights.begin(), editorSpotLights.end(),
                            [&](const SpotLight& light) { return light.id == selectedEditorObject.id; }), editorSpotLights.end());
                        noesis.SetEditorStatus("Deleted spot light #" + std::to_string(selectedEditorObject.id));
                        selectedEditorObject = {};
                    }
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
                noesis.SetDynamicLightEditorState(dynamicLightState);

                LightingState lightingState = noesis.GetLightingState();
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
                terrain.SetLightingState(lightingState);
                terrain.SetWaterConfig(noesis.GetWaterConfig());
                if (warriorOk)
                {
                    warrior.SetLightingState(lightingState);
                    warrior.SetWaterConfig(noesis.GetWaterConfig());
                }
                if (commands.paletteSlotChanged)
                {
                    const auto slots = noesis.GetPaletteSlots();
                    if (commands.paletteSlot < slots.size() &&
                        !terrain.ApplyPaletteSlotChange(device, slots[commands.paletteSlot]))
                    {
                        Tracenf("[MAIN] failed to apply terrain palette slot %u", commands.paletteSlot);
                    }
                }
                if (commands.save)
                    terrain.RequestEditorSave();
                if (commands.reload)
                    terrain.RequestEditorReload();
                if (commands.undo)
                    terrain.RequestEditorUndo();
                terrain.UpdateEditor(device, deltaSeconds, frameCamera, renderSize.width, renderSize.height);
            }
        }

        device.BeginFrame();
        if (device.IsFrameActive())
        {
            const bool isInWorld = noesis.IsInWorld();
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
                    if (noesis.IsMapEditorOpen())
                    {
                        const size_t editorMarkerRenderCount =
                            editorMarkers.size() + editorPointLights.size() + editorSpotLights.size();
                        for (size_t markerIndex = 0; markerIndex < editorMarkerRenderCount; ++markerIndex)
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
            else if (noesis.IsLobbyActive() && warriorOk)
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
                    [&](const WorldCamera& mirrorCamera, VkExtent2D reflectionExtent, VkRenderPass reflectionRenderPass)
                    {
                        if (!warriorOk)
                            return;

                        const float waterLevelY = noesis.GetWaterConfig().waterLevelY;
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

                        if (noesis.IsMapEditorOpen())
                        {
                            for (const auto& marker : editorMarkers)
                            {
                                if (skinSlot >= WarriorRenderer::MaxSkinSlots())
                                    break;
                                WorldVec3 position = marker.position;
                                position.y += warrior.GroundOffsetY();
                                const bool selected = marker.id == selectedEditorMarkerId;
                                warrior.RenderInWorldReflection(device,
                                    mirrorCamera,
                                    reflectionExtent,
                                    reflectionRenderPass,
                                    waterLevelY,
                                    position,
                                    marker.rotation.y,
                                    skinSlot,
                                    selected
                                        ? std::array<float, 4>{1.8f, 1.55f, 0.25f, 1.0f}
                                        : std::array<float, 4>{0.9f, 1.2f, 1.7f, 1.0f});
                                ++skinSlot;
                            }
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

            noesis.RenderOffscreen(device);
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
                if (warriorOk && noesis.IsMapEditorOpen())
                {
                    for (const auto& marker : editorMarkers)
                    {
                        if (skinSlot >= WarriorRenderer::MaxSkinSlots())
                            break;
                        WorldVec3 position = marker.position;
                        position.y += warrior.GroundOffsetY();
                        const bool selected = marker.id == selectedEditorMarkerId;
                        warrior.RenderInWorld(device,
                            seconds,
                            camera,
                            position,
                            marker.rotation.y,
                            skinSlot,
                            selected
                                ? std::array<float, 4>{1.8f, 1.55f, 0.25f, 1.0f}
                                : std::array<float, 4>{0.9f, 1.2f, 1.7f, 1.0f});
                        plates.push_back(NameplateRenderer::Nameplate{
                            WorldAdd(marker.position, {0.0f, 2.0f, 0.0f}),
                            "Marker " + std::to_string(marker.id),
                            1,
                            selected ? 2u : 0u,
                            1.0f,
                            1.0f,
                            1.0f,
                            selected});
                        ++skinSlot;
                    }
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
                    terrain.RenderWater(device, camera, seconds);
                if (!useOffscreenScene && nameplatesOk)
                    nameplates.Render(device, camera, plates);
            }
            else if (noesis.IsLobbyActive() && warriorOk)
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
            noesis.RenderOnscreen(device);
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
    noesis.Destroy();
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
    if (!window.Create(instance, "Standalone Vulkan Clear - Noesis Overlay", 1280, 720))
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
