#include "platform/LegacyControlPromptBackend.h"

#ifdef PSP_PLATFORM

std::string legacyControlPromptLabel(const GameSettings &settings, LegacyControlAction action)
{
    (void)settings;
    switch (action)
    {
    case LegacyControlAction::Attack: return "L";
    case LegacyControlAction::Use: return "R";
    case LegacyControlAction::Jump: return "D-Up";
    case LegacyControlAction::Inventory: return "Select";
    case LegacyControlAction::Drop: return "Triangle";
    }
    return std::string();
}

#endif // PSP_PLATFORM
