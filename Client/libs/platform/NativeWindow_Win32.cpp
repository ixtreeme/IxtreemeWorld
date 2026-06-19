#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "NativeWindow_Win32.h"

#include <shellapi.h>
#include <string>
#include <utility>
#include <vector>
#include <windowsx.h>

namespace
{
constexpr const char* kWindowClassName = "IxtreemeEngineWindow";

bool IsEngineMouseInputMessage(UINT message)
{
    switch (message)
    {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
    case WM_MOUSEWHEEL:
        return true;
    default:
        return false;
    }
}

bool IsEngineKeyboardInputMessage(UINT message)
{
    switch (message)
    {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
        return true;
    default:
        return false;
    }
}

Key TranslateVirtualKey(WPARAM vk)
{
    if (vk >= 'A' && vk <= 'Z')
        return static_cast<Key>(Key_A + (vk - 'A'));

    if (vk >= '0' && vk <= '9')
        return static_cast<Key>(Key_0 + (vk - '0'));

    switch (vk)
    {
    case VK_RETURN: return Key_Enter;
    case VK_ESCAPE: return Key_Escape;
    case VK_BACK: return Key_Backspace;
    case VK_TAB: return Key_Tab;
    case VK_SPACE: return Key_Space;
    case VK_LEFT: return Key_Left;
    case VK_RIGHT: return Key_Right;
    case VK_UP: return Key_Up;
    case VK_DOWN: return Key_Down;
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
        return Key_Shift;
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
        return Key_Control;
    case VK_DELETE: return Key_Delete;
    case VK_HOME: return Key_Home;
    case VK_END: return Key_End;
    case VK_F1: return Key_F1;
    case VK_F2: return Key_F2;
    case VK_F4: return Key_F4;
    case VK_F5: return Key_F5;
    case VK_F6: return Key_F6;
    case VK_F7: return Key_F7;
    case VK_F8: return Key_F8;
    default: return Key_None;
    }
}

uint32_t DecodeSurrogatePair(uint16_t high, uint16_t low)
{
    return 0x10000u + ((static_cast<uint32_t>(high) - 0xD800u) << 10) +
        (static_cast<uint32_t>(low) - 0xDC00u);
}

std::string WideToUtf8(const wchar_t* text)
{
    if (!text)
        return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1)
        return {};
    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), needed, nullptr, nullptr);
    return out;
}
}

bool NativeWindow_Win32::GetPrimaryMonitorResolution(uint32_t& width, uint32_t& height)
{
    const int nativeWidth = GetSystemMetrics(SM_CXSCREEN);
    const int nativeHeight = GetSystemMetrics(SM_CYSCREEN);
    if (nativeWidth <= 0 || nativeHeight <= 0)
        return false;

    width = static_cast<uint32_t>(nativeWidth);
    height = static_cast<uint32_t>(nativeHeight);
    return true;
}

bool NativeWindow_Win32::Create(HINSTANCE instance, const char* title, uint32_t width, uint32_t height)
{
    m_instance = instance;
    m_width = width;
    m_height = height;

    WNDCLASSEX wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &NativeWindow_Win32::StaticWndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClassName;

    if (!RegisterClassEx(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    const DWORD style = WS_OVERLAPPEDWINDOW;
    int x = CW_USEDEFAULT;
    int y = CW_USEDEFAULT;
    int windowWidth = 0;
    int windowHeight = 0;

    uint32_t nativeWidth = 0;
    uint32_t nativeHeight = 0;
    const bool nativeResolutionWindow =
        GetPrimaryMonitorResolution(nativeWidth, nativeHeight) &&
        width == nativeWidth &&
        height == nativeHeight;

    if (nativeResolutionWindow)
    {
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        const HMONITOR primaryMonitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        if (GetMonitorInfo(primaryMonitor, &monitor))
        {
            x = monitor.rcMonitor.left;
            y = monitor.rcMonitor.top;
            windowWidth = monitor.rcMonitor.right - monitor.rcMonitor.left;
            windowHeight = monitor.rcMonitor.bottom - monitor.rcMonitor.top;
        }
    }
    if (windowWidth <= 0 || windowHeight <= 0)
    {
        RECT rect{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        AdjustWindowRect(&rect, style, FALSE);
        windowWidth = rect.right - rect.left;
        windowHeight = rect.bottom - rect.top;
    }

    m_hwnd = CreateWindowEx(
        0,
        kWindowClassName,
        title,
        style,
        x,
        y,
        windowWidth,
        windowHeight,
        nullptr,
        nullptr,
        instance,
        this);

    if (!m_hwnd)
        return false;

    ShowWindow(m_hwnd, SW_MAXIMIZE);
    UpdateWindow(m_hwnd);
    DragAcceptFiles(m_hwnd, TRUE);

    RECT client{};
    if (GetClientRect(m_hwnd, &client))
    {
        m_width = static_cast<uint32_t>(client.right - client.left);
        m_height = static_cast<uint32_t>(client.bottom - client.top);
    }
    m_resizePending = false;
    return true;
}

void NativeWindow_Win32::Destroy()
{
    if (m_hwnd)
    {
        DragAcceptFiles(m_hwnd, FALSE);
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

bool NativeWindow_Win32::PumpMessages()
{
    MSG msg{};
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        if (msg.message == WM_QUIT)
            return false;

        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return true;
}

void NativeWindow_Win32::RequestClose()
{
    PostQuitMessage(0);
}

void NativeWindow_Win32::SetTitle(const std::string& title)
{
    if (m_hwnd)
        SetWindowTextA(m_hwnd, title.c_str());
}

void NativeWindow_Win32::SetInputCallback(InputCallback cb)
{
    m_inputCallback = std::move(cb);
}

void NativeWindow_Win32::SetFileDropCallback(FileDropCallback cb)
{
    m_fileDropCallback = std::move(cb);
}

void NativeWindow_Win32::SetMessageCallback(MessageCallback cb)
{
    m_messageCallback = std::move(cb);
}

bool NativeWindow_Win32::ConsumeResize(uint32_t& width, uint32_t& height)
{
    if (!m_resizePending)
        return false;

    m_resizePending = false;
    width = m_width;
    height = m_height;
    return true;
}

VkResult NativeWindow_Win32::CreateVulkanSurface(VkInstance instance, VkSurfaceKHR* outSurface)
{
    VkWin32SurfaceCreateInfoKHR create{};
    create.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    create.hinstance = m_instance;
    create.hwnd = m_hwnd;
    return vkCreateWin32SurfaceKHR(instance, &create, nullptr, outSurface);
}

void NativeWindow_Win32::DispatchInput(const InputEvent& event)
{
    if (m_inputCallback)
        m_inputCallback(event);
}

void NativeWindow_Win32::DispatchFileDrop(const std::vector<std::string>& paths)
{
    if (m_fileDropCallback)
        m_fileDropCallback(paths);
}

LRESULT CALLBACK NativeWindow_Win32::StaticWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    NativeWindow_Win32* window = nullptr;

    if (message == WM_NCCREATE)
    {
        auto* create = reinterpret_cast<CREATESTRUCTA*>(lParam);
        window = static_cast<NativeWindow_Win32*>(create->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
    }
    else
    {
        window = reinterpret_cast<NativeWindow_Win32*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }

    if (window)
        return window->WndProc(hwnd, message, wParam, lParam);

    return DefWindowProc(hwnd, message, wParam, lParam);
}

LRESULT NativeWindow_Win32::WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (m_messageCallback)
    {
        LRESULT result = 0;
        if (m_messageCallback(hwnd, message, wParam, lParam, result))
        {
            if (IsEngineMouseInputMessage(message) || IsEngineKeyboardInputMessage(message))
            {
                // ImGui may consume native input while the engine still needs
                // the platform-level event. The editor input router applies its own
                // WantCapture gate before viewport tools see the event.
            }
            else
            {
                return result;
            }
        }
    }

    switch (message)
    {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT paint{};
        BeginPaint(hwnd, &paint);
        EndPaint(hwnd, &paint);
        return 0;
    }

    case WM_DROPFILES:
    {
        HDROP drop = reinterpret_cast<HDROP>(wParam);
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        std::vector<std::string> paths;
        paths.reserve(count);
        for (UINT i = 0; i < count; ++i)
        {
            const UINT length = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring path(static_cast<size_t>(length) + 1u, L'\0');
            DragQueryFileW(drop, i, path.data(), length + 1);
            paths.push_back(WideToUtf8(path.c_str()));
        }
        DragFinish(drop);
        DispatchFileDrop(paths);
        return 0;
    }

    case WM_MOUSEMOVE:
    {
        InputEvent event{};
        event.type = InputEvent::MouseMove;
        event.x = GET_X_LPARAM(lParam);
        event.y = GET_Y_LPARAM(lParam);
        DispatchInput(event);
        return 0;
    }

    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    {
        SetCapture(hwnd);
        InputEvent event{};
        event.type = InputEvent::MouseDown;
        event.x = GET_X_LPARAM(lParam);
        event.y = GET_Y_LPARAM(lParam);
        event.button = message == WM_LBUTTONDOWN ? MouseButton_Left :
            (message == WM_RBUTTONDOWN ? MouseButton_Right : MouseButton_Middle);
        DispatchInput(event);
        return 0;
    }

    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
    {
        InputEvent event{};
        event.type = InputEvent::MouseUp;
        event.x = GET_X_LPARAM(lParam);
        event.y = GET_Y_LPARAM(lParam);
        event.button = message == WM_LBUTTONUP ? MouseButton_Left :
            (message == WM_RBUTTONUP ? MouseButton_Right : MouseButton_Middle);
        DispatchInput(event);

        if ((wParam & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON)) == 0)
            ReleaseCapture();
        return 0;
    }

    case WM_MOUSEWHEEL:
    {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd, &point);

        InputEvent event{};
        event.type = InputEvent::MouseWheel;
        event.x = point.x;
        event.y = point.y;
        event.wheelDelta = GET_WHEEL_DELTA_WPARAM(wParam);
        DispatchInput(event);
        return 0;
    }

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
    {
        const Key key = TranslateVirtualKey(wParam);
        if (key != Key_None)
        {
            InputEvent event{};
            event.type = (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) ?
                InputEvent::KeyDown : InputEvent::KeyUp;
            event.key = key;
            DispatchInput(event);
        }
        return 0;
    }

    case WM_CHAR:
    {
        const uint16_t ch = static_cast<uint16_t>(wParam);
        uint32_t codepoint = 0;

        if (ch >= 0xD800 && ch <= 0xDBFF)
        {
            m_pendingHighSurrogate = ch;
            return 0;
        }

        if (ch >= 0xDC00 && ch <= 0xDFFF)
        {
            if (m_pendingHighSurrogate != 0)
                codepoint = DecodeSurrogatePair(m_pendingHighSurrogate, ch);
            m_pendingHighSurrogate = 0;

            if (codepoint == 0)
                return 0;
        }
        else
        {
            m_pendingHighSurrogate = 0;
            codepoint = ch;
        }

        InputEvent event{};
        event.type = InputEvent::Char;
        event.codepoint = codepoint;
        DispatchInput(event);
        return 0;
    }

    case WM_SIZE:
        m_width = LOWORD(lParam);
        m_height = HIWORD(lParam);
        m_resizePending = true;
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        m_hwnd = nullptr;
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProc(hwnd, message, wParam, lParam);
    }
}
