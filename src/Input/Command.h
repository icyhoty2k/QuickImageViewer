#pragma once
#include <windows.h>

enum class Command {
    None,
    NextImage,
    PrevImage,
    ToggleInvert,
    ToggleGrayscale,
    ToggleSepia,
    ToggleFullscreen,
    ResetAll,
    SaveImage,
    HideToTray
};

class InputManager {
    public:
        // This is the only public "door"
        static void handleKeyboard(HWND hWnd, WPARAM wParam);

    private:
        // These are hidden from the rest of the app
        static Command ResolveKeyboardKeys(UINT key);

        static void ExecuteKeyboardShortcutCommand(HWND hWnd, Command cmd);
};
