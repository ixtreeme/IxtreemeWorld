#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include "NativeWindow_Android.h"
#include "Debug.h"

#include <android/input.h>

#include <utility>

NativeWindow_Android::NativeWindow_Android(android_app* app)
    : m_app(app)
{
    if (m_app)
    {
        m_app->userData = this;
        m_app->onAppCmd = &NativeWindow_Android::OnAppCmd;
        m_app->onInputEvent = &NativeWindow_Android::OnInputEvent;
    }
}

NativeWindow_Android::~NativeWindow_Android()
{
    if (m_app)
    {
        m_app->userData = nullptr;
        m_app->onAppCmd = nullptr;
        m_app->onInputEvent = nullptr;
    }
}

bool NativeWindow_Android::WaitForWindow()
{
    while (!m_nativeWindow && !m_shouldClose && m_app && !m_app->destroyRequested)
    {
        if (!PumpMessages())
            return false;
    }
    return m_nativeWindow != nullptr;
}

bool NativeWindow_Android::PumpMessages()
{
    int events = 0;
    android_poll_source* source = nullptr;
    int ident = ALooper_pollOnce(0, nullptr, &events, reinterpret_cast<void**>(&source));
    while (ident >= 0)
    {
        if (source)
            source->process(m_app, source);

        if (m_app && m_app->destroyRequested)
        {
            m_shouldClose = true;
            return false;
        }

        ident = ALooper_pollOnce(0, nullptr, &events, reinterpret_cast<void**>(&source));
    }

    return !m_shouldClose;
}

bool NativeWindow_Android::ConsumeResize(uint32_t& width, uint32_t& height)
{
    if (!m_resizePending)
        return false;

    m_resizePending = false;
    width = m_width;
    height = m_height;
    Tracenf("[NATIVEWINDOW] ConsumeResize: %ux%u (flag reset)", width, height);
    return true;
}

void NativeWindow_Android::RequestClose()
{
    m_shouldClose = true;
    if (m_app && m_app->activity)
        ANativeActivity_finish(m_app->activity);
}

void NativeWindow_Android::SetInputCallback(InputCallback cb)
{
    m_inputCallback = std::move(cb);
}

VkResult NativeWindow_Android::CreateVulkanSurface(VkInstance instance, VkSurfaceKHR* outSurface)
{
    if (!m_nativeWindow)
        return VK_ERROR_SURFACE_LOST_KHR;

    VkAndroidSurfaceCreateInfoKHR create{};
    create.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    create.window = m_nativeWindow;
    return vkCreateAndroidSurfaceKHR(instance, &create, nullptr, outSurface);
}

void NativeWindow_Android::OnAppCmd(android_app* app, int32_t cmd)
{
    auto* self = static_cast<NativeWindow_Android*>(app->userData);
    if (self)
        self->HandleCmd(cmd);
}

void NativeWindow_Android::HandleCmd(int32_t cmd)
{
    switch (cmd)
    {
    case APP_CMD_INIT_WINDOW:
        m_nativeWindow = m_app ? m_app->window : nullptr;
        if (m_nativeWindow)
        {
            const uint32_t newWidth = static_cast<uint32_t>(ANativeWindow_getWidth(m_nativeWindow));
            const uint32_t newHeight = static_cast<uint32_t>(ANativeWindow_getHeight(m_nativeWindow));
            const bool changed = newWidth != m_width || newHeight != m_height;
            Tracenf("[NATIVEWINDOW] APP_CMD_INIT_WINDOW: %ux%u (prev=%ux%u, pending=%d, changed=%d)",
                newWidth,
                newHeight,
                m_width,
                m_height,
                m_resizePending ? 1 : 0,
                changed ? 1 : 0);

            if (changed)
            {
                m_width = newWidth;
                m_height = newHeight;
                m_resizePending = true;
            }
        }
        break;

    case APP_CMD_TERM_WINDOW:
        Tracen("[NATIVEWINDOW] APP_CMD_TERM_WINDOW");
        m_nativeWindow = nullptr;
        break;

    case APP_CMD_WINDOW_RESIZED:
    case APP_CMD_CONFIG_CHANGED:
        if (m_nativeWindow)
        {
            const uint32_t newWidth = static_cast<uint32_t>(ANativeWindow_getWidth(m_nativeWindow));
            const uint32_t newHeight = static_cast<uint32_t>(ANativeWindow_getHeight(m_nativeWindow));
            const bool changed = newWidth != m_width || newHeight != m_height;
            Tracenf("[NATIVEWINDOW] %s: %ux%u (prev=%ux%u, pending=%d, changed=%d)",
                cmd == APP_CMD_WINDOW_RESIZED ? "APP_CMD_WINDOW_RESIZED" : "APP_CMD_CONFIG_CHANGED",
                newWidth,
                newHeight,
                m_width,
                m_height,
                m_resizePending ? 1 : 0,
                changed ? 1 : 0);

            if (changed)
            {
                m_width = newWidth;
                m_height = newHeight;
                m_resizePending = true;
            }
        }
        break;

    case APP_CMD_GAINED_FOCUS:
        Tracen("[NATIVEWINDOW] APP_CMD_GAINED_FOCUS");
        break;

    case APP_CMD_LOST_FOCUS:
        Tracen("[NATIVEWINDOW] APP_CMD_LOST_FOCUS");
        break;

    case APP_CMD_DESTROY:
        Tracen("[NATIVEWINDOW] APP_CMD_DESTROY");
        m_shouldClose = true;
        break;

    default:
        break;
    }
}

int32_t NativeWindow_Android::OnInputEvent(android_app* app, AInputEvent* event)
{
    auto* self = static_cast<NativeWindow_Android*>(app->userData);
    return self ? self->HandleInputEvent(event) : 0;
}

int32_t NativeWindow_Android::HandleInputEvent(AInputEvent* event)
{
    if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION)
        return 0;

    const int32_t action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
    InputEvent input{};
    if (action == AMOTION_EVENT_ACTION_DOWN)
        input.type = InputEvent::MouseDown;
    else if (action == AMOTION_EVENT_ACTION_UP)
        input.type = InputEvent::MouseUp;
    else if (action == AMOTION_EVENT_ACTION_MOVE)
        input.type = InputEvent::MouseMove;
    else
        return 0;

    input.x = static_cast<int>(AMotionEvent_getX(event, 0));
    input.y = static_cast<int>(AMotionEvent_getY(event, 0));
    input.button = MouseButton_Left;
    input.touchId = AMotionEvent_getPointerId(event, 0);

    if (m_inputCallback)
        m_inputCallback(input);
    return 1;
}
