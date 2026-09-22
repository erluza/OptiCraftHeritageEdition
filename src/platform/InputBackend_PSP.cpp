#include "platform/Input.h"

#ifdef PSP_PLATFORM
#include "psp/input/PspPadState.h"
#include <pspctrl.h>

namespace
{
std::uint32_t mapTextButtons(std::uint32_t bits)
{
    std::uint32_t value = 0;
    if (bits & PSP_CTRL_LEFT)     value |= PLATFORM_TEXT_LEFT;
    if (bits & PSP_CTRL_RIGHT)    value |= PLATFORM_TEXT_RIGHT;
    if (bits & PSP_CTRL_UP)       value |= PLATFORM_TEXT_UP;
    if (bits & PSP_CTRL_DOWN)     value |= PLATFORM_TEXT_DOWN;
    if (bits & PSP_CTRL_CROSS)    value |= PLATFORM_TEXT_TYPE;
    if (bits & PSP_CTRL_SQUARE)   value |= PLATFORM_TEXT_BACK;
    if (bits & PSP_CTRL_SELECT)   value |= PLATFORM_TEXT_SPACE;
    if (bits & PSP_CTRL_TRIANGLE) value |= PLATFORM_TEXT_SHIFT;
    if (bits & PSP_CTRL_START)    value |= PLATFORM_TEXT_ENTER;
    if (bits & PSP_CTRL_CIRCLE)   value |= PLATFORM_TEXT_CLOSE;
    return value;
}
}

PlatformTextInputSnapshot platformTextInputSnapshot(int port)
{
    (void)port;
    PlatformTextInputSnapshot out;
    const PspPadSnapshot& pad = PspPadState::getSnapshot();
    out.connected = pad.connected;
    out.held = mapTextButtons(pad.held);
    out.pressed = mapTextButtons(PspPadState::consumePressed());
    out.pointerValid = true;
    out.pointerX = PspPadState::getCursorX();
    out.pointerY = PspPadState::getCursorY();
    out.pointerWidth = 480;
    out.pointerHeight = 272;
    return out;
}

PlatformGamepadSnapshot platformGamepadSnapshot(int port)
{
    (void)port;
    PlatformGamepadSnapshot out;
    const PspPadSnapshot& pad = PspPadState::getSnapshot();
    out.connected = pad.connected;
    out.leftX = pad.leftX;
    out.leftY = pad.leftY;
    return out;
}

PlatformGamepadSnapshot platformRawGamepadSnapshot(int port)
{
    return platformGamepadSnapshot(port);
}

int platformMenuPad()
{
    return 0;
}

bool platformMenuPointerActive()
{
    return true;
}

bool platformMenuCursorVisible()
{
    return true;
}

void platformSetMenuCursor(int x, int y)
{
    PspPadState::setCursorPosition(x, y);
}

const PlatformKeyboardHints& platformKeyboardHints()
{
    static const PlatformKeyboardHints hints = {
        { "X:select O:back D-pad:nav",
          "Triangle:shift Square:del",
          "Select:space Start:enter" },
        3
    };
    return hints;
}
#endif
