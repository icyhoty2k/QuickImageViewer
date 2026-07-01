#include "Command.h"
#include "../AppState.h"
#include "../Platform/FileHandler.h"

extern AppState g_app;

// the final of a 3 stage keyboard hadling
void InputManager::handleKeyboard(HWND hWnd, WPARAM wParam) {
    // 1.Call the private static method Resolve
    Command cmd = ResolveKeyboardKeys(static_cast<UINT>(wParam));

    // 2. Call the private static method Execute
    if (cmd != Command::None) {
        ExecuteKeyboardShortcutCommand(hWnd, cmd);
    }
}

// stage 2 keyboard handling
void InputManager::ExecuteKeyboardShortcutCommand(HWND hWnd, Command cmd) {
    switch (cmd) {
        case Command::NextImage:
            if (!g_app.playlist.empty()) {
                LoadImageIndex(hWnd, (g_app.currentIndex + 1) % (int) g_app.playlist.size());
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            break;

        case Command::PrevImage:
            if (!g_app.playlist.empty()) {
                int size = (int) g_app.playlist.size();
                LoadImageIndex(hWnd, (g_app.currentIndex - 1 + size) % size);
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            break;

        case Command::ToggleInvert:
            g_app.WakeUpAndApplyEffects(hWnd, g_app.effectInvert);
            break;

        case Command::ToggleGrayscale:
            g_app.WakeUpAndApplyEffects(hWnd, g_app.effectGrayscale);
            break;

        default:
            break;
    }
}
