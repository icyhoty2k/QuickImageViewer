#include "Command.h"
#include "Shortcuts.h"


//stage 1 keyboard handling
Command InputManager::ResolveKeyboardKeys(UINT key) {
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;

    switch (key) {
        case Shortcuts::SC_NAV_NEXT: return Command::NextImage;
        case Shortcuts::SC_NAV_PREV: return Command::PrevImage;
        case Shortcuts::ImageEffects::SC_COLOR_INVERT: return Command::ToggleInvert;
        case VK_ADD: return alt ? Command::ResetAll : Command::ToggleGrayscale;
        case VK_DELETE: return shift ? Command::ResetAll : Command::ToggleGrayscale;
        case 'S': if (ctrl) return Command::SaveImage;
            break;
        case VK_ESCAPE: return Command::HideToTray;
    }
    return Command::None;
}
