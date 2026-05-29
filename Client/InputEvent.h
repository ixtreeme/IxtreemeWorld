#pragma once

#include <cstdint>

enum Key
{
    Key_None,
    Key_A,
    Key_B,
    Key_C,
    Key_D,
    Key_E,
    Key_F,
    Key_G,
    Key_H,
    Key_I,
    Key_J,
    Key_K,
    Key_L,
    Key_M,
    Key_N,
    Key_O,
    Key_P,
    Key_Q,
    Key_R,
    Key_S,
    Key_T,
    Key_U,
    Key_V,
    Key_W,
    Key_X,
    Key_Y,
    Key_Z,
    Key_0,
    Key_1,
    Key_2,
    Key_3,
    Key_4,
    Key_5,
    Key_6,
    Key_7,
    Key_8,
    Key_9,
    Key_Enter,
    Key_Escape,
    Key_Backspace,
    Key_Tab,
    Key_Space,
    Key_Left,
    Key_Right,
    Key_Up,
    Key_Down,
    Key_Shift,
    Key_Control,
    Key_Delete,
    Key_Home,
    Key_End
};

enum MouseButton
{
    MouseButton_Left,
    MouseButton_Right,
    MouseButton_Middle
};

struct InputEvent
{
    enum Type
    {
        MouseMove,
        MouseDown,
        MouseUp,
        MouseWheel,
        KeyDown,
        KeyUp,
        Char,

        // Reserved for the future Android/touch translator; not dispatched by Win32 yet.
        TouchDown,
        TouchMove,
        TouchUp
    };

    Type type = MouseMove;
    int x = 0;
    int y = 0;
    MouseButton button = MouseButton_Left;
    int wheelDelta = 0;
    Key key = Key_None;
    uint32_t codepoint = 0;
    int touchId = 0;
};
