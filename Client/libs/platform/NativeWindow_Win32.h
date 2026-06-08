#pragma once

#include "NativeWindow.h"

#include <functional>
#include <windows.h>

class NativeWindow_Win32 final : public NativeWindow
{
public:
    using MessageCallback = std::function<bool(HWND, UINT, WPARAM, LPARAM, LRESULT&)>;

    bool Create(HINSTANCE instance, const char* title, uint32_t width, uint32_t height);
    void Destroy();

    bool PumpMessages() override;
    bool ConsumeResize(uint32_t& width, uint32_t& height) override;
    void RequestClose() override;
    void SetTitle(const std::string& title) override;
    void SetInputCallback(InputCallback cb) override;
    void SetFileDropCallback(FileDropCallback cb) override;
    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
    VkResult CreateVulkanSurface(VkInstance instance, VkSurfaceKHR* outSurface) override;
    const char* GetVulkanSurfaceExtensionName() const override { return "VK_KHR_win32_surface"; }

    HWND GetHwnd() const { return m_hwnd; }
    void SetMessageCallback(MessageCallback cb);

private:
    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    void DispatchInput(const InputEvent& event);
    void DispatchFileDrop(const std::vector<std::string>& paths);

    HINSTANCE m_instance = nullptr;
    HWND m_hwnd = nullptr;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    bool m_resizePending = false;
    uint16_t m_pendingHighSurrogate = 0;
    InputCallback m_inputCallback;
    FileDropCallback m_fileDropCallback;
    MessageCallback m_messageCallback;
};
