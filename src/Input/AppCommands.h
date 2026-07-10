// AppCommands.h
#pragma once
#include <windows.h>
#define WM_TRAYICON (WM_APP + 1)
#define ID_TRAY_APP_ICON 1001

class AppCommands {
    public:
        static void ToggleFullscreen(HWND hWnd);

        static void ResetWindowLayoutAndEffects(HWND hWnd);

        static void AddTrayIcon(HWND hWnd);

        static void changeAppThemeToDarkMode(HWND hWnd, bool isDarkThemed);

        static void RemoveTrayIcon(HWND hWnd);

    private:
        // This remains private and inaccessible to the rest of the app
        static void SaveImageToDisk(HWND hWnd);


        // Only the InputManager class can call the method above
        friend class InputManager;
};
