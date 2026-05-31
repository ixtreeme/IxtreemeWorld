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
#include <cmath>
#include <cstdio>
#include <cstdint>
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
        case Key_Shift: shift = pressed; return true;
        default: return false;
        }
    }

    bool HasDirection() const { return w || a || s || d; }

    float DirectionAngle() const
    {
        float x = 0.0f;
        float y = 0.0f;
        if (w) y += 1.0f;
        if (s) y -= 1.0f;
        if (d) x += 1.0f;
        if (a) x -= 1.0f;
        return std::atan2(x, y);
    }

    client::net::MoveState State() const
    {
        if (!HasDirection())
            return client::net::MoveState::Idle;
        return shift ? client::net::MoveState::Running : client::net::MoveState::Walking;
    }
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

int RunGame(NativeWindow& window, client::asset::IAssetReader& assets)
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

    window.SetInputCallback([&noesis, &movement](const InputEvent& event)
    {
        movement.Apply(event);
        if (!noesis.OnInput(event))
        {
            // TODO: forward unconsumed events to the game/3D scene input path.
            //LogUnhandledInput(event);
        }
    });

    const auto startTime = std::chrono::steady_clock::now();
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
        clientSession.Update();
        clientSession.SendMoveInput(movement.DirectionAngle(), movement.State());
        noesis.Update(seconds);

        device.BeginFrame();
        if (device.IsFrameActive())
        {
            const bool isInWorld = noesis.IsInWorld();
            std::vector<WorldRenderEntity> entities;
            WorldCamera camera{};

            if (isInWorld)
            {
                entities = noesis.GetWorldEntities();
                WorldVec3 cameraTarget{};
                bool hasOwn = false;
                for (const auto& entity : entities)
                {
                    if (entity.netId == noesis.GetOwnNetId())
                    {
                        cameraTarget = ServerMetersToDisplay(entity.position);
                        hasOwn = true;
                        break;
                    }
                }
                if (!hasOwn && !entities.empty())
                    cameraTarget = ServerMetersToDisplay(entities.front().position);

                camera = BuildFollowCamera(
                    renderSize.width,
                    renderSize.height,
                    cameraTarget,
                    movement.HasDirection() ? movement.DirectionAngle() : 0.0f);

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
            noesis.RenderOnscreen(device);

            if (isInWorld)
            {
                if (terrainOk)
                    terrain.Render(device, camera);

                std::vector<NameplateRenderer::Nameplate> plates;
                plates.reserve(entities.size());
                uint32_t skinSlot = 0;
                for (const auto& entity : entities)
                {
                    const auto position = ServerMetersToDisplay(entity.position);
                    if (warriorOk && skinSlot < WarriorRenderer::MaxSkinSlots())
                    {
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
    const int result = RunGame(window, assets);
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
