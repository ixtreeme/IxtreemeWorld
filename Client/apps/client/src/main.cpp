#if defined(_WIN32)
#include <windows.h>
#endif

#include "EngineApplication.h"
#include "NativeWindow.h"

#if defined(_WIN32)
#include "NativeWindow_Win32.h"
#include "asset/FileAssetReader.h"
#endif

#if defined(__ANDROID__)
#include "NativeWindow_Android.h"
#include "asset/AAssetManagerAssetReader.h"
#include <android_native_app_glue.h>
#endif

#include "Debug.h"

#include <cstdint>
#include <cstdio>

namespace
{
void ShowFatal(const char* message)
{
#if defined(_WIN32)
    MessageBoxA(nullptr, message, "IxtreemeEngine", MB_ICONERROR);
#else
    std::fprintf(stderr, "%s\n", message);
#endif
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
    if (!window.Create(instance, "Ixtreeme Engine", windowWidth, windowHeight))
    {
        ShowFatal("Failed to create Win32 window.");
        return 1;
    }

    client::asset::FileAssetReader assets(ResolveIxtreemeEngineAssetRoot());
    const int result = RunIxtreemeEngine(window, assets);
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
    (void)RunIxtreemeEngine(window, assets);
    g_androidApp = nullptr;
}
#endif
