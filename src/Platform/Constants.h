#pragma once
#include <iterator>

namespace Constants {
    constexpr const wchar_t *BASE_NAME = L"QuickImageViewer";

    constexpr const wchar_t *APP_CREATOR = L"Ivan Hristov Yanev";
    constexpr const wchar_t *APP_HELP_FOOTER = L"Copyright® 06.2026 All rights reserved";
    constexpr const wchar_t *APP_TASKBAR_NAME = L"QIV";
    constexpr const wchar_t *APP_VERSION = L"1.0.0"; // major.minor.patch
    //Saveable options
    static constexpr float ZOOM_STEP = 1.1f; // +/- keys and ctrl+wheel
    constexpr float COLOR_ADJUST_STEP = 0.1f; // step for brightness contrast and saturation
    constexpr float DEFAULT_SATURATION = 1.0f;
    constexpr float DEFAULT_BRIGHTNESS = 0.0f;
    constexpr float DEFAULT_CONTRAST = 1.0f;
    static constexpr float ZOOM_CLICK = 3.0f; // left click zoom multiplier
    static constexpr int OPACITY_STEP = 10; // left click zoom multiplier from 10 to 255 step is 10
    constexpr bool DEFAULT_SHOW_OVERLAY = true;


    constexpr int BASE_WIDTH = 1200;
    constexpr int BASE_HEIGHT = 800;

    inline bool SWAP_MOUSE_BUTTONS = true; // Set this to true to swap Left and Right mouse button functions

    //==========================Cache optimization====================================
    constexpr const int VRAM_CACHE_IMAGES_COUNT = 10;
    constexpr const int VRAM_CACHE_SVG_COUNT = 10;
    constexpr const int PRELOAD_LOOKASIDE_COUNT = 1;
    constexpr const int PRELOAD_TIMER_COUNTDOWN = 150; // this is used to delay preloading if user scrolls very fast
    //==========================Cache optimization====================================
    //end Saveable options

    // Custom window messages
    constexpr UINT WM_QIV_PENDING_UPLOADS = WM_USER + 1; // Posted by background decoder thread
    constexpr UINT WM_QIV_REPAINT = WM_USER + 2; // Signal to UI thread that bitmap is ready
    constexpr UINT WM_QIV_SVG_READY = WM_USER + 3; // Posted by IO thread when SVG bytes are loaded
    // DWM API Attributes
    constexpr DWORD DWMWA_WINDOW_CORNER_PREFERENCE = 33;

    // =============================================================================
    constexpr const wchar_t *APP_NAME = BASE_NAME;
    constexpr const wchar_t *WINDOW_CLASS_NAME = BASE_NAME;


    namespace Registry {
        // Switch this between HKEY_CURRENT_USER and HKEY_LOCAL_MACHINE
        inline HKEY ROOT_HIVE = HKEY_CURRENT_USER;

        // Base path for application-specific user preferences (HKEY_CURRENT_USER)
        constexpr const wchar_t *ROOT_KEY = L"Software\\QuickImageViewer";

        // --- Settings (Stored under ROOT_KEY) ---
        // Boolean flag to show/hide the on-screen information text overlay
        constexpr const wchar_t *OVERLAY_ENABLED = L"ShowOverlay";
        // Path string to the last directory accessed by the user
        constexpr const wchar_t *LAST_FOLDER = L"LastFolder";

        // --- System Integration (Open With & Startup) ---
        // Registry path to define the shell command for opening associated files
        constexpr const wchar_t *OPEN_WITH_COMMAND = L"Software\\Classes\\Applications\\QuickImageViewer.exe\\shell\\open\\command";
        // Base registry key for the application's file association definition
        constexpr const wchar_t *OPEN_WITH_ROOT = L"Software\\Classes\\Applications\\QuickImageViewer.exe";
        // Key containing a list of supported file extensions (e.g., .jpg, .png)
        constexpr const wchar_t *OPEN_WITH_TYPES = L"Software\\Classes\\Applications\\QuickImageViewer.exe\\SupportedTypes";

        // Windows Auto-start path (Standard location for user-specific startup applications)
        constexpr const wchar_t *RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
        // Value name used for the application's auto-start entry in the Run key
        constexpr const wchar_t *RUN_VALUE_NAME = L"QuickImageViewer";

        constexpr const wchar_t *SUPPORTED_EXTENSIONS[] = {
            L".jpg", L".jpeg", L".png", L".webp", L".bmp", L".gif", L".tiff", L".tif",
            L".ico", L".heic", L".heif", L".jxr", L".wdp", L".hdp", L".dds",
            L".dng", L".cr2", L".cr3", L".nef", L".arw",
            L".svg", L".avif", L".tga"
        };

        // Helper to get the number of elements
        constexpr size_t SUPPORTED_EXTENSIONS_COUNT = std::size(SUPPORTED_EXTENSIONS);
    }

    namespace ViewModes {
        enum class ViewMode {
            FitToView_PreserveAspectRatio = 1,
            FitToWidth_DoNotPreserveAspectRatio = 2,
            FitToHeight_DoNotPreserveAspectRatio = 3,
            FitToWindow_DoNotPreserveAspectRatio = 4,
            OriginalImageSize_PreserveAspectRatio = 5
        };

        constexpr ViewMode defaultViewMode = ViewMode::FitToView_PreserveAspectRatio;
    }
}
