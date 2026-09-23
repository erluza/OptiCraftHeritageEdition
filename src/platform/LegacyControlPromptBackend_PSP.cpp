#include "platform/LegacyControlPromptBackend.h"

#ifdef PSP_PLATFORM

std::string legacyControlPromptLabel(const GameSettings &settings, LegacyControlAction action)
{
    (void)settings;
    switch (action)
    {
    case LegacyControlAction::Attack: return "Square";
    case LegacyControlAction::Use: return "Circle";
    case LegacyControlAction::Jump: return "Cross";
    case LegacyControlAction::Inventory: return "Triangle";
    case LegacyControlAction::Drop: return "Select";
    }
    return std::string();
}

#endif // PSP_PLATFORM
