#pragma once

// =========================================================================
// RC COMPATIBLE DEFINITIONS
// Used by the Resource Compiler for version metadata
// =========================================================================
#define VER_NUMERIC 1,5,1,0        // Numeric version for file properties (4 comma-separated integers) must be exactly 4 numbers separated by commas
#define VER_STR "1.5.1"            // Human-readable version string; may contain text such as "1.5 Beta"
#define FILE_DESC "qIV"            // Friendly name displayed in Task Manager/Explorer
#define ORIG_FILENAME "QuickImageViewer.exe" // Original name of the binary
#define PROD_NAME "Quick Image Viewer"       // Official product name
#define COPYRIGHT "Copyright® 06.2026 All rights reserved, Ivan Hristov Yanev" // Copyright notice
// =========================================================================

#include <iterator>
#include <d2d1.h>
#include <dwmapi.h>
#include "ConstantsTheme.h"  // All application colors with theme support

namespace Constants {
    constexpr const wchar_t *BASE_NAME = L"QuickImageViewer";
    constexpr const wchar_t *APP_CREATOR = L"Ivan Hristov Yanev";

    constexpr const wchar_t *APP_HELP_FOOTER = L"" COPYRIGHT;
    constexpr const wchar_t *APP_TASKBAR_NAME = L"" FILE_DESC;
    constexpr const wchar_t *APP_VERSION = L"" VER_STR; // major.minor.patch
    constexpr const wchar_t *APP_NAME = BASE_NAME;
    constexpr const wchar_t *WINDOW_CLASS_NAME = BASE_NAME;
    constexpr bool IS_ENABLE_RUN_ON_STARTUP = true; // enable or disable run on startup reg value add/delete

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
    static constexpr float ZOOM_CLICK = 3.0f; //  left click zoom multiplier
    static constexpr int OPACITY_STEP = 10; // 0 to 100
    static constexpr int KEYBOARD_PAN_STEP = 30;         // W/A/S/D viewport pan step (DPI-scaled in executor)
    static constexpr int KEYBOARD_WINDOW_MOVE_STEP = 20; // Shift+W/A/S/D window move step (DPI-scaled in executor)
    static constexpr int WINDOW_SNAP_DISTANCE = 24;      // px from screen edge to trigger drag-end snap


    // Custom window messages
    constexpr UINT WM_QIV_CENTER_MSG_HIDE = WM_USER + 10; // Posted by WM_TIMER to hide center msg


    constexpr int BASE_WIDTH = 1200;
    constexpr int BASE_HEIGHT = 800;

    inline bool SWAP_MOUSE_BUTTONS = true; // Set this to true to swap Left and Right mouse button functions
    // =========================================================================
    // CACHE WINDOW AND CURRENT DIR WINDOW
    // =========================================================================
    constexpr float THUMBNAIL_PANEL_THUMB_WIDTH = 120.0f;
    constexpr float THUMBNAIL_PANEL_THUMB_HEIGHT = 80.0f;
    constexpr float THUMBNAIL_PANEL_THUMB_SPACING = 18.0f;
    constexpr float THUMBNAIL_PANEL_THUMB_MARGIN = 20.0f;
    constexpr BYTE THUMBNAIL_PANEL_WINDOW_OPACITY = 210;
    constexpr float THUMBNAIL_PANEL_WINDOW_MOUSE_WHEEL_SPEED = 120.0f;
    constexpr int8_t THUMBNAIL_PANEL_WINDOW_MOUSE_WHEEL_DIRECTION = 1; // 1 is forward -1 is reverse
    constexpr int THUMBNAIL_PANEL_WINDOW_THICKNESS = 120;
    // Pixel gap between a panel edge and the taskbar (visual breathing room)
    constexpr int THUMBNAIL_PANEL_TASKBAR_BOTTOM_GAP_HORIZONTAL_PANEL = 6;
    constexpr int THUMBNAIL_PANEL_NEIGHBOUR_GAP_VERTICAL_PANEL = 2; // gap between vertical and horizontal panels
    //   position 0 : centered floating panel (80 % wide, thumb-height tall)
    //   position 1 : top edge strip (full width)
    //   position 2 : right edge strip (full height)
    //   position 3 : bottom edge strip (full width)
    //   position 4 : left edge strip (full height)
    //cache window
    constexpr int8_t CACHE_WINDOW_POSITION = 3;
    //current dir window (F5 — always independent, always top strip)
    constexpr int8_t CURRENT_DIR_WINDOW_POSITION = 1;

    // =========================================================================
    // Spawned DirWnd instances (from HistoryWnd Shift+Enter)
    // =========================================================================
    // Maximum number of DirWnd instances that can be spawned from history.
    // Slot assignment (0-based spawn index → panel position):
    //   spawn 0 → left  strip (position 4)
    //   spawn 1 → right strip (position 2)
    //   spawn 2 → center floating (position 0)
    //   spawn 3 → wraps back to 0 (oldest slot reused)
    constexpr int DIR_WND_MAX_INSTANCES = 4; // top, left, right, bottom
    constexpr int8_t DIR_WND_SPAWN_POSITIONS[DIR_WND_MAX_INSTANCES] = {1, 4, 2, 3};


    namespace ThumbnailPanel {
        // Geometry & Layout (non-color)
        constexpr float SELECTION_BORDER_THICKNESS = 3.0f;
        constexpr float HOVER_THICKNESS = 1.0f;
        constexpr float SCROLLBAR_THICKNESS = 4.0f; // px width of the strip
        constexpr float SCROLLBAR_MIN_THUMB = 20.0f; // minimum thumb length in px

        // Scrollbar position enums
        enum class ScrollbarSide { LEFT = 0, RIGHT = 1 };

        enum class ScrollbarEdge { TOP = 0, BOTTOM = 1 };

        // Per-position scrollbar placement
        // Position mapping: 0=center, 1=top, 2=right, 3=bottom, 4=left
        constexpr ScrollbarSide SCROLLBAR_POS_RIGHT_PANEL = ScrollbarSide::LEFT; // right panel (pos 2), scrollbar on left
        constexpr ScrollbarSide SCROLLBAR_POS_LEFT_PANEL = ScrollbarSide::RIGHT; // left panel (pos 4), scrollbar on right
        constexpr ScrollbarEdge SCROLLBAR_POS_TOP_PANEL = ScrollbarEdge::BOTTOM; // top panel (pos 1), scrollbar at bottom
        constexpr ScrollbarEdge SCROLLBAR_POS_BOTTOM_PANEL = ScrollbarEdge::BOTTOM; // bottom panel (pos 3), scrollbar at bottom

        // Note: All colors moved to ConstantsTheme.h
    }

    // =========================================================================
    // Cache optimization
    // =========================================================================
    constexpr const int VRAM_CACHE_THUMBS_THREADS_COUNT = 4; // fallback if processor has less than 8 thread otherwise dynamic thread count / 2
    constexpr const int VRAM_CACHE_DECODER_THREADS_COUNT = 2;
    constexpr const int VRAM_CACHE_IMAGES_COUNT = 20;
    constexpr const int VRAM_CACHE_SVG_COUNT = 20;
    // VRAM budget for the dir-panel thumbnail cache.
    // Each entry is CACHE_THUMB_WIDTH * CACHE_THUMB_HEIGHT * 4 bytes ≈ 37 KB
    // after scaling.  512 MB holds ~14 000 thumbnails — far more than any
    // realistic folder.  Increase if you open folders with tens of thousands
    // of images; decrease on low-VRAM cards.
    constexpr const size_t DIR_THUMB_CACHE_BUDGET_MB = 512;
    constexpr const int PRELOAD_LOOKASIDE_COUNT = 1;
    constexpr const int PRELOAD_TIMER_COUNTDOWN = 60; // {ms} this is used to delay preloading if user scrolls very fast
    //==========================Cache optimization====================================
    //end Saveable options


    // Custom window messages
    constexpr UINT WM_QIV_PENDING_UPLOADS = WM_USER + 1; // Posted by background decoder thread
    constexpr UINT WM_QIV_REPAINT = WM_USER + 2; // Signal to UI thread that bitmap is ready
    constexpr UINT WM_QIV_SVG_READY = WM_USER + 3; // Posted by IO thread when SVG bytes are loaded
    // =============================================================================


    namespace Registry {
        // Switch this between HKEY_CURRENT_USER and HKEY_LOCAL_MACHINE
        inline HKEY ROOT_HIVE = HKEY_CURRENT_USER;

        // Base path for application-specific user preferences (HKEY_CURRENT_USER)
        constexpr const wchar_t *ROOT_KEY = L"Software\\QuickImageViewer";
        // Path string to the last directory accessed by the user
        constexpr const wchar_t *LAST_FOLDER = L"LastFolder";

        // --- Settings (Stored under ROOT_KEY) ---
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
            L".jpg", L".jpeg", L".jpe", L".png", L".apng", L".webp", L".bmp", L".gif", L".tiff", L".tif",
            L".ico", L".cur", L".heic", L".heif", L".hif", L".heics", L".heifs",
            L".jxr", L".wdp", L".hdp", L".dds", L".jxl",
            L".avif", L".avcs", L".avci", L".avifs",
            L".dng", L".cr2", L".cr3", L".nef", L".arw",
            L".svg", L".tga"
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
        constexpr const bool IS_COMPACT_OVERLAY_MODE = true; // true → 1-line, false → 2-line
        // P key — toggle semi-transparent background behind all overlay text.
        // Text is always drawn; only the background rect is suppressed when false.
        inline bool OVERLAY_SHOW_BACKGROUND = true;
        // Layout mode cycled with O key:
        //   0 — default 3×3 grid
        //   1 — all slots stacked vertically on top-left
        //   2 — compact 2-line summary top-left:
        //         line 1: index / total + filename
        //         line 2: zoom% + WxH / size  (TOP_CENTER + BOT_RIGHT combined)
        inline int OVERLAY_LAYOUT_MODE = 0;

        // =========================================================================
        // Overlay — Center-Center message queue (MID_CENTER slot)
        // =========================================================================
        // How long the center-center notification stays visible before auto-hiding (ms)

        constexpr UINT MSG_CENTER_DISPLAY_MS = 1000;
        // Center-center text color  (R, G, B, A)
        constexpr float MSG_CENTER_COLOR_R = 1.0f;
        constexpr float MSG_CENTER_COLOR_G = 0.85f;
        constexpr float MSG_CENTER_COLOR_B = 0.20f;
        constexpr float MSG_CENTER_COLOR_A = 1.0f;
        constexpr float MSG_BASE_FONT_SIZE = 10.0f;
        // Center-center font size (pt). Other slots use the RendererD2D default size.
        constexpr float MSG_ALL_BUT_CENTER_FONT_SIZE = MSG_BASE_FONT_SIZE * 1.4f;
        constexpr float MSG_CENTER_FONT_SIZE = MSG_BASE_FONT_SIZE * 1.6f;
        constexpr const wchar_t *MSG_ALL_BUT_CENTER_FONT_FAMILY_DEFAULT = L"Segoe UI";
        constexpr const wchar_t *MSG_CENTER__FONT_FAMILY_DEFAULT = L"Segoe UI";
        constexpr const wchar_t *MSG_ALL_FONT_FAMILY_FALLBACK = L"Arial";
        constexpr const wchar_t *MSG_ALL_FONT_LOCALE = L"en-us";

        // =========================================================================
        // Overlay — per-slot notification panel width / height
        // =========================================================================
        // Width of the center-center message box (pixels)

        constexpr float MSG_CENTER_WIDTH = 420.0f;
        // Height of a single-line center-center message box
        constexpr float MSG_CENTER_HEIGHT = 36.0f;
        // Custom window messages
        constexpr UINT WM_QIV_CENTER_MSG_HIDE = WM_USER + 10; // Posted by WM_TIMER to hide center msg
    }

    namespace History {
        // =========================================================================
        // Folder History (HistoryWindow)
        // =========================================================================
        constexpr bool HISTORY_SHOW_FULL_HISTORY = false; // controls initial behaviour of HistoryWnd full or limited , you can swith with key comb that after you show but this is for inital behaviour
        constexpr const wchar_t *HISTORY_FILE_NAME = L"qivHistory.txt";
        constexpr const wchar_t *DEDICATED_HISTORY_FILE_NAME = L"qivHistory_dedicated.txt"; // -dedicated mode
        constexpr const wchar_t *FAVORITES_FILE_NAME = L"qivFavorites.txt";
        //backup name must be the same as the file name only append the currentDate ex: qivHistory_DATE.bak
        //We Backup when we delete history/favorites only then we first backup then delete !
        constexpr const wchar_t *HISTORY_FAVORITES_BACKUP_FOLDER = L"/QivBackup";
        constexpr const wchar_t *HISTORY_FAVORITES_BACKUP_VERSION = L"Backup Version Schema : 1.0";
        //when backing up history or favorites first line must be the date time and the QuickImageViewer backupVersion ,and COMPUTER_NAME ex:BACKUP COMPUTER_NAME, dd.MM.YYYY, HH:MM:SS.ms, HISTORY_FAVORITES_BACKUP_VERSION
        constexpr const wchar_t *HISTORY_FAVORITES_BACKUP_EXTENSION = L".bak";
        //theese are kept im mot recently used order in ram , when addin a new one to file just append to end with no duplicates
        constexpr int HISTORY_MAX_DIRS_TO_SHOW = 10; // how many folders to show in historyWnd
        constexpr int HISTORY_MAX_DIRS_TO_SAVE = 1000; // how many folders to remember/sava in file , just append to end new ones until max is reached excluding duplicates
        constexpr char HISTORY_FAVORITES_MARK = '*'; // mark for favorites appened before the file name
        constexpr int HISTORY_MAX_FAVORITES_TO_SHOW = 10; // how many favorites folders to show in HistoryWnd
        constexpr int HISTORY_FAVORITES_POSITION = 0; // 0 on top , 1 on bottom , 2 don't change position(not pinned)

        constexpr int HISTORY_ROW_HEIGHT = 28; // px at 96 DPI per history row
        constexpr int HISTORY_PADDING = 16; // px at 96 DPI inner padding
        constexpr int HISTORY_FONT_SIZE = 14; // pt at 96 DPI — header / hint lines
        constexpr int HISTORY_LIST_FONT_SIZE = 16; // pt at 96 DPI — list item text (tune independently)
        // Scrollbar (right-edge GDI strip) - geometry only
        constexpr int SCROLLBAR_THICKNESS = 6; // px width
        constexpr int SCROLLBAR_MIN_THUMB = 16; // minimum thumb height in px
        // Note: Scrollbar colors moved to ConstantsTheme.h
    }

    // =========================================================================
    // Cursors — LMB mode indicators
    // =========================================================================
    // Change these IDC_ values to swap the cursor for each mode.
    // Values are the raw resource IDs (WORD) behind the IDC_ macros.
    // Use as: LoadCursor(nullptr, MAKEINTRESOURCEW(Constants::Cursors::LMB_ZOOM))
    // Change these to any other OCR_* value to swap the cursor for each mode.
    namespace Cursors {
        constexpr WORD LMB_ZOOM = 32515; // IDC_CROSS  — LMB will 3x zoom
        constexpr WORD LMB_PAN = 32649; // IDC_HAND   — LMB will pan
        constexpr WORD RMB_DOWN = 32512; // IDC_ARROW  — RMB is held
        constexpr WORD DEFAULT = 32512; // IDC_ARROW  — restored after action
    }

    namespace FileHandler {
        constexpr const int FILE_HANDLER_DEFAULT_SORT_ORDER = 0; // 0 name, 1 date, 2 size,3 extension(type), 4 performance mode - SortPlaylistByDiskOrder
        constexpr const bool FILE_HANDLER_SORT_TYPE_IS_REVERSE = false;
    }

    // =========================================================================
    // Slideshow  (Ctrl+F1 — play / pause / stop cycle)
    // Init-only constants; runtime state lives in AppState::SlideshowState.
    // =========================================================================
    namespace Slideshow {
        // WM_TIMER wParam IDs (1001/1002 are taken by lookaside/center-msg)
        constexpr UINT_PTR TIMER_ID = 1003; // slide-advance tick
        constexpr UINT_PTR CURSOR_TIMER_ID = 1004; // cursor-hide inactivity tick
        constexpr UINT_PTR TRANSITION_TIMER_ID = 1005; // transition animation tick

        constexpr int INTERVAL_MS = 5000; // ms between auto-advances
        constexpr bool LOOP = true; // wrap to first image at end
        constexpr bool SHUFFLE = false; // random order
        constexpr int CURSOR_HIDE_MS = 3000; // ms of inactivity before hiding cursor (0 = never)
        constexpr int TRANSITION_TICK_MS = 16; // animation tick interval ~60 fps
        constexpr int TRANSITION_DURATION_MS = 800; // default transition length ms
    }
}
