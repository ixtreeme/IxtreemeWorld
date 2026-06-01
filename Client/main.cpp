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

#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
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
    bool q = false;
    bool e = false;
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
        case Key_Q: q = pressed; return true;
        case Key_E: e = pressed; return true;
        case Key_Shift: shift = pressed; return true;
        default: return false;
        }
    }

    bool HasDirection() const { return w || a || s || d; }
    bool HasFlyVertical() const { return q || e; }

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
            flyMode_ = !flyMode_;
            if (flyMode_)
            {
                flyEye_ = lastEye_;
                flyYaw_ = yaw_;
                flyPitch_ = pitch_;
                Tracen("[CAMERA] mode=fly");
            }
            else
            {
                yaw_ = flyYaw_;
                pitch_ = std::clamp(flyPitch_, -kMaxPitch, kMaxPitch);
                Tracen("[CAMERA] mode=tps");
            }
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
            flyPitch_ = std::clamp(flyPitch_ - dy * kSensitivity, -kMaxPitch, kMaxPitch);
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
        if (movement.e) localY += 1.0f;
        if (movement.q) localY -= 1.0f;

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
        const WorldMat4 projection = WorldPerspective(45.0f * 3.1415926535f / 180.0f, aspect, 0.1f, 1000.0f);
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

    NameplateRenderer nameplates;
    bool nameplatesOk = nameplates.Create(device, assets);
    if (!nameplatesOk)
    {
        Tracenf("[MAIN] NameplateRenderer failed to initialize - nameplates will not be available");
        nameplates.Destroy();
    }

    noesis.SetQuitCallback([&window]()
    {
        window.RequestClose();
    });

    MovementInputState movement;
    CameraController cameraController;

    window.SetInputCallback([&noesis, &movement, &cameraController, &terrain, &terrainOk](const InputEvent& event)
    {
        movement.Apply(event);
        if (event.type == InputEvent::KeyDown && event.key == Key_F2 && terrainOk)
        {
            terrain.ToggleWalkabilityDebug();
            if (!terrain.IsWalkabilityDebugEnabled() && noesis.IsMapEditorOpen())
            {
                noesis.ToggleMapEditor();
                terrain.SetMapEditorOpen(false);
            }
            return;
        }
        if (event.type == InputEvent::KeyDown && event.key == Key_F4 && terrainOk &&
            noesis.IsInWorld() && terrain.IsWalkabilityDebugEnabled())
        {
            noesis.ToggleMapEditor();
            terrain.SetMapEditorOpen(noesis.IsMapEditorOpen());
            return;
        }

        if (terrainOk && noesis.IsMapEditorOpen())
        {
            if (event.type == InputEvent::KeyDown || event.type == InputEvent::KeyUp)
            {
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
                    terrain.HandleEditorInput(event);
                else if (!consumedByEditorUi)
                    terrain.HandleEditorInput(event);

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

            if (terrainOk)
            {
                terrain.SetMapEditorOpen(noesis.IsMapEditorOpen());
                terrain.SetMapEditorSettings(noesis.GetMapEditorSettings());
                const MapEditorCommands commands = noesis.ConsumeMapEditorCommands();
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
                }
            }
            else if (noesis.IsLobbyActive() && warriorOk)
            {
                warrior.Skin(device, seconds);
            }

            noesis.RenderOffscreen(device);
            device.BeginSwapchainRenderPass();

            if (isInWorld)
            {
                if (terrainOk)
                    terrain.Render(device, camera);

                std::vector<NameplateRenderer::Nameplate> plates;
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
                        warrior.RenderInWorld(device,
                            seconds,
                            camera,
                            position,
                            HeadingFromQuantized(entity.heading),
                            skinSlot);
                    }
                    plates.push_back(NameplateRenderer::Nameplate{WorldAdd(position, {0.0f, 2.2f, 0.0f}), entity.name, 1, 0});
                    ++skinSlot;
                }
                if (nameplatesOk)
                    nameplates.Render(device, camera, plates);
            }
            else if (noesis.IsLobbyActive() && warriorOk)
                warrior.Render(device, seconds);

            noesis.RenderOnscreen(device);
        }
        device.EndFrame();
    }

    device.WaitIdle();
    clientSession.Disconnect();
    if (nameplatesOk)
        nameplates.Destroy();
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
