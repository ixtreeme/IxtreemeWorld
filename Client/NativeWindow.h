#pragma once

#include "InputEvent.h"

#include <windows.h>
#include <cstdint>
#include <functional>

class NativeWindow
{
public:
    bool Create(HINSTANCE instance, const char* title, uint32_t width, uint32_t height);
    void Destroy();

    bool PumpMessages();
    bool ConsumeResize(uint32_t& width, uint32_t& height);
    void RequestClose();
    using InputCallback = std::function<void(const InputEvent&)>;
    void SetInputCallback(InputCallback cb);

    HWND GetHwnd() const { return m_hwnd; }
    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }

private:
    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    void DispatchInput(const InputEvent& event);

    HINSTANCE m_instance = nullptr;
    HWND m_hwnd = nullptr;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    bool m_resizePending = false;
    uint16_t m_pendingHighSurrogate = 0;
    InputCallback m_inputCallback;
};
