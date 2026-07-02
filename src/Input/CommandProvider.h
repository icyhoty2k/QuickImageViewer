// CommandProvider.h
#pragma once
#include <windows.h>

class CommandProvider {
    public:
        static void ToggleFullscreen(HWND hWnd);

        static void ResetWindowLayoutAndEffects(HWND hWnd);

    private:
        // This remains private and inaccessible to the rest of the app
        static void SaveImageToDisk(HWND hWnd);


        // Only the InputManager class can call the method above
        friend class InputManager;
};
