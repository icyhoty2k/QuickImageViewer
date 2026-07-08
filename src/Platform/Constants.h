#pragma once
#include <iterator>
#include <d2d1.h>
#include <dwmapi.h>

namespace Constants {
    constexpr const wchar_t *BASE_NAME = L"QuickImageViewer";

    constexpr const wchar_t *APP_CREATOR = L"Ivan Hristov Yanev";
    constexpr const wchar_t *APP_HELP_FOOTER = L"Copyright® 06.2026 All rights reserved";
    constexpr const wchar_t *APP_TASKBAR_NAME = L"QIV";
    constexpr const wchar_t *APP_VERSION = L"1.0.0"; // major.minor.patch
    //Saveable options
    static constexpr float ZOOM_STEP = 1.1f; // +/- keys and ctrl+wheel

    constexpr float COLOR_ADJUST_STEP = 0.1f; // step for brightness contrast and saturation
    constexpr float DEFAULT_SATURATION = 1.0f; // the default i dont want change when not using it i want original picture
    constexpr float DEFAULT_BRIGHTNESS = 0.0f; // the default i dont want change when not using it i want original picture
    constexpr float DEFAULT_CONTRAST = 1.0f; // the default i dont want change when not using it i want original picture
    constexpr float MIN_MAX_SATURATION = 7.0f;
    constexpr float MIN_MAX_BRIGHTNESS = 1.0f;
    constexpr float MIN_MAX_CONTRAST = 7.0f;

    // ----- Gamma (ImageEffects: SC_COLOR_GAMMA_UP / SC_COLOR_GAMMA_DOWN) -----
    constexpr float DEFAULT_GAMMA = 1.0f; // neutral, no curve applied
    constexpr float GAMMA_STEP = 0.1f;
    constexpr float MIN_GAMMA = 0.1f; // avoid 0 or negative exponents
    constexpr float MAX_GAMMA = 4.0f;

    // ----- Solarize / Threshold (ImageEffects toggles) -----
    constexpr float SOLARIZE_THRESHOLD = 0.5f; // values above this get inverted
    constexpr float BW_THRESHOLD_LEVEL = 0.5f; // black/white cutover point

    // ----- Outline (D2D EdgeDetection) -----
    constexpr float OUTLINE_STRENGTH = 0.5f;
    constexpr float OUTLINE_BLUR_RADIUS = 0.0f;
    static constexpr float ZOOM_CLICK = 3.0f; // left click zoom multiplier
    static constexpr int OPACITY_STEP = 10; // left click zoom multiplier from 10 to 255 step is 10


    // Custom window messages
    constexpr UINT WM_QIV_CENTER_MSG_HIDE = WM_USER + 10; // Posted by WM_TIMER to hide center msg


    constexpr int BASE_WIDTH = 1200;
    constexpr int BASE_HEIGHT = 800;

    inline bool SWAP_MOUSE_BUTTONS = true; // Set this to true to swap Left and Right mouse button functions
    // =========================================================================
    // CACHE WINDOW AND CURRENT DIR WINDOW
    // =========================================================================
    constexpr float CACHE_THUMB_WIDTH = 120.0f;
    constexpr float CACHE_THUMB_HEIGHT = 80.0f;
    constexpr float CACHE_THUMB_SPACING = 18.0f;
    constexpr float CACHE_THUMB_MARGIN = 20.0f;
    constexpr BYTE CACHE_WINDOW_OPACITY = 210;
    constexpr float CACHE_WINDOW_MOUSE_WHEEL_SPEED = 120.0f;
    constexpr int8_t CACHE_WINDOW_MOUSE_WHEEL_DIRECTION = 1; // 1 is forward -1 is reverse
    constexpr int CACHE_WINDOW_THICKNESS = 120;
    constexpr int8_t CACHE_WINDOW_POSITION = 2; // /0 top /1 right /2 bottom /3 left

    //   position 0 : centered floating panel (80 % wide, thumb-height tall)
    //   position 1 : top edge strip (full width)
    //   position 2 : right edge strip (full height)
    //   position 3 : bottom edge strip (full width)
    //   position 4 : left edge strip (full height)
    constexpr int8_t CURRENT_DIR_WINDOW_POSITION = 1; // /0 top /1 right /2 bottom /3 left


    namespace CacheColors {
        const D2D1::ColorF::Enum BACKGROUND = D2D1::ColorF::Black; // Or your custom 0.08f, 0.08f, 0.08f
        const D2D1::ColorF::Enum SELECTION_BORDER = D2D1::ColorF::LightGreen;
        const float SELECTION_BORDER_THICKNESS = 3.0f;
        const D2D1::ColorF::Enum HOVER = D2D1::ColorF::White;
        const float HOVER_THICKNESS = 1.0f;
        const D2D1::ColorF::Enum PLACEHOLDER = D2D1::ColorF::DarkSlateGray;
    }

    //==========================Cache optimization====================================
    constexpr const int VRAM_CACHE_IMAGES_COUNT = 30;
    constexpr const int VRAM_CACHE_SVG_COUNT = 10;
    // Maximum number of small dir-panel thumbnails kept in VRAM.
    // Each entry is ~CACHE_THUMB_WIDTH * CACHE_THUMB_HEIGHT * 4 bytes ≈ 37 KB,
    // so 256 entries ≈ 9 MB — safe even on the RTX 3060 with 12 GB GDDR6.
    constexpr const int DIR_THUMB_CACHE_MAX = 256;
    constexpr const int PRELOAD_LOOKASIDE_COUNT = 1;
    constexpr const int PRELOAD_TIMER_COUNTDOWN = 150; // this is used to delay preloading if user scrolls very fast
    //==========================Cache optimization====================================
    //end Saveable options

    // =========================================================================
    // Folder History (HistoryWindow)
    // =========================================================================
    constexpr int HISTORY_MAX_DIRS = 10; // how many folders to remember — change here
    constexpr int HISTORY_ROW_HEIGHT = 28; // px at 96 DPI per history row
    constexpr int HISTORY_PADDING = 16; // px at 96 DPI inner padding
    constexpr int HISTORY_FONT_SIZE = 14; // pt at 96 DPI body font

    // Custom window messages
    constexpr UINT WM_QIV_PENDING_UPLOADS = WM_USER + 1; // Posted by background decoder thread
    constexpr UINT WM_QIV_REPAINT = WM_USER + 2; // Signal to UI thread that bitmap is ready
    constexpr UINT WM_QIV_SVG_READY = WM_USER + 3; // Posted by IO thread when SVG bytes are loaded
    // DWM API Attributes
    constexpr DWORD DWMWA_WINDOW_CORNER_PREFERENCES = DWMWA_WINDOW_CORNER_PREFERENCE;
    // 0 (DWMWCP_DEFAULT): Let Windows decide. 1 (DWMWCP_DONOTROUND): Square corners. 2 (DWMWCP_ROUND): Standard rounded corners. 3 (DWMWCP_ROUNDSMALL): Slightly rounded corners.
    constexpr DWORD APP_CORNER_PREFERENCES = DWMWCP_DEFAULT;

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

    namespace Overlay {
        constexpr bool DEFAULT_SHOW_OVERLAY = true;
        constexpr const bool COMPACT_OVERLAY_MODE = true; // true → 1-line, false → 2-line

        // =========================================================================
        // Overlay — Center-Center message queue (MID_CENTER slot)
        // =========================================================================
        // How long the center-center notification stays visible before auto-hiding (ms)
        constexpr UINT MSG_CENTER_DISPLAY_MS = 1500;

        // Center-center text color  (R, G, B, A)
        constexpr float MSG_CENTER_COLOR_R = 1.0f;
        constexpr float MSG_CENTER_COLOR_G = 0.85f;
        constexpr float MSG_CENTER_COLOR_B = 0.20f;
        constexpr float MSG_CENTER_COLOR_A = 1.0f;

        // Center-center font size (pt). Other slots use the RendererD2D default size.
        constexpr float MSG_ALL_BUT_CENTER_FONT_SIZE = 14.0f;
        constexpr float MSG_CENTER_FONT_SIZE = 14.0f;

        // =========================================================================
        // Overlay — per-slot notification panel width / height
        // =========================================================================
        // Width of the center-center message box (pixels)
        constexpr float MSG_CENTER_WIDTH = 420.0f;
        // Height of a single-line center-center message box
        constexpr float MSG_CENTER_HEIGHT = 36.0f;

        // Custom window messages
        constexpr UINT WM_QIV_CENTER_MSG_HIDE = WM_USER + 10; // Posted by WM_TIMER to hide center msg


        // Layout mode cycled with O key:
        //   0 — default 3×3 grid
        //   1 — all slots stacked vertically on top-left
        //   2 — compact 2-line summary top-left:
        //         line 1: index / total + filename
        //         line 2: zoom% + WxH / size  (TOP_CENTER + BOT_RIGHT combined)
        inline int OVERLAY_LAYOUT_MODE = 0;

        // P key — toggle semi-transparent background behind all overlay text.
        // Text is always drawn; only the background rect is suppressed when false.
        inline bool OVERLAY_SHOW_BACKGROUND = true;
    }
}
