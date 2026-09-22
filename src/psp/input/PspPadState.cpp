#ifdef PSP_PLATFORM

#include "PspPadState.h"
#include <pspctrl.h>
#include <cmath>
#include <algorithm>

namespace
{
    static PspPadSnapshot s_snapshot;
    static std::uint32_t s_lastButtons = 0;
    static std::uint32_t s_latchedPressed = 0;

    static int s_cursorX = 240;
    static int s_cursorY = 136;

    float applyDeadzone(float value, float deadzone)
    {
        if (std::abs(value) < deadzone)
            return 0.0f;
        float sign = (value > 0.0f) ? 1.0f : -1.0f;
        return sign * ((std::abs(value) - deadzone) / (1.0f - deadzone));
    }
}

namespace PspPadState
{
    void init()
    {
        sceCtrlSetSamplingCycle(0);
        sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
        s_cursorX = 240;
        s_cursorY = 136;
        s_lastButtons = 0;
        s_latchedPressed = 0;
    }

    void update()
    {
        SceCtrlData pad;
        if (sceCtrlPeekBufferPositive(&pad, 1) > 0)
        {
            s_snapshot.connected = true;
            s_snapshot.held = pad.Buttons;

            std::uint32_t newlyPressed = pad.Buttons & ~s_lastButtons;
            s_snapshot.pressed = newlyPressed;
            s_snapshot.released = s_lastButtons & ~pad.Buttons;
            s_latchedPressed |= newlyPressed;
            s_lastButtons = pad.Buttons;

            // Map analog stick (-1.0 to 1.0)
            float rawX = (static_cast<float>(pad.Lx) - 128.0f) / 128.0f;
            float rawY = (static_cast<float>(pad.Ly) - 128.0f) / 128.0f;

            s_snapshot.leftX = applyDeadzone(rawX, 0.18f);
            s_snapshot.leftY = applyDeadzone(rawY, 0.18f);

            // Update virtual menu cursor
            if (std::abs(s_snapshot.leftX) > 0.01f || std::abs(s_snapshot.leftY) > 0.01f)
            {
                s_cursorX += static_cast<int>(s_snapshot.leftX * 4.0f);
                s_cursorY += static_cast<int>(s_snapshot.leftY * 4.0f);
                s_cursorX = std::max(0, std::min(480, s_cursorX));
                s_cursorY = std::max(0, std::min(272, s_cursorY));
            }
        }
        else
        {
            s_snapshot.connected = false;
        }
    }

    const PspPadSnapshot& getSnapshot()
    {
        return s_snapshot;
    }

    std::uint32_t consumePressed()
    {
        std::uint32_t pressed = s_latchedPressed;
        s_latchedPressed = 0;
        return pressed;
    }

    void clearLatches()
    {
        s_latchedPressed = 0;
    }

    int getCursorX()
    {
        return s_cursorX;
    }

    int getCursorY()
    {
        return s_cursorY;
    }

    void setCursorPosition(int x, int y)
    {
        s_cursorX = std::max(0, std::min(480, x));
        s_cursorY = std::max(0, std::min(272, y));
    }
}

#endif // PSP_PLATFORM
