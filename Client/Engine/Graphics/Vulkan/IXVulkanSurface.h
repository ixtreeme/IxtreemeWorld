#pragma once

// IXVulkanSurface — backend-owned platform surface creation (Phase 3C, §17).
//
// Builds VkSurfaceKHR from the platform-neutral NativeWindowDesc, so
// NativeWindow never includes Vulkan. Win32/Android today; Apple adds a
// VK_EXT_metal_surface branch here later (MoltenVK prep, no renderer changes).
//
// Header-only by design: legacy platform code (IXEnginePlatform, which cannot
// link the backend) delegates surface creation here without a dependency cycle.

#if defined(_WIN32) && !defined(VK_USE_PLATFORM_WIN32_KHR)
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <vulkan/vulkan.h>

#include "platform/NativeWindowDesc.h"

#if defined(_WIN32)
#include <windows.h>
#endif
#if defined(__ANDROID__)
#include <android/native_window.h>
#endif

namespace ixvulkan
{

inline VkResult CreateSurfaceForWindow(const NativeWindowDesc& desc,
                                       VkInstance instance,
                                       VkSurfaceKHR* outSurface)
{
    if (outSurface == nullptr)
        return VK_ERROR_INITIALIZATION_FAILED;
    *outSurface = VK_NULL_HANDLE;

    if (desc.type == NativeWindowDesc::Type::Win32)
    {
#if defined(_WIN32)
        VkWin32SurfaceCreateInfoKHR create{};
        create.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        create.hinstance = static_cast<HINSTANCE>(desc.instanceHandle);
        create.hwnd = static_cast<HWND>(desc.windowHandle);
        return vkCreateWin32SurfaceKHR(instance, &create, nullptr, outSurface);
#else
        (void)instance;
        return VK_ERROR_EXTENSION_NOT_PRESENT;
#endif
    }

    if (desc.type == NativeWindowDesc::Type::Android)
    {
#if defined(__ANDROID__)
        if (desc.windowHandle == nullptr)
            return VK_ERROR_SURFACE_LOST_KHR;
        VkAndroidSurfaceCreateInfoKHR create{};
        create.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
        create.window = static_cast<ANativeWindow*>(desc.windowHandle);
        return vkCreateAndroidSurfaceKHR(instance, &create, nullptr, outSurface);
#else
        (void)instance;
        return VK_ERROR_EXTENSION_NOT_PRESENT;
#endif
    }

    return VK_ERROR_EXTENSION_NOT_PRESENT;
}

} // namespace ixvulkan
