#pragma once

#include "NativeWindow.h"

#include <android/native_window.h>
#include <android_native_app_glue.h>

class NativeWindow_Android final : public NativeWindow
{
public:
    explicit NativeWindow_Android(android_app* app);
    ~NativeWindow_Android() override;

    bool WaitForWindow();

    bool PumpMessages() override;
    bool ConsumeResize(uint32_t& width, uint32_t& height) override;
    void RequestClose() override;
    void SetInputCallback(InputCallback cb) override;
    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
    VkResult CreateVulkanSurface(VkInstance instance, VkSurfaceKHR* outSurface) override;
    const char* GetVulkanSurfaceExtensionName() const override { return "VK_KHR_android_surface"; }

    bool HasNativeWindow() const { return m_nativeWindow != nullptr; }

private:
    static void OnAppCmd(android_app* app, int32_t cmd);
    static int32_t OnInputEvent(android_app* app, AInputEvent* event);

    void HandleCmd(int32_t cmd);
    int32_t HandleInputEvent(AInputEvent* event);

    android_app* m_app = nullptr;
    ANativeWindow* m_nativeWindow = nullptr;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    bool m_resizePending = false;
    bool m_shouldClose = false;
    InputCallback m_inputCallback;
};
