#pragma once

// NativeWindowDesc — platform-independent native window descriptor (Phase 3C).
//
// Lets IXVulkan create the Vulkan surface WITHOUT NativeWindow knowing Vulkan
// (no vulkan includes here or in NativeWindow.h anymore). Handles cross the
// boundary opaquely; the stronger typing stays in the platform headers
// (NativeWindow_Win32::GetHwnd, NativeWindow_Android::HasNativeWindow).
// Future Apple targets add a Type + native layer pointer here without
// touching IXRHI renderer API.

#include <cstdint>

struct NativeWindowDesc
{
    enum class Type : std::uint8_t
    {
        Unknown = 0,
        Win32,
        Android,
    };

    Type type = Type::Unknown;
    // Win32: windowHandle = HWND, instanceHandle = HINSTANCE.
    // Android: windowHandle = ANativeWindow*, instanceHandle unused.
    void* windowHandle = nullptr;
    void* instanceHandle = nullptr;
    // Required VK_KHR_*_surface instance extension for this platform.
    const char* vulkanSurfaceExtension = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};
