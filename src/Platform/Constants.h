#pragma once

// =========================================================================
// RC COMPATIBLE DEFINITIONS
// Used by the Resource Compiler for version metadata
// *** Update ONLY the four numbers below to bump the version everywhere ***
// =========================================================================
#define VER_MAJOR 2
#define VER_MINOR 15
#define VER_PATCH 0
#define VER_BUILD 0

// Comma form  — FILEVERSION / PRODUCTVERSION in .rc  (e.g. 2,3,0,0)
#define VER_NUMERIC   VER_MAJOR,VER_MINOR,VER_PATCH,VER_BUILD

// String form — derived via C preprocessor stringification.
// rc.exe (Windows SDK 10) runs a full C-preprocessor pass, so # works here.
// In C++: L"" VER_STR  →  L"2.3.0.0"
// In RC:  VALUE "FileVersion", VER_STR  →  "2.3.0.0"
#define _QIV_S(x)     #x
#define _QIV_STR(x)   _QIV_S(x)
#define VER_STR       _QIV_STR(VER_MAJOR) "." _QIV_STR(VER_MINOR) "." _QIV_STR(VER_PATCH) "." _QIV_STR(VER_BUILD)

#define FILE_DESC     "qIV"
#define ORIG_FILENAME "QuickImageViewer.exe"
#define PROD_NAME     "Quick Image Viewer"
#define COPYRIGHT     "Copyright \xA9 2026 All rights reserved, Ivan Hristov Yanev"
// =========================================================================

#include <iterator>
#include <d2d1.h>

#include "ConstantsTheme.h"  // All application colors with theme support

namespace Constants {
    constexpr const wchar_t *BASE_NAME = L"QuickImageViewer";
    constexpr const wchar_t *APP_CREATOR = L"Ivan Hristov Yanev";

    constexpr const wchar_t *APP_HELP_FOOTER = L"" COPYRIGHT;
    constexpr const wchar_t *APP_TASKBAR_NAME = L"" FILE_DESC;
    constexpr const wchar_t *APP_VERSION = L"" VER_STR; // major.minor.patch.build  e.g. 2.3.0.0
    constexpr const wchar_t *APP_NAME = BASE_NAME;
    constexpr const wchar_t *WINDOW_CLASS_NAME = BASE_NAME;
    constexpr bool IS_ENABLE_RUN_ON_STARTUP = true; // enable or disable run on startup reg value add/delete
    constexpr bool IS_KEEP_IN_BACKGROUND = true; // enable or disable run on startup reg value add/delete
    constexpr bool IS_OPEN_DIRWND_ON_START = false; // open F6 DirWnd automatically when the app starts
    // Prefix applied to every registry value name and every data file name
    // when the app is running in dedicated (-dedicated) mode.
    // Guarantees that a dedicated instance and a normal instance never share
    // any persistent state even when running side-by-side on the same machine.
    namespace DedicatedMode {
        constexpr const wchar_t *DEDICATED_MODE_GLOBAL_PREFIX = L"qivDedicated_";
    }

    // =========================================================================
    // CLICKABLE LINKS — app-wide appearance, single source of truth.
    // *** Change these TWO values to restyle every clickable link in the app ***
    // Consumers: RendererD2D overlay path, HelpWnd footer, StatsWnd link rows,
    // HistoryListWnd link fonts.
    // =========================================================================

    namespace Links {
        constexpr COLORREF COLOR = RGB(100, 180, 255); // light blue
        constexpr bool UNDERLINE = true;

        // Derived float channels (0-1) for D2D / DWrite consumers — do not edit.
        // Explicit masking instead of GetR/G/BValue: the macros cast COLORREF→BYTE
        // which triggers C4310 (constant truncation) on /W4.
        constexpr float COLOR_R_F = static_cast<float>(COLOR & 0xFF) / 255.0f;
        constexpr float COLOR_G_F = static_cast<float>((COLOR >> 8) & 0xFF) / 255.0f;
        constexpr float COLOR_B_F = static_cast<float>((COLOR >> 16) & 0xFF) / 255.0f;
    }


    //Mouse
    constexpr bool IS_MOUSE_VERTICAL_REVERSE_SCROLL_DIRECTION = false;
    constexpr bool IS_MOUSE_HORIZONTAL_REVERSE_SCROLL_DIRECTION = false;
    // How many WHEEL_DELTA ticks (each tick = 120) must accumulate before a
    // horizontal scroll triggers a folder change. 1 = every tick, 2 = every
    // other tick, 3 = three ticks, etc. Raise to reduce sensitivity.
    constexpr int MOUSE_HSCROLL_FOLDER_TICKS = 3;


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
    static constexpr int KEYBOARD_PAN_STEP = 30; // W/A/S/D viewport pan step (DPI-scaled in executor)
    static constexpr int KEYBOARD_WINDOW_MOVE_STEP = 20; // Shift+W/A/S/D window move step (DPI-scaled in executor)
    static constexpr int KEYBOARD_WINDOW_RESIZE_STEP = 20; // Shift+Numpad+/- / Shift++/- resize per side, DPI-scaled
    static constexpr int WINDOW_SNAP_DISTANCE = 24; // px from screen edge to trigger drag-end snap
    static constexpr int CONTEXT_MENU_DRAG_TOLERANCE = 4; // px the cursor may move while RMB is down and still count as a "click" (raises the right-click context menu)


    // Custom window messages
    constexpr UINT WM_QIV_CENTER_MSG_HIDE = WM_USER + 10; // Posted by WM_TIMER to hide center msg


    constexpr int IS_BASE_WIDTH  = 1200;
    constexpr int IS_BASE_HEIGHT = 800;

    constexpr bool IS_SWAP_MOUSE_BUTTONS = true;
    constexpr bool IS_CONTEXT_MENU_ENABLED = true; // main-window right-click context menu on/off

    // Desktop wallpaper fit styles — the 6 native Windows options. These indices
    // map 1:1 onto DESKTOP_WALLPAPER_POSITION inside AppCommands.cpp, and index
    // Constants::Messages::WALLPAPER_NAMES for the labels.
    namespace Wallpaper {
        constexpr int FILL    = 0;
        constexpr int FIT     = 1;
        constexpr int STRETCH = 2;
        constexpr int TILE    = 3;
        constexpr int CENTER  = 4;
        constexpr int SPAN    = 5;
        constexpr int COUNT   = 6;
    }
    constexpr bool IS_CTRL_C_ENABLED       = true;
    constexpr bool IS_THUMB_COPY_ENABLED   = true;
    constexpr bool IS_THUMB_MOVE_ENABLED   = true;
    constexpr bool IS_THUMB_DELETE_ENABLED = true;
    constexpr bool IS_THUMB_PASTE_ENABLED  = true;
    // =========================================================================
    // EXIF WINDOW EMBEDDED THUMBNAIL PREVIEW
    // =========================================================================
    constexpr int EXIF_THUMB_DISPLAY_SIZE = 100; // logical px — max bounding box for embedded thumb
    constexpr int EXIF_THUMB_COL_GAP = 8; // logical px — gap between text column and thumbnail

    // =========================================================================
    // CACHE WINDOW AND CURRENT DIR WINDOW
    // =========================================================================
    constexpr int THUMBNAIL_PANEL_WINDOW_THICKNESS = 120;
    constexpr float THUMBNAIL_PANEL_THUMB_WIDTH = 128.0f; // 128 * 2x DPI = 256px physical → exact Windows 256px thumbnail cache bucket
    constexpr float THUMBNAIL_PANEL_THUMB_HEIGHT = 80.0f;
    constexpr float THUMBNAIL_PANEL_THUMB_SPACING = 18.0f;
    constexpr float THUMBNAIL_PANEL_THUMB_MARGIN = 20.0f;
    constexpr BYTE THUMBNAIL_PANEL_WINDOW_OPACITY = 210;
    constexpr float THUMBNAIL_PANEL_WINDOW_MOUSE_WHEEL_SPEED = 120.0f;
    constexpr int8_t THUMBNAIL_PANEL_WINDOW_MOUSE_WHEEL_DIRECTION = 1; // 1 is forward -1 is reverse
    constexpr bool THUMBNAIL_PANEL_WHEEL_WRAP_AROUND = false; // wrap strip from last→first and first→last on wheel overflow
    constexpr bool THUMBNAIL_PANEL_WHEEL_WRAP_OVERLAY = true; // show center overlay message on strip wrap-around
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
        constexpr float SCROLLBAR_THICKNESS = 8.0f; // px width of the strip
        constexpr float SCROLLBAR_MIN_THUMB = 20.0f; // minimum thumb length in px
        // Opacity (0.0–1.0) applied to a thumbnail that has been cut to the clipboard.
        constexpr float THUMBNAIL_CUT_OPACITY = 0.35f;

        // =====================================================================
        // Visual Effects (U key toggles master at runtime)
        // Each bool can be independently disabled here; all are guarded by
        // AppState::thumbnailEffectsEnabled (the runtime master switch).
        // =====================================================================
        namespace ThumbnailEffects {
            constexpr bool EFFECTS_MASTER_ENABLED = false; // AppState init default
            constexpr bool EFFECT_ROUNDED_CORNERS = true; // overdraw corner bites with bg color
            constexpr bool EFFECT_GLOW_BORDER = true; // accent-color border on selected thumb
            constexpr bool EFFECT_HOVER_SCALE = true; // scale hovered thumb by HOVER_SCALE_FACTOR

            constexpr float CORNER_RADIUS = 6.0f; // logical px — DPI-scaled at runtime
            constexpr float HOVER_SCALE_FACTOR = 1.05f; // uniform enlarge on hover
            // Glow border: LightGreen — same hue as the classic selection border
            constexpr float GLOW_COLOR_R = 0.565f;
            constexpr float GLOW_COLOR_G = 0.933f;
            constexpr float GLOW_COLOR_B = 0.565f;
            constexpr float GLOW_COLOR_A = 1.0f;
            // Fake glow: three rounded strokes at decreasing opacity — reads as a
            // soft halo with zero GPU-blur cost. All values logical px, DPI-scaled.
            constexpr float GLOW_OUTER_OFFSET = 5.0f; // rect inflation of outermost pass
            constexpr float GLOW_OUTER_STROKE = 6.0f;
            constexpr float GLOW_OUTER_OPACITY = 0.12f;
            constexpr float GLOW_MID_OFFSET = 2.0f;
            constexpr float GLOW_MID_STROKE = 3.0f;
            constexpr float GLOW_MID_OPACITY = 0.30f;
            constexpr float GLOW_CORE_STROKE = 2.0f; // crisp innermost border
            constexpr float GLOW_CORE_OPACITY = 0.95f;
        }

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
    constexpr int IS_VRAM_CACHE_IMAGES_COUNT = 20;
    // VRAM budget for the dir-panel thumbnail cache.
    // Each entry is CACHE_THUMB_WIDTH * CACHE_THUMB_HEIGHT * 4 bytes ≈ 37 KB
    // after scaling.  512 MB holds ~14 000 thumbnails — far more than any
    // realistic folder.  Increase if you open folders with tens of thousands
    // of images; decrease on low-VRAM cards.
    // Shell thumbnail retrieval flags (SIIGBF — int bitmask from shobjidl.h):
    //   0x00000000  SIIGBF_RESIZETOFIT    — fit within requested SIZE, generate+cache if needed (default)
    //   0x00000001  SIIGBF_BIGGERSIZEOK   — allow returning a larger bitmap than requested
    //   0x00000008  SIIGBF_THUMBNAILONLY  — only thumbnail, no icon fallback
    //   0x00000010  SIIGBF_INCACHEONLY    — return cached entry only, never generate
    // Default: let Windows generate and persistently cache thumbnails on first access.
    constexpr int SHELL_THUMB_FLAGS = 0x00000000;

    // Maximum path buffer size for Win32 file dialogs (OPENFILENAMEW documented max).
    // Use everywhere a wchar_t buffer receives a user-selected or drag-dropped path.
    constexpr DWORD MAX_FILE_PATH = 32767;

    constexpr int IS_DIR_THUMB_CACHE_BUDGET_MB = 512;
    constexpr int IS_PRELOAD_LOOKASIDE_COUNT = 1;
    constexpr const int PRELOAD_TIMER_COUNTDOWN = 60; // {ms} this is used to delay preloading if user scrolls very fast
    //==========================Cache optimization====================================
    //end Saveable options

    // Custom window messages
    constexpr UINT WM_QIV_PENDING_UPLOADS = WM_USER + 1; // Posted by background decoder thread
    constexpr UINT WM_QIV_REPAINT = WM_USER + 2; // Signal to UI thread that bitmap is ready
    constexpr UINT WM_QIV_SVG_READY = WM_USER + 3; // Posted by IO thread when SVG bytes are loaded
    constexpr UINT WM_QIV_OPEN_FILE = WM_USER + 4; // Posted by DropTarget/WM_COPYDATA; LPARAM = new std::wstring*
    constexpr UINT WM_QIV_SWITCH_TO_FIND = WM_USER + 5; // FindWnd  ← PANEL_SWITCH_TO_FIND_CHAR typed in JumpToWnd
    constexpr UINT WM_QIV_SWITCH_TO_JUMP = WM_USER + 6; // JumpToWnd ← PANEL_SWITCH_TO_JUMP_CHAR typed in FindWnd
    constexpr UINT WM_QIV_SCAN_COMPLETE = WM_USER + 7; // Background dir scan done; LPARAM = new ScanResult*
    constexpr UINT WM_QIV_HISTORY_VALIDATED = WM_USER + 8; // Background history folder scan done; WPARAM = generation, LPARAM = new std::vector<DirScanResult>*
    constexpr UINT WM_QIV_DIR_CHANGED = WM_USER + 9; // Posted by DirWatcher thread when a file-system change is detected

    // ---------------------------------------------------------------------------
    // Directory watcher (ReadDirectoryChangesW / FindFirstChangeNotification)
    // ---------------------------------------------------------------------------
    constexpr bool WATCH_DIR_FOR_CHANGES = true; // master on/off switch
    constexpr UINT DIR_WATCHER_DEBOUNCE_MS = 400; // quiet period before auto-refresh fires
    constexpr UINT_PTR DIR_WATCHER_TIMER_ID = 1008; // main-window dir-change debounce tick
    constexpr UINT_PTR PANEL_DIR_WATCHER_TIMER_ID = 1009; // per-panel dir-change debounce tick

    // First-character panel-switch triggers
    constexpr wchar_t PANEL_SWITCH_TO_JUMP_CHAR = L'#'; // type this in FindWnd  to open JumpToWnd
    constexpr wchar_t PANEL_SWITCH_TO_FIND_CHAR = L'@'; // type this in JumpToWnd to open FindWnd
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
            L".svg", L".tga", L".qoi",
            L".jp2", L".j2k", L".j2c", L".jpf", L".jpx",
            L".hdr", L".exr",
            L".ppm", L".pgm", L".pbm"
        };

        // Helper to get the number of elements
        constexpr size_t SUPPORTED_EXTENSIONS_COUNT = std::size(SUPPORTED_EXTENSIONS);
        constexpr const wchar_t *THEME_FACTOR = L"qivThemeFactor";
        constexpr const wchar_t *KEEP_IN_BACKGROUND = L"qivKeepInBackground";
        constexpr const wchar_t *RUN_ON_STARTUP = L"qivRunOnStartup";
        constexpr const wchar_t *THUMBNAIL_EFFECTS = L"qivThumbnailEffects";
        constexpr const wchar_t *HISTORY_FULL_MODE = L"qivHistoryFullMode";
        constexpr const wchar_t *OVERLAY_VISIBLE = L"qivOverlayVisible";
        constexpr const wchar_t *OVERLAY_SHOW_BG = L"qivOverlayShowBg";
        constexpr const wchar_t *OPEN_DIRWND_ON_START = L"qivOpenDirWndOnStart";
        constexpr const wchar_t *SWAP_MOUSE_BUTTONS = L"qivSwapMouseButtons";
        constexpr const wchar_t *WHEEL_INVERT   = L"qivWheelInvert";
        constexpr const wchar_t *WHEEL_INVERT_H = L"qivWheelInvertH";
        constexpr const wchar_t *VRAM_CACHE_COUNT = L"qivVramCacheCount";
        constexpr const wchar_t *VIEW_MODE        = L"qivViewMode";
        constexpr const wchar_t *BASE_WIDTH_KEY   = L"qivBaseWidth";
        constexpr const wchar_t *BASE_HEIGHT_KEY  = L"qivBaseHeight";
        constexpr const wchar_t *START_FULLSCREEN      = L"qivStartFullscreen";
        constexpr const wchar_t *HISTORY_MAX_DIRS      = L"qivHistoryMaxDirs";
        constexpr const wchar_t *HISTORY_MAX_FAVS      = L"qivHistoryMaxFavs";
        constexpr const wchar_t *DIR_THUMB_CACHE_MB    = L"qivDirThumbCacheMB";
        constexpr const wchar_t *PRELOAD_LOOKASIDE      = L"qivPreloadLookaside";
        constexpr const wchar_t *MSG_CENTER_MS          = L"qivMsgCenterMs";
        constexpr const wchar_t *HISTORY_MAX_DIRS_SAVE  = L"qivHistoryMaxDirsSave";
        constexpr const wchar_t *SLIDESHOW_INTERVAL_MS  = L"qivSlideshowIntervalMs";
        constexpr const wchar_t *SLIDESHOW_LOOP          = L"qivSlideshowLoop";
        constexpr const wchar_t *SLIDESHOW_SHUFFLE       = L"qivSlideshowShuffle";
        constexpr const wchar_t *SLIDESHOW_TRANSITION    = L"qivSlideshowTransition";
        constexpr const wchar_t *SLIDESHOW_TRANS_SOURCE  = L"qivSlideshowTransSource";
        constexpr const wchar_t *SLIDESHOW_TRANS_ORDER   = L"qivSlideshowTransOrder";
        constexpr const wchar_t *SLIDESHOW_TRANS_LIST    = L"qivSlideshowTransList"; // bitmask
        constexpr const wchar_t *SORT_ORDER              = L"qivSortOrder";
        constexpr const wchar_t *SORT_REVERSE            = L"qivSortReverse";
        constexpr const wchar_t *CTRL_C_ENABLED          = L"qivCtrlCEnabled";
        constexpr const wchar_t *INPUTBOX_CARET_STYLE    = L"qivCaretStyle";
        constexpr const wchar_t *ZOOM_CLICK_MULT         = L"qivZoomClick";
        constexpr const wchar_t *CONTEXT_MENU_ENABLED    = L"qivContextMenu";
        constexpr const wchar_t *THUMB_COPY_ENABLED      = L"qivThumbCopy";
        constexpr const wchar_t *THUMB_MOVE_ENABLED      = L"qivThumbMove";
        constexpr const wchar_t *THUMB_DELETE_ENABLED    = L"qivThumbDelete";
        constexpr const wchar_t *THUMB_PASTE_ENABLED     = L"qivThumbPaste";
    }

    namespace SettingsFile {
        constexpr const wchar_t *EXPORT_PREFIX = L"QIVSettings_";
        constexpr const wchar_t *EXPORT_EXTENSION = L".ini";
        constexpr const wchar_t *EXPORT_FILTER = L"INI Settings (*.ini)\0*.ini\0All Files (*.*)\0*.*\0";
    }

    namespace Backup {
        constexpr const wchar_t *BACKUP_PREFIX = L"qIVBackup_";
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
        constexpr bool IS_OVERLAY_SHOW_BACKGROUND = true;
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

        constexpr UINT IS_MSG_CENTER_DISPLAY_MS = 1000;
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
        constexpr const wchar_t *FAVORITES_FILE_NAME = L"qivFavorites.txt";
        //backup name must be the same as the file name only append the currentDate ex: qivHistory_DATE.bak
        //We Backup when we delete history/favorites only then we first backup then delete !
        constexpr const wchar_t *HISTORY_FAVORITES_BACKUP_FOLDER = L"/QivBackup";
        constexpr const wchar_t *HISTORY_FAVORITES_BACKUP_VERSION = L"Backup Version Schema : 1.0";
        //when backing up history or favorites first line must be the date time and the QuickImageViewer backupVersion ,and COMPUTER_NAME ex:BACKUP COMPUTER_NAME, dd.MM.YYYY, HH:MM:SS.ms, HISTORY_FAVORITES_BACKUP_VERSION
        constexpr const wchar_t *HISTORY_FAVORITES_BACKUP_EXTENSION = L".bak";
        //theese are kept im mot recently used order in ram , when addin a new one to file just append to end with no duplicates
        constexpr int IS_HISTORY_MAX_DIRS_TO_SHOW = 10; // how many folders to show in historyWnd
        constexpr int IS_HISTORY_MAX_DIRS_TO_SAVE = 1000; // how many folders to remember/sava in file , just append to end new ones until max is reached excluding duplicates
        constexpr char HISTORY_FAVORITES_MARK = '*'; // mark for favorites appened before the file name
        constexpr int IS_HISTORY_MAX_FAVORITES_TO_SHOW = 10; // how many favorites folders to show in HistoryWnd
        constexpr int HISTORY_FAVORITES_POSITION = 0; // 0 on top , 1 on bottom , 2 don't change position(not pinned)

        constexpr int HISTORY_ROW_HEIGHT = 28; // px at 96 DPI per history row
        constexpr int HISTORY_PADDING = 16; // px at 96 DPI inner padding
        constexpr int HISTORY_FONT_SIZE = 14; // pt at 96 DPI — header / hint lines
        constexpr int HISTORY_LIST_FONT_SIZE = 16; // pt at 96 DPI — list item text (tune independently)
        constexpr int HISTORY_FILTER_ROW_H = 24; // px at 96 DPI — filter input row below the footer
        // Scrollbar (right-edge GDI strip) - geometry only
        constexpr int SCROLLBAR_THICKNESS = 6; // px width
        constexpr int SCROLLBAR_MIN_THUMB = 16; // minimum thumb height in px
        // Note: Scrollbar colors moved to ConstantsTheme.h

        // Window size limits (px at 96 DPI — DPI-scaled at runtime)
        constexpr int HISTORY_MIN_W = 690; // minimum panel width
        constexpr int HISTORY_MAX_W = IS_BASE_WIDTH  - 120; // maximum panel width
        constexpr int HISTORY_MIN_H = 620; // minimum panel height
        constexpr int HISTORY_MAX_H = IS_BASE_HEIGHT - 60; // maximum panel height (also capped to 80% of monitor)
    }

    // =========================================================================
    // InputBox — shared single-line text control (Find / JumpTo / History / Help)
    // =========================================================================
    namespace InputBox {
        // Text-caret geometry (px at 96 DPI, DPI-scaled at draw time).
        //   CARET_STYLE 0 = vertical bar (|) sized to the text height.
        //   CARET_STYLE 1 = underscore (_) along the text baseline.
        constexpr int   CARET_STYLE          = 1;    // 0 = vertical bar, 1 = underscore
        constexpr int   CARET_PADDING_HEIGHT = 0;    // bar: top/bottom inset from text height
                                                     // underscore: lift off the baseline
        constexpr float CARET_THICKNESS      = 1.0f; // bar: width / underscore: height (float:
                                                     // e.g. 1.5 reads as in-between on high-DPI)
        constexpr int   CARET_GAP            = 0;    // horizontal space between caret and text
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

        // Adaptive directory-scan reserve hint (DirScanReserveHint). The hint is
        // the previous scan's image count, clamped to this range, used to pre-size
        // the playlist + size/time maps in one allocation.
        //   FLOOR — cover small folders without realloc churn.
        //   CAP   — bound the worst-case over-reserve after a one-off huge folder.
        //           Folders larger than CAP still load fine; the vector just grows
        //           past the reserve (a few extra reallocs). Raise CAP to match the
        //           largest folder you regularly open.
        constexpr size_t DIR_SCAN_RESERVE_FLOOR = 256;
        constexpr size_t DIR_SCAN_RESERVE_CAP   = 16384;
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
        constexpr UINT_PTR GIF_TIMER_ID = 1006; // animated GIF frame-advance tick


        constexpr int IS_INTERVAL_MS = 5000; // ms between auto-advances
        constexpr bool IS_LOOP = true; // wrap to first image at end
        constexpr bool IS_SHUFFLE = false; // random order
        constexpr int CURSOR_HIDE_MS = 3000; // ms of inactivity before hiding cursor (0 = never)
        constexpr int TRANSITION_TICK_MS = 16; // animation tick interval ~60 fps
        constexpr int TRANSITION_DURATION_MS = 800; // default transition length ms

        // Number of TransitionType members — keep in sync with the enum in
        // SlideshowTransitions.h (a static_assert-free contract used for menu
        // building, cycling and registry clamping).
        constexpr int TRANSITION_COUNT = 21;

        // Transition selection is two independent axes:
        //   SOURCE — which transitions are in play
        //   ORDER  — how the next one is drawn from that pool (ignored for NONE)
        // Both index the matching *_NAMES arrays in ConstantsStrings.h.
        namespace TransitionSource {
            constexpr int NONE  = 0; // always the single transition the user picked
            constexpr int ALL   = 1; // every entry in the menu
            constexpr int LIST  = 2; // only the entries ticked in the menu
            constexpr int COUNT = 3;
        }
        namespace TransitionOrder {
            constexpr int SEQUENTIAL = 0; // menu order, one per slide, then repeat
            constexpr int RANDOM     = 1; // random pick from the pool every slide
            constexpr int COUNT      = 2;
        }

        // Dissolve — alpha ramp is quantised into this many discrete steps so it
        // reads as a grainy staircase instead of a smooth Fade.
        constexpr int DISSOLVE_STEPS = 14;

        // Ripple — damped zoom oscillation around 1.0.
        constexpr float RIPPLE_AMPLITUDE = 0.12f; // peak zoom deviation at progress 0
        constexpr float RIPPLE_WAVES     = 3.0f;  // full oscillations across the transition

        // ZoomIn — starting zoom that grows to 1.0 (ZoomOut is the 2× → 1× inverse).
        constexpr float ZOOM_IN_START = 0.4f;

        // ── Tunables for the extended transition set ─────────────────────────
        constexpr float SOFT_ZOOM_START  = 1.5f; // SoftZoom starting zoom
        constexpr int   SPIN_DEGREES     = 360;  // Spin full turn
        constexpr int   SPIN_ZOOM_DEGREES = 180; // SpinZoom half turn
        constexpr float SPIN_ZOOM_START  = 0.3f; // SpinZoom starting zoom
        constexpr float DRIFT_FRACTION   = 0.12f;// Drift* travel as a fraction of the window
        constexpr float FLICKER_CYCLES   = 6.0f; // Flicker strobes across the transition
        constexpr float BOUNCE_START     = 0.6f; // Bounce starting zoom
        constexpr float BOUNCE_OVERSHOOT = 0.18f;// Bounce peak above 1.0
        constexpr int   SWING_DEGREES    = 14;   // Swing peak tilt
        constexpr float SWING_WAVES      = 2.5f; // Swing oscillations
        constexpr float SLAM_START       = 3.0f; // Slam starting zoom
        constexpr float IRIS_START       = 0.02f;// Iris starting zoom (near a point)
    }

    // =========================================================================
    // Save dialog formats  (Ctrl+S)
    // Add a row here to expose a new format in the save dialog.
    // SaveCurrentImageWithEffects() detects format from the chosen file extension.
    // =========================================================================
    namespace Save {
        struct Format {
            const wchar_t *description; // label shown in the dialog filter list
            const wchar_t *pattern; // file mask(s), e.g. L"*.jpg;*.jpeg"
            const wchar_t *ext; // extension auto-appended when user omits it
        };

        constexpr Format FORMATS[] = {
            {L"PNG Image", L"*.png", L"png"},
            {L"JPEG Image", L"*.jpg;*.jpeg", L"jpg"},
            {L"BMP Image", L"*.bmp", L"bmp"},
            {L"TIFF Image", L"*.tif;*.tiff", L"tif"},
            {L"GIF Image", L"*.gif", L"gif"},
        };
        constexpr wchar_t DEFAULT_EXT[] = L"png"; // used when no filter is selected
    }
}
