// AppCommands.h
#pragma once
#include <windows.h>
#include <string>
#include <vector>
#define WM_TRAYICON (WM_APP + 1)
#define ID_TRAY_APP_ICON 1001

class AppCommands {
    public:
        static void ToggleFullscreen(HWND hWnd);

        static void ResetWindowLayoutAndEffects(HWND hWnd);

        static void AddTrayIcon(HWND hWnd);

        static void changeAppThemeToDarkMode(HWND hWnd, bool isDarkThemed);
        static void changeAppCornerPreference(HWND hWnd, DWORD cornerStyle);
        static void changeAppThemeFactor(HWND hWnd, float newFactor);
        static void changeAppBackdropType(HWND hWnd, DWORD newType);

        // Slideshow
        static void toggleSlideshow(HWND hWnd);      // Ctrl+F1: start / stop
        static void pauseResumeSlideshow(HWND hWnd); // Space: pause / resume
        static void stopSlideshow(HWND hWnd);        // also called from WM_TIMER (end of playlist)

        static void RemoveTrayIcon(HWND hWnd);

        // File clipboard / shell operations (used by thumbnail panel context menu)
        static void CopyFileToClipboard(HWND hWnd, const std::wstring &path, bool cut = false);
        static void CopyFilesToClipboard(HWND hWnd, const std::vector<std::wstring> &paths, bool cut = false);
        static void DeleteFileToRecycleBin(const std::wstring &path);
        static void DeleteFilesToRecycleBin(const std::vector<std::wstring> &paths);
        static void PasteFilesFromClipboard(HWND hWnd, const std::wstring &targetDir);
        static bool ClipboardHasFiles();

    private:
        // This remains private and inaccessible to the rest of the app
        static void SaveImageToDisk(HWND hWnd);
        static void CopyImageToClipboard(HWND hWnd);

        // Sets the currently displayed file as the desktop wallpaper.
        // position: Constants::Wallpaper::FILL .. SPAN — mapped onto the native
        // DESKTOP_WALLPAPER_POSITION enum inside the .cpp so this header stays
        // free of <shobjidl.h>.
        static void SetDesktopWallpaper(HWND hWnd, int position);


        // Only the InputManager class can call the method above
        friend class InputManager;
};
