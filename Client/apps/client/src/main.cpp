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
#include <cstring>
#include <cstdlib>

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

void ConfigurePresentSchedulerFromCommandLine(const char* commandLine)
{
    if (commandLine == nullptr)
    {
        return;
    }

    // Keep the feature removable: the command line only overrides the
    // process-local environment used by the Vulkan device.
    if (std::strstr(commandLine, "--async-present") != nullptr)
    {
#if defined(_WIN32)
        _putenv_s("IX_ASYNC_PRESENT", "1");
#else
        setenv("IX_ASYNC_PRESENT", "1", 1);
#endif
        Tracen("[BOOT] async present requested (--async-present)");
    }
    else if (std::strstr(commandLine, "--sync-present") != nullptr)
    {
#if defined(_WIN32)
        _putenv_s("IX_ASYNC_PRESENT", "0");
#else
        setenv("IX_ASYNC_PRESENT", "0", 1);
#endif
        Tracen("[BOOT] synchronous present requested (--sync-present)");
    }
}
}

#if defined(_WIN32)
int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR commandLine, int showCommand)
{
    (void)showCommand;
    ConfigurePresentSchedulerFromCommandLine(commandLine);

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

int main(int argc, char** argv)
{
    // Some toolchains select the console entry point even for a Windows target;
    // preserve the same scheduler switches in that configuration too.
    for (int i = 1; i < argc; ++i)
        ConfigurePresentSchedulerFromCommandLine(argv[i]);
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
