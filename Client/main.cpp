#include <winsock2.h>
#include <windows.h>

#include "GrannyModel.h"
#include "NameplateRenderer.h"
#include "NativeWindow.h"
#include "NoesisLayer.h"
#include "TerrainRenderer.h"
#include "VulkanDevice.h"
#include "WarriorRenderer.h"
#include "WorldCamera.h"
#include "Debug.h"
#include "network/ClientSession.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <unordered_map>
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
    OutputDebugStringA(buffer);
    std::fprintf(stderr, "%s", buffer);
}

std::string ExecutableDirectory()
{
    char path[MAX_PATH]{};
    DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (length == 0 || length == MAX_PATH)
        return ".";

    std::string result(path, length);
    size_t slash = result.find_last_of("\\/");
    return slash == std::string::npos ? "." : result.substr(0, slash);
}

WorldVec3 EntityToLocalDisplay(const TWorldEntityInfo& entity, const TWorldEnterInfo& origin)
{
    return {
        static_cast<float>(entity.x - origin.x) * 0.01f,
        static_cast<float>(entity.z - origin.z) * 0.01f,
        -static_cast<float>(entity.y - origin.y) * 0.01f
    };
}

float ServerAngleToYawRadians(float angleDegrees)
{
    constexpr float kPi = 3.14159265358979323846f;
    return angleDegrees * kPi / 180.0f;
}

float RadiansToDegrees(float radians)
{
    constexpr float kPi = 3.14159265358979323846f;
    float degrees = radians * 180.0f / kPi;
    while (degrees < 0.0f)
        degrees += 360.0f;
    while (degrees >= 360.0f)
        degrees -= 360.0f;
    return degrees;
}

bool IsRenderablePlayerEntity(const TWorldEntityInfo& entity)
{
    constexpr uint8_t kCharTypePc = 6;
    constexpr uint8_t kCharTypePolymorphPc = 7;
    return entity.hasPosition && (entity.type == kCharTypePc || entity.type == kCharTypePolymorphPc);
}

bool IsPlayerEntityType(const TWorldEntityInfo& entity)
{
    constexpr uint8_t kCharTypePc = 6;
    constexpr uint8_t kCharTypePolymorphPc = 7;
    return entity.type == kCharTypePc || entity.type == kCharTypePolymorphPc;
}

const char* AnimStateName(WarriorRenderer::MotionState state)
{
    switch (state)
    {
    case WarriorRenderer::MotionState::Walk: return "walk";
    case WarriorRenderer::MotionState::Run: return "run";
    case WarriorRenderer::MotionState::Idle:
    default: return "wait";
    }
}

WorldVec3 SeparateOverlappingRemote(WorldVec3 remotePosition, uint32_t vid)
{
    const float flatDistanceSq = remotePosition.x * remotePosition.x + remotePosition.z * remotePosition.z;
    if (flatDistanceSq >= 0.16f)
        return remotePosition;

    const float side = (vid & 1u) ? -0.65f : 0.65f;
    remotePosition.x += side;
    remotePosition.z += 0.35f;
    return remotePosition;
}

float LerpFloat(float a, float b, float t)
{
    return a + (b - a) * t;
}

float ShortAngleDelta(float from, float to)
{
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kTwoPi = kPi * 2.0f;
    float delta = std::fmod(to - from, kTwoPi);
    if (delta > kPi)
        delta -= kTwoPi;
    if (delta < -kPi)
        delta += kTwoPi;
    return delta;
}

float LerpAngle(float from, float to, float t)
{
    return from + ShortAngleDelta(from, to) * t;
}

WorldVec3 ServerCmToLocalDisplay(float serverX, float serverY, const TWorldEnterInfo& origin)
{
    return {
        (serverX - static_cast<float>(origin.x)) * 0.01f,
        0.0f,
        -(serverY - static_cast<float>(origin.y)) * 0.01f
    };
}

struct RemoteRenderState
{
    struct Snapshot
    {
        double t = 0.0;
        float serverX = 0.0f;
        float serverY = 0.0f;
        float yawRadians = 0.0f;
    };

    uint32_t vid = 0;
    uint32_t skinSlot = 0;
    bool initialized = false;
    float renderServerX = 0.0f;
    float renderServerY = 0.0f;
    float yawRadians = 0.0f;
    uint32_t moveSequence = 0;
    double renderClock = 0.0;
    double lastSnapshotArrivalTime = 0.0;
    double idleDeadline = 0.0;
    double lastInterpLogTime = -1000.0;
    WarriorRenderer::MotionState animState = WarriorRenderer::MotionState::Idle;
    WarriorRenderer::MotionState loggedAnimState = WarriorRenderer::MotionState::Idle;
    double animTime = 0.0;
    std::deque<Snapshot> snapshots;
};

struct WorldRenderModel
{
    uint32_t vid = 0;
    uint32_t skinSlot = 0;
    WorldVec3 position{};
    std::string name;
    uint32_t level = 0;
    int32_t alignment = 0;
    float yawRadians = 0.0f;
    WarriorRenderer::MotionState animState = WarriorRenderer::MotionState::Idle;
    float animTime = 0.0f;
};

void PushRemoteSnapshot(RemoteRenderState& state, double snapshotTime, float serverX, float serverY, float yawRadians)
{
    constexpr size_t kMaxSnapshots = 8;
    RemoteRenderState::Snapshot snapshot{};
    snapshot.t = state.snapshots.empty() ? snapshotTime : std::max(snapshotTime, state.snapshots.back().t + 0.001);
    snapshot.serverX = serverX;
    snapshot.serverY = serverY;
    snapshot.yawRadians = yawRadians;
    state.snapshots.push_back(snapshot);
    while (state.snapshots.size() > kMaxSnapshots)
        state.snapshots.pop_front();
}

float SnapshotTimeOrZero(const RemoteRenderState::Snapshot* snapshot)
{
    return snapshot ? static_cast<float>(snapshot->t) : 0.0f;
}

struct WorldPlayerController
{
    bool keyW = false;
    bool keyA = false;
    bool keyS = false;
    bool keyD = false;
    bool keyShift = false;
    bool moving = false;
    bool running = false;
    bool lastNetworkMoving = false;
    bool orbitingCamera = false;
    int lastMouseX = 0;
    int lastMouseY = 0;
    WorldVec3 position{};
    float yawRadians = 0.0f;
    float cameraYawRadians = 0.0f;
    float cameraPitchRadians = 0.45f;
    float cameraDistance = 18.0f;
    float boundMinX = -48.0f;
    float boundMaxX = 48.0f;
    float boundMinZ = -48.0f;
    float boundMaxZ = 48.0f;
    double lastLogTime = -1000.0;
    double lastNetworkMoveTime = -1000.0;

    void Reset()
    {
        keyW = keyA = keyS = keyD = keyShift = false;
        moving = false;
        running = false;
        lastNetworkMoving = false;
        orbitingCamera = false;
        lastMouseX = 0;
        lastMouseY = 0;
        position = {};
        yawRadians = 0.0f;
        cameraYawRadians = 0.0f;
        cameraPitchRadians = 0.45f;
        cameraDistance = 18.0f;
        boundMinX = -48.0f;
        boundMaxX = 48.0f;
        boundMinZ = -48.0f;
        boundMaxZ = 48.0f;
        lastLogTime = -1000.0;
        lastNetworkMoveTime = -1000.0;
    }

    bool HasMovementInput() const
    {
        return keyW || keyA || keyS || keyD;
    }

    void ClearMovementInput()
    {
        keyW = keyA = keyS = keyD = keyShift = false;
        moving = false;
        running = false;
        orbitingCamera = false;
    }

    bool HandleInput(const InputEvent& event)
    {
        if (event.type == InputEvent::MouseDown && event.button == MouseButton_Right)
        {
            orbitingCamera = true;
            lastMouseX = event.x;
            lastMouseY = event.y;
            return true;
        }

        if (event.type == InputEvent::MouseUp && event.button == MouseButton_Right)
        {
            orbitingCamera = false;
            return true;
        }

        if (event.type == InputEvent::MouseMove && orbitingCamera)
        {
            const int dx = event.x - lastMouseX;
            const int dy = event.y - lastMouseY;
            lastMouseX = event.x;
            lastMouseY = event.y;
            const float yawDelta = static_cast<float>(dx) * 0.006f;
            cameraYawRadians += yawDelta;
            if (HasMovementInput())
                yawRadians = cameraYawRadians;
            cameraPitchRadians = std::clamp(cameraPitchRadians + static_cast<float>(dy) * 0.004f, 0.12f, 1.15f);
            return true;
        }

        if (event.type == InputEvent::MouseWheel)
        {
            const float wheelSteps = static_cast<float>(event.wheelDelta) / 120.0f;
            cameraDistance = std::clamp(cameraDistance - wheelSteps * 1.5f, 6.0f, 35.0f);
            return true;
        }

        if (event.type != InputEvent::KeyDown && event.type != InputEvent::KeyUp)
            return false;

        const bool pressed = event.type == InputEvent::KeyDown;
        switch (event.key)
        {
        case Key_W: keyW = pressed; return true;
        case Key_A: keyA = pressed; return true;
        case Key_S: keyS = pressed; return true;
        case Key_D: keyD = pressed; return true;
        case Key_Shift: keyShift = pressed; return true;
        default: return false;
        }
    }

    void Update(float deltaSeconds, double nowSeconds)
    {
        float moveX = 0.0f;
        float moveZ = 0.0f;
        if (keyW)
            moveZ += 1.0f;
        if (keyS)
            moveZ -= 1.0f;
        if (keyD)
            moveX += 1.0f;
        if (keyA)
            moveX -= 1.0f;

        const float length = std::sqrt(moveX * moveX + moveZ * moveZ);
        moving = length > 0.0001f;
        running = moving && keyShift;
        if (moving)
        {
            moveX /= length;
            moveZ /= length;
            float worldX = 0.0f;
            float worldZ = 0.0f;
            if (orbitingCamera)
            {
                yawRadians = cameraYawRadians;
                const float direction = (keyS && !keyW) ? -1.0f : 1.0f;
                worldX = std::sin(yawRadians) * direction;
                worldZ = std::cos(yawRadians) * direction;
            }
            else
            {
                const float forwardX = std::sin(cameraYawRadians);
                const float forwardZ = std::cos(cameraYawRadians);
                const float rightX = std::cos(cameraYawRadians);
                const float rightZ = -std::sin(cameraYawRadians);
                worldX = forwardX * moveZ + rightX * moveX;
                worldZ = forwardZ * moveZ + rightZ * moveX;
                yawRadians = std::atan2(worldX, worldZ);
            }
            const float speed = running ? 5.5f : 2.35f;
            position.x = std::clamp(position.x + worldX * speed * deltaSeconds, boundMinX, boundMaxX);
            position.z = std::clamp(position.z + worldZ * speed * deltaSeconds, boundMinZ, boundMaxZ);
        }

        if (nowSeconds - lastLogTime >= 1.0)
        {
            Tracenf("[MOVE] pos=(%.2f,%.2f,%.2f) yaw=%.2f moving=%d running=%d cameraYaw=%.2f cameraPitch=%.2f zoom=%.2f",
                position.x,
                position.y,
                position.z,
                yawRadians,
                moving ? 1 : 0,
                running ? 1 : 0,
                cameraYawRadians,
                cameraPitchRadians,
                cameraDistance);
            lastLogTime = nowSeconds;
        }
    }

    WarriorRenderer::MotionState GetMotionState() const
    {
        if (!moving)
            return WarriorRenderer::MotionState::Idle;
        return running ? WarriorRenderer::MotionState::Run : WarriorRenderer::MotionState::Walk;
    }

    bool ShouldSendNetworkMove(double nowSeconds) const
    {
        if (moving)
            return nowSeconds - lastNetworkMoveTime >= 0.12;
        return lastNetworkMoving;
    }

    void MarkNetworkMoveSent(double nowSeconds)
    {
        lastNetworkMoveTime = nowSeconds;
        lastNetworkMoving = moving;
    }
};

int Run(HINSTANCE instance, int showCommand)
{
    (void)showCommand;

    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        MessageBoxA(nullptr, "Failed to initialize WinSock.", "VulkanClear", MB_ICONERROR);
        return 1;
    }

    NativeWindow window;
    if (!window.Create(instance, "Standalone Vulkan Clear - Noesis Overlay", 1280, 720))
    {
        MessageBoxA(nullptr, "Failed to create Win32 window.", "VulkanClear", MB_ICONERROR);
        WSACleanup();
        return 1;
    }

    VulkanDevice device;
    if (!device.Create(window.GetHwnd(), window.GetWidth(), window.GetHeight()))
    {
        MessageBoxA(nullptr, "Failed to create Vulkan device. See debug output/stderr.", "VulkanClear", MB_ICONERROR);
        window.Destroy();
        WSACleanup();
        return 1;
    }

    NoesisLayer noesis;
    if (!noesis.Create(device, window.GetWidth(), window.GetHeight()))
    {
        MessageBoxA(nullptr, "Failed to create Noesis layer. See debug output/stderr.", "VulkanClear", MB_ICONERROR);
        device.Destroy();
        window.Destroy();
        WSACleanup();
        return 1;
    }
    client::net::ClientSession clientSession(noesis);
    noesis.SetClientSession(&clientSession);

    GrannyModel grannyModel;
    const std::string warriorModelPath = ExecutableDirectory() + "\\Character\\warrior_4-1.gr2";
    const std::string selectedAnimationPath = ExecutableDirectory() + "\\Character\\selected.gr2";
    grannyModel.LoadAndLog(warriorModelPath);
    grannyModel.LoadAnimationAndCompare(warriorModelPath, selectedAnimationPath);
    grannyModel.ComputeStaticPoseAndLog(warriorModelPath, selectedAnimationPath, 0.0f);

    WarriorRenderer warrior;
    if (!warrior.Create(device, warriorModelPath))
    {
        MessageBoxA(nullptr, "Failed to create warrior renderer. See debug output/stderr.", "VulkanClear", MB_ICONERROR);
        noesis.Destroy();
        device.Destroy();
        window.Destroy();
        WSACleanup();
        return 1;
    }

    TerrainRenderer terrain;
    if (!terrain.Create(device))
    {
        MessageBoxA(nullptr, "Failed to create terrain renderer. See debug output/stderr.", "VulkanClear", MB_ICONERROR);
        warrior.Destroy();
        noesis.Destroy();
        device.Destroy();
        window.Destroy();
        WSACleanup();
        return 1;
    }

    NameplateRenderer nameplates;
    if (!nameplates.Create(device))
    {
        MessageBoxA(nullptr, "Failed to create nameplate renderer. See debug output/stderr.", "VulkanClear", MB_ICONERROR);
        terrain.Destroy();
        warrior.Destroy();
        noesis.Destroy();
        device.Destroy();
        window.Destroy();
        WSACleanup();
        return 1;
    }

    noesis.SetQuitCallback([&window]()
    {
        window.RequestClose();
    });

    WorldPlayerController worldPlayer;
    window.SetInputCallback([&noesis, &worldPlayer](const InputEvent& event)
    {
        if (noesis.IsWorldActive() &&
            event.type == InputEvent::KeyDown &&
            event.key == Key_Escape)
        {
            worldPlayer.ClearMovementInput();
            noesis.ToggleInGameMenu();
            return;
        }

        if (noesis.IsInGameMenuOpen())
        {
            noesis.OnInput(event);
            return;
        }

        if (noesis.IsWorldActive() &&
            event.type == InputEvent::KeyDown &&
            event.key == Key_Q)
        {
            noesis.ToggleQuestPanel();
            return;
        }

        if (noesis.IsWorldActive() &&
            event.type == InputEvent::KeyDown &&
            event.key == Key_C)
        {
            noesis.ToggleCharacterPanel();
            return;
        }

        if (noesis.IsQuestPanelOpen() || noesis.IsCharacterPanelOpen())
        {
            if (event.type == InputEvent::KeyDown ||
                event.type == InputEvent::KeyUp ||
                event.type == InputEvent::Char)
            {
                if (noesis.IsWorldActive() && worldPlayer.HandleInput(event))
                    return;
                if (!noesis.OnInput(event))
                {
                    // TODO: forward unconsumed events to the game/3D scene input path.
                    //LogUnhandledInput(event);
                }
                return;
            }

            if (!noesis.OnInput(event) &&
                noesis.IsWorldActive() &&
                worldPlayer.HandleInput(event))
            {
                return;
            }
            return;
        }

        if (noesis.IsWorldActive() && worldPlayer.HandleInput(event))
            return;

        if (!noesis.OnInput(event))
        {
            // TODO: forward unconsumed events to the game/3D scene input path.
            //LogUnhandledInput(event);
        }
    });

    const auto startTime = std::chrono::steady_clock::now();
    auto previousFrameTime = startTime;
    bool wasWorldActive = false;
    bool worldMapLoaded = false;
    double lastGroundLogTime = -1000.0;
    constexpr double kRemoteRenderDelaySeconds = 0.07;
    constexpr double kRemoteMinSampleStepSeconds = 0.016;
    constexpr double kRemoteMaxSampleStepSeconds = 0.080;
    constexpr double kRemoteMaxSnapshotLeadSeconds = 0.120;
    constexpr double kRemoteMaxExtrapolateSeconds = 0.180;
    WarriorRenderer::MotionState localAnimState = WarriorRenderer::MotionState::Idle;
    WarriorRenderer::MotionState loggedLocalAnimState = WarriorRenderer::MotionState::Idle;
    double localAnimTime = 0.0;
    std::unordered_map<uint32_t, RemoteRenderState> remoteRenderStates;
    uint32_t nextRemoteSkinSlot = 1;
    std::vector<WorldRenderModel> worldRenderModels;
    bool running = true;
    while (running)
    {
        running = window.PumpMessages();

        uint32_t width = 0;
        uint32_t height = 0;
        if (window.ConsumeResize(width, height))
        {
            if (device.Resize(width, height))
            {
                warrior.RecreatePipeline(device);
                terrain.RecreatePipeline(device);
                nameplates.RecreatePipeline(device);
                noesis.OnRenderPassChanged(device);
                noesis.Resize(width, height);
            }
        }

        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(now - startTime).count();
        const float deltaSeconds = std::min(0.1f, static_cast<float>(std::chrono::duration<double>(now - previousFrameTime).count()));
        previousFrameTime = now;
        clientSession.Update();
        noesis.Update(seconds);
        const bool worldActive = noesis.IsWorldActive();
        if (worldActive && !wasWorldActive)
        {
            worldPlayer.Reset();
            remoteRenderStates.clear();
            nextRemoteSkinSlot = 1;
            localAnimState = WarriorRenderer::MotionState::Idle;
            loggedLocalAnimState = WarriorRenderer::MotionState::Idle;
            localAnimTime = 0.0;
            const TWorldEnterInfo& info = noesis.GetWorldEnterInfo();
            const std::string mapDirectory = ExecutableDirectory() + "\\Maps\\metin2_map_main_razor93";
            worldMapLoaded = terrain.LoadMap(device, mapDirectory, info.x, info.y);
            if (worldMapLoaded)
            {
                const TerrainRenderer::MovementBounds bounds = terrain.GetMovementBounds();
                if (bounds.valid)
                {
                    worldPlayer.boundMinX = bounds.minX;
                    worldPlayer.boundMaxX = bounds.maxX;
                    worldPlayer.boundMinZ = bounds.minZ;
                    worldPlayer.boundMaxZ = bounds.maxZ;
                    Tracenf("[MOVE] bounds x=(%.2f,%.2f) z=(%.2f,%.2f)",
                        worldPlayer.boundMinX,
                        worldPlayer.boundMaxX,
                        worldPlayer.boundMinZ,
                        worldPlayer.boundMaxZ);
                }
            }
            lastGroundLogTime = -1000.0;
            Tracen("[MOVE] world controls active: WASD move, Shift run, right mouse orbit, wheel zoom");
        }
        if (worldActive)
        {
            worldPlayer.Update(deltaSeconds, seconds);
            if (worldMapLoaded)
            {
                const float sampledGroundY = terrain.SampleHeightAt(worldPlayer.position.x, worldPlayer.position.z);
                worldPlayer.position.y = sampledGroundY;
                if (seconds - lastGroundLogTime >= 1.0)
                {
                    Tracenf("[GROUND] playerLocalXZ=(%.2f,%.2f) sampledY=%.2f cameraTargetY=%.2f",
                        worldPlayer.position.x,
                        worldPlayer.position.z,
                        sampledGroundY,
                        sampledGroundY + 1.15f);
                    lastGroundLogTime = seconds;
                }
            }

            if (worldPlayer.ShouldSendNetworkMove(seconds))
            {
                // TODO Phase 3: client->server movement packet.
                worldPlayer.MarkNetworkMoveSent(seconds);
            }
        }
        else
        {
            worldMapLoaded = false;
            worldRenderModels.clear();
            remoteRenderStates.clear();
            warrior.SetMotionState(WarriorRenderer::MotionState::Idle);
        }

        worldRenderModels.clear();
        if (worldActive)
        {
            const TWorldEnterInfo& info = noesis.GetWorldEnterInfo();
            const std::vector<TWorldEntityInfo>& entities = noesis.GetWorldEntities();
            localAnimState = worldPlayer.GetMotionState();
            localAnimTime += deltaSeconds;
            if (localAnimState != loggedLocalAnimState)
            {
                Tracenf("[ANIM] vid=%u state=%s", info.vid, AnimStateName(localAnimState));
                loggedLocalAnimState = localAnimState;
            }

            const TWorldEntityInfo* localEntity = nullptr;
            for (const TWorldEntityInfo& entity : entities)
            {
                if (entity.vid == info.vid)
                {
                    localEntity = &entity;
                    break;
                }
            }

            WorldRenderModel localModel{};
            localModel.vid = info.vid;
            localModel.skinSlot = 0;
            localModel.position = worldPlayer.position;
            localModel.name = localEntity && localEntity->name[0] ? localEntity->name : info.name;
            localModel.level = localEntity ? localEntity->level : 0;
            localModel.alignment = localEntity ? localEntity->alignment : 0;
            localModel.yawRadians = worldPlayer.yawRadians;
            localModel.animState = localAnimState;
            localModel.animTime = static_cast<float>(localAnimTime);
            worldRenderModels.push_back(localModel);

            for (const TWorldEntityInfo& entity : entities)
            {
                if (entity.vid == 0 || entity.vid == info.vid || !IsRenderablePlayerEntity(entity))
                    continue;

                RemoteRenderState& state = remoteRenderStates[entity.vid];
                if (!state.initialized)
                {
                    state.vid = entity.vid;
                    state.skinSlot = nextRemoteSkinSlot < WarriorRenderer::MaxSkinSlots() ? nextRemoteSkinSlot++ : 0;
                    state.initialized = true;
                    const TWorldMoveSample* firstSample = entity.moveSampleCount > 0 ? &entity.moveSamples[0] : nullptr;
                    state.renderServerX = firstSample ? static_cast<float>(firstSample->x) : static_cast<float>(entity.x);
                    state.renderServerY = firstSample ? static_cast<float>(firstSample->y) : static_cast<float>(entity.y);
                    state.yawRadians = ServerAngleToYawRadians(firstSample ? firstSample->angle : entity.angle);
                    state.moveSequence = firstSample && firstSample->sequence > 0 ? firstSample->sequence - 1 : entity.moveSequence;
                    state.renderClock = seconds;
                    state.lastSnapshotArrivalTime = seconds;
                    state.animState = entity.moveFunc == 1 ? WarriorRenderer::MotionState::Walk : WarriorRenderer::MotionState::Idle;
                    state.loggedAnimState = state.animState;
                    state.animTime = static_cast<double>((entity.vid % 997u) + 1u) * 0.013;
                    state.idleDeadline = seconds + static_cast<double>(entity.moveDurationMs) * 0.001 + 0.5;
                    PushRemoteSnapshot(state, state.renderClock, state.renderServerX, state.renderServerY, state.yawRadians);
                    Tracenf("[ANIM] vid=%u state=%s", entity.vid, AnimStateName(state.animState));
                }
                else
                {
                    state.renderClock += deltaSeconds;
                }

                for (uint32_t sampleIndex = 0; sampleIndex < entity.moveSampleCount; ++sampleIndex)
                {
                    const TWorldMoveSample& sample = entity.moveSamples[sampleIndex];
                    if (sample.sequence <= state.moveSequence)
                        continue;

                    if (seconds - state.lastSnapshotArrivalTime > 0.5 && state.animState == WarriorRenderer::MotionState::Idle)
                    {
                        state.snapshots.clear();
                        PushRemoteSnapshot(state, state.renderClock, state.renderServerX, state.renderServerY, state.yawRadians);
                    }

                    const double sampleSeconds = std::clamp(
                        static_cast<double>(sample.durationMs) * 0.001,
                        kRemoteMinSampleStepSeconds,
                        kRemoteMaxSampleStepSeconds);
                    const double maxSnapshotTime = state.renderClock + kRemoteMaxSnapshotLeadSeconds;
                    const double baseSnapshotTime = state.snapshots.empty() ? state.renderClock : state.snapshots.back().t;
                    const double snapshotTime = std::min(
                        std::max(state.renderClock, baseSnapshotTime) + sampleSeconds,
                        maxSnapshotTime);
                    const float sampleYaw = ServerAngleToYawRadians(sample.angle);
                    if (!state.snapshots.empty() && state.snapshots.back().t >= maxSnapshotTime)
                    {
                        RemoteRenderState::Snapshot& latest = state.snapshots.back();
                        latest.serverX = static_cast<float>(sample.x);
                        latest.serverY = static_cast<float>(sample.y);
                        latest.yawRadians = sampleYaw;
                    }
                    else
                    {
                        PushRemoteSnapshot(state, snapshotTime, static_cast<float>(sample.x), static_cast<float>(sample.y),
                            sampleYaw);
                    }
                    state.lastSnapshotArrivalTime = seconds;
                    state.moveSequence = sample.sequence;
                    state.idleDeadline = seconds + static_cast<double>(sample.durationMs) * 0.001 + 0.5;
                    const WarriorRenderer::MotionState newState = sample.func == 1
                        ? WarriorRenderer::MotionState::Walk
                        : WarriorRenderer::MotionState::Idle;
                    if (newState != state.animState)
                    {
                        state.animState = newState;
                        Tracenf("[ANIM] vid=%u state=%s", entity.vid, AnimStateName(state.animState));
                    }
                }
                if (state.animState != WarriorRenderer::MotionState::Idle && seconds > state.idleDeadline)
                {
                    state.animState = WarriorRenderer::MotionState::Idle;
                    Tracenf("[ANIM] vid=%u state=%s", entity.vid, AnimStateName(state.animState));
                }

                const double renderTime = state.renderClock - kRemoteRenderDelaySeconds;
                const RemoteRenderState::Snapshot* previous = nullptr;
                const RemoteRenderState::Snapshot* a = nullptr;
                const RemoteRenderState::Snapshot* b = nullptr;
                for (const RemoteRenderState::Snapshot& snapshot : state.snapshots)
                {
                    if (snapshot.t <= renderTime)
                    {
                        previous = a;
                        a = &snapshot;
                    }
                    else
                    {
                        b = &snapshot;
                        break;
                    }
                }

                float interpT = 0.0f;
                const char* interpMode = "empty";
                bool extrapolated = false;
                if (a && b)
                {
                    const double span = b->t - a->t;
                    interpT = span > 0.000001 ? std::clamp(static_cast<float>((renderTime - a->t) / span), 0.0f, 1.0f) : 1.0f;
                    state.renderServerX = LerpFloat(a->serverX, b->serverX, interpT);
                    state.renderServerY = LerpFloat(a->serverY, b->serverY, interpT);
                    state.yawRadians = LerpAngle(a->yawRadians, b->yawRadians, interpT);
                    interpMode = "interp";
                }
                else if (a)
                {
                    const double extrapolateSeconds = std::clamp(renderTime - a->t, 0.0, kRemoteMaxExtrapolateSeconds);
                    if (previous && extrapolateSeconds > 0.0 && state.animState != WarriorRenderer::MotionState::Idle)
                    {
                        const double span = a->t - previous->t;
                        if (span > 0.000001)
                        {
                            const float xVelocity = static_cast<float>((a->serverX - previous->serverX) / span);
                            const float yVelocity = static_cast<float>((a->serverY - previous->serverY) / span);
                            const float yawVelocity = static_cast<float>(ShortAngleDelta(previous->yawRadians, a->yawRadians) / span);
                            state.renderServerX = a->serverX + xVelocity * static_cast<float>(extrapolateSeconds);
                            state.renderServerY = a->serverY + yVelocity * static_cast<float>(extrapolateSeconds);
                            state.yawRadians = a->yawRadians + yawVelocity * static_cast<float>(extrapolateSeconds);
                            interpMode = "extrap";
                            extrapolated = true;
                        }
                    }
                    if (!extrapolated)
                    {
                        state.renderServerX = a->serverX;
                        state.renderServerY = a->serverY;
                        state.yawRadians = a->yawRadians;
                        interpMode = "hold";
                    }
                }
                else if (!state.snapshots.empty())
                {
                    const RemoteRenderState::Snapshot& first = state.snapshots.front();
                    state.renderServerX = first.serverX;
                    state.renderServerY = first.serverY;
                    state.yawRadians = first.yawRadians;
                    interpMode = "warmup";
                }

                if (seconds - state.lastInterpLogTime >= 0.10)
                {
                    const float aTime = SnapshotTimeOrZero(a);
                    const float bTime = SnapshotTimeOrZero(b);
                    Tracenf("[INTERP] vid=%u mode=%s renderTime=%.3f clock=%.3f snaps=%zu a.t=%.3f b.t=%.3f t=%.2f renderSrv=(%.1f,%.1f) yaw=%.2f",
                        entity.vid,
                        interpMode,
                        renderTime,
                        state.renderClock,
                        state.snapshots.size(),
                        aTime,
                        bTime,
                        interpT,
                        state.renderServerX,
                        state.renderServerY,
                        RadiansToDegrees(state.yawRadians));
                    state.lastInterpLogTime = seconds;
                }

                while (state.snapshots.size() > 2 && state.snapshots[1].t < renderTime - 0.5)
                    state.snapshots.pop_front();

                state.animTime += deltaSeconds;
                WorldVec3 entityPosition = SeparateOverlappingRemote(
                    ServerCmToLocalDisplay(state.renderServerX, state.renderServerY, info),
                    entity.vid);
                if (worldMapLoaded)
                    entityPosition.y = terrain.SampleHeightAt(entityPosition.x, entityPosition.z);

                if (state.skinSlot > 0)
                {
                    WorldRenderModel remoteModel{};
                    remoteModel.vid = entity.vid;
                    remoteModel.skinSlot = state.skinSlot;
                    remoteModel.position = entityPosition;
                    remoteModel.name = entity.name;
                    remoteModel.level = entity.level;
                    remoteModel.alignment = entity.alignment;
                    remoteModel.yawRadians = state.yawRadians;
                    remoteModel.animState = state.animState;
                    remoteModel.animTime = static_cast<float>(state.animTime);
                    worldRenderModels.push_back(remoteModel);
                }
            }
        }
        wasWorldActive = worldActive;

        device.BeginFrame();
        if (device.IsFrameActive())
        {
            if (worldActive)
            {
                for (const WorldRenderModel& model : worldRenderModels)
                    warrior.SkinInstance(device, model.skinSlot, model.animState, model.animTime);
            }
            else
            {
                warrior.Skin(device, seconds);
            }
            noesis.RenderOffscreen(device);
            device.BeginSwapchainRenderPass();
            if (worldActive)
            {
                static bool loggedWorldRender = false;
                const TWorldEnterInfo& info = noesis.GetWorldEnterInfo();
                const WorldVec3 spawnDisplay = RawServerToDisplay(info.x, info.y, info.z);
                const VkExtent2D extent = device.GetSwapchainExtent();
                const WorldCamera camera = BuildOrbitCamera(extent.width, extent.height,
                    worldPlayer.position,
                    worldPlayer.cameraYawRadians,
                    worldPlayer.cameraPitchRadians,
                    worldPlayer.cameraDistance);
                if (!loggedWorldRender)
                {
                    Tracenf("[WORLD-RENDER] active=1 vid=%u spawnServer=(%d,%d,%d) spawnDisplay=(%.2f,%.2f,%.2f) worldOrigin=spawn playerLocal=(%.2f,%.2f,%.2f) cameraEye=(%.2f,%.2f,%.2f) cameraTarget=(%.2f,%.2f,%.2f)",
                        info.vid,
                        info.x,
                        info.y,
                        info.z,
                        spawnDisplay.x,
                        spawnDisplay.y,
                        spawnDisplay.z,
                        worldPlayer.position.x,
                        worldPlayer.position.y,
                        worldPlayer.position.z,
                        camera.eye.x,
                        camera.eye.y,
                        camera.eye.z,
                        camera.target.x,
                        camera.target.y,
                        camera.target.z);
                    loggedWorldRender = true;
                }
                terrain.Render(device, camera);
                for (const WorldRenderModel& model : worldRenderModels)
                    warrior.RenderInWorld(device, seconds, camera, model.position, model.yawRadians, model.skinSlot);
                std::vector<NameplateRenderer::Nameplate> nameplateBatch;
                nameplateBatch.reserve(worldRenderModels.size());
                for (const WorldRenderModel& model : worldRenderModels)
                {
                    NameplateRenderer::Nameplate nameplate{};
                    nameplate.position = model.position;
                    nameplate.name = model.name;
                    nameplate.level = model.level;
                    nameplate.alignment = model.alignment;
                    nameplateBatch.push_back(nameplate);
                }
                nameplates.Render(device, camera, nameplateBatch);
                const std::vector<TWorldEntityInfo>& entities = noesis.GetWorldEntities();
                uint32_t renderedRemoteCount = 0;
                uint32_t skippedSelfCount = 0;
                uint32_t skippedNoPositionCount = 0;
                uint32_t skippedNonPlayerCount = 0;
                uint32_t playerEntityCount = 0;
                uint32_t unknownPositionEntityCount = 0;
                TWorldEntityInfo firstRenderedRemote{};
                WorldVec3 firstRenderedPosition{};
                TWorldEntityInfo firstSkippedEntity{};
                TWorldEntityInfo firstOtherPlayerEntity{};
                for (const TWorldEntityInfo& entity : entities)
                {
                    if (entity.vid == 0)
                        continue;

                    if (IsPlayerEntityType(entity))
                    {
                        ++playerEntityCount;
                        if (entity.vid != info.vid && firstOtherPlayerEntity.vid == 0)
                            firstOtherPlayerEntity = entity;
                    }
                    else if (entity.hasPosition && entity.type == 0)
                    {
                        ++unknownPositionEntityCount;
                    }

                    if (entity.vid == info.vid)
                    {
                        ++skippedSelfCount;
                        continue;
                    }

                    if (!IsRenderablePlayerEntity(entity))
                    {
                        if (skippedNoPositionCount == 0 && skippedNonPlayerCount == 0)
                            firstSkippedEntity = entity;
                        if (!entity.hasPosition)
                            ++skippedNoPositionCount;
                        else
                            ++skippedNonPlayerCount;
                        continue;
                    }

                    const WorldRenderModel* renderedModel = nullptr;
                    for (const WorldRenderModel& model : worldRenderModels)
                    {
                        if (model.vid == entity.vid)
                        {
                            renderedModel = &model;
                            break;
                        }
                    }
                    if (!renderedModel)
                        continue;
                    if (renderedRemoteCount == 0)
                    {
                        firstRenderedRemote = entity;
                        firstRenderedPosition = renderedModel->position;
                    }
                    ++renderedRemoteCount;
                }

                static double lastRemoteRenderLogTime = -1000.0;
                if (seconds - lastRemoteRenderLogTime >= 1.0)
                {
                    char entitySummary[512]{};
                    size_t summaryOffset = 0;
                    const size_t summaryCount = std::min<size_t>(entities.size(), 4);
                    for (size_t index = 0; index < summaryCount; ++index)
                    {
                        const TWorldEntityInfo& entity = entities[index];
                        const int written = std::snprintf(
                            entitySummary + summaryOffset,
                            sizeof(entitySummary) - summaryOffset,
                            "%s[%zu vid=%u type=%u race=%u pos=%u name=%s srv=(%d,%d,%d)]",
                            index == 0 ? "" : " ",
                            index,
                            entity.vid,
                            entity.type,
                            entity.race,
                            entity.hasPosition ? 1 : 0,
                            entity.name,
                            entity.x,
                            entity.y,
                            entity.z);
                        if (written <= 0)
                            break;
                        summaryOffset += std::min<size_t>(static_cast<size_t>(written), sizeof(entitySummary) - summaryOffset - 1);
                    }

                    const int32_t localServerX = info.x + static_cast<int32_t>(std::lround(worldPlayer.position.x * 100.0f));
                    const int32_t localServerY = info.y - static_cast<int32_t>(std::lround(worldPlayer.position.z * 100.0f));
                    const int32_t localServerZ = info.z + static_cast<int32_t>(std::lround(worldPlayer.position.y * 100.0f));
                    Tracenf("[WORLD-ENTITY-RENDER] remoteCharacters=%u totalEntities=%zu playerEntities=%u unknownPositionEntities=%u skippedSelf=%u skippedNoPos=%u skippedNonPlayer=%u localServer=(%d,%d,%d) firstOtherPlayerVid=%u type=%u race=%u hasPos=%u name=%s srv=(%d,%d,%d) firstRemoteVid=%u type=%u race=%u hasPos=%u name=%s local=(%.2f,%.2f,%.2f) firstSkippedVid=%u type=%u race=%u hasPos=%u name=%s entities=%s",
                        renderedRemoteCount,
                        entities.size(),
                        playerEntityCount,
                        unknownPositionEntityCount,
                        skippedSelfCount,
                        skippedNoPositionCount,
                        skippedNonPlayerCount,
                        localServerX,
                        localServerY,
                        localServerZ,
                        firstOtherPlayerEntity.vid,
                        firstOtherPlayerEntity.type,
                        firstOtherPlayerEntity.race,
                        firstOtherPlayerEntity.hasPosition ? 1 : 0,
                        firstOtherPlayerEntity.name,
                        firstOtherPlayerEntity.x,
                        firstOtherPlayerEntity.y,
                        firstOtherPlayerEntity.z,
                        firstRenderedRemote.vid,
                        firstRenderedRemote.type,
                        firstRenderedRemote.race,
                        firstRenderedRemote.hasPosition ? 1 : 0,
                        firstRenderedRemote.name,
                        firstRenderedPosition.x,
                        firstRenderedPosition.y,
                        firstRenderedPosition.z,
                        firstSkippedEntity.vid,
                        firstSkippedEntity.type,
                        firstSkippedEntity.race,
                        firstSkippedEntity.hasPosition ? 1 : 0,
                        firstSkippedEntity.name,
                        entitySummary);
                    lastRemoteRenderLogTime = seconds;
                }
                if (noesis.IsInGameMenuOpen() || noesis.IsQuestPanelOpen() || noesis.IsCharacterPanelOpen())
                    noesis.RenderOnscreen(device);
            }
            else
            {
                noesis.RenderOnscreen(device);
            }

            if (noesis.IsLobbyActive())
                warrior.Render(device, seconds);
        }
        device.EndFrame();
    }

    device.WaitIdle();
    clientSession.Disconnect();
    grannyModel.Destroy();
    nameplates.Destroy();
    terrain.Destroy();
    warrior.Destroy();
    noesis.Destroy();
    device.Destroy();
    window.Destroy();
    WSACleanup();
    return 0;
}
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int showCommand)
{
    return Run(instance, showCommand);
}

int main()
{
    return Run(GetModuleHandleA(nullptr), SW_SHOW);
}
