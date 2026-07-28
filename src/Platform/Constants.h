#pragma once

// Version and product identity — FILE_DESC, PROD_NAME, COPYRIGHT, and the
// numbers you edit to bump a release — all live in Common/Version.h.
#include "Common/Version.h"

#include <iterator>
#include <cstdint>   // uint32_t — Slideshow::TRANSITION_LIST_DEFAULT_MASK
#include "../resources/resource.h"

namespace Constants {
    constexpr const wchar_t *BASE_NAME = L"QuickImageViewer";
    constexpr const wchar_t *APP_CREATOR = L"Ivan Hristov Yanev";

    constexpr const wchar_t *APP_HELP_FOOTER = L"" COPYRIGHT;
    constexpr const wchar_t *APP_TASKBAR_NAME = L"" FILE_DESC;
    // major.minor.patch.build — e.g. 2.80.0.123
    // Defined in Version.cpp, the one TU that sees the generated build number.
    // Deliberately not constexpr: making it so would drag BuildNumber.h in here
    // and rebuild the world on every build. Every use is a runtime string.
    extern const wchar_t *const APP_VERSION;
    constexpr const wchar_t *APP_NAME = BASE_NAME;
    constexpr const wchar_t *WINDOW_CLASS_NAME = BASE_NAME;
    constexpr bool IS_ENABLE_RUN_ON_STARTUP = true; // enable or disable run on startup reg value add/delete
    constexpr bool IS_KEEP_IN_BACKGROUND = true; // enable or disable run on startup reg value add/delete
    constexpr bool IS_OPEN_DIRWND_ON_START = false; // open F6 DirWnd automatically when the app starts

    // Viewport lock (Y). When ON, zoom and pan survive an image change instead of
    // being reset — so flipping through same-framed shots keeps the same detail
    // on screen at the same magnification. Rotation and flips are NOT carried:
    // ApplyOrientationToViewport rewrites them from each file's EXIF tag, and
    // suppressing that would show portrait shots sideways in a mixed folder.
    // Never default this ON — a locked viewport on a fresh install looks like the
    // app failed to fit the image.
    constexpr bool IS_LOCK_VIEWPORT = false;
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
    static constexpr float ZOOM_CLICK_MIN  = 1.0f;  // minimum (1 = off)
    static constexpr float ZOOM_CLICK_MAX  = 100.0f; // maximum
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

    // KIOSK lock — every keyboard and mouse message to the main window is
    // swallowed, so a screen on a wall cannot be driven by a passer-by. Persisted
    // like any other toggle, which is what lets a dedicated screen ship locked
    // from its .ini. The ONLY way back out is the tray icon: TrackPopupMenu runs
    // its own message loop, so the tray menu still works while the window is
    // deaf. Never default this on — a locked app with no tray icon is a brick.
    constexpr bool IS_KIOSK_LOCK_ENABLED = false;

    // Window stays above all others. Ctrl+T toggles it; a dedicated screen sets
    // it in its config so nothing can cover the display.
    constexpr bool IS_ALWAYS_ON_TOP = false;

    // Hold off the screensaver and display sleep while the main window is
    // visible. An unattended screen is useless once Windows blanks it, and no
    // input ever arrives to wake it — least of all with the kiosk lock on.
    // Off by default: the main app has no business overriding a laptop's power
    // plan unless asked.
    constexpr bool IS_KEEP_DISPLAY_AWAKE = false;

    // Desktop wallpaper fit styles — the 6 native Windows options. These indices
    // map 1:1 onto DESKTOP_WALLPAPER_POSITION inside AppCommands.cpp, and index
    // Constants::Messages::WALLPAPER_NAMES for the labels.
    // =========================================================================
    // DEDICATED INSTANCES  (src/Dedicated/*)
    // A dedicated instance is a named, isolated copy of the app — its own mutex,
    // window class, registry namespace and history file — typically parked
    // fullscreen on one monitor running a slideshow. It may additionally pull
    // from a SECOND playlist of "promotions" that is never merged into the
    // normal image playlist.
    // =========================================================================
    namespace Dedicated {
        // ── F8 panel appearance — single place to restyle the whole window ──
        // Opacity of the Dedicated panel, 0 = invisible .. 255 = opaque.
        constexpr BYTE PANEL_OPACITY = 244;

        // Value colours, chosen so a setting's STATE is readable at a glance
        // without reading the text: green/red for on/off, amber for numbers,
        // blue for paths, violet for multi-choice.
        namespace PanelColors {
            constexpr COLORREF ON       = RGB(126, 211, 133); // toggle enabled
            constexpr COLORREF OFF      = RGB(214, 122, 122); // toggle disabled
            constexpr COLORREF NUMBER   = RGB(232, 190, 110); // numeric value
            constexpr COLORREF PATH     = RGB(120, 186, 244); // folder / file path
            constexpr COLORREF CHOICE   = RGB(196, 160, 240); // one of several options
            constexpr COLORREF TEXT     = RGB(230, 230, 230); // free text value
            constexpr COLORREF HEADER   = RGB(120, 190, 250); // section heading
            constexpr COLORREF STRIPE   = RGB(70, 120, 180);  // heading accent bar
            constexpr COLORREF BTN_MAIN = RGB(58, 104, 158);  // build actions
            constexpr COLORREF BTN_ALT  = RGB(70, 92, 74);    // deploy actions
            constexpr COLORREF WARN     = RGB(222, 148, 96);  // unset / needs attention

            constexpr COLORREF SCROLL_TRACK     = RGB(48, 48, 52);
            constexpr COLORREF SCROLL_THUMB     = RGB(96, 104, 118);
            constexpr COLORREF SCROLL_THUMB_HOT = RGB(132, 156, 196);
        }

        // Scrollbar geometry for the F8 panel (DPI-scaled at draw time).
        constexpr int PANEL_SCROLLBAR_W  = 12;
        constexpr int PANEL_SCROLL_MIN_H = 28; // shortest the thumb may get

        // Promotion weighting. A promo's weight is its relative chance of being
        // drawn; 65535 is ~65535× more likely than 1.
        constexpr int PROMO_WEIGHT_MIN     = 1;
        constexpr int PROMO_WEIGHT_MAX     = 65535;
        constexpr int PROMO_WEIGHT_DEFAULT = 1;

        // Weight is read from a "#<n>" suffix on the file stem, e.g.
        // "summer-sale#500.jpg". Mirrors the existing -monitorNum#N convention.
        constexpr wchar_t PROMO_WEIGHT_MARKER = L'#';

        // WHEN a promotion appears. TWO triggers exist — an image counter and a
        // wall-clock timer — each configured as a (from, to) pair:
        //
        //   (0, *)    OFF. A missing/zero `from` always disables the trigger,
        //             whatever `to` says. This is deliberate: a half-filled pair
        //             like (0, 90) is a config mistake, and collapsing it to OFF
        //             is safer than inventing a 0..90 range that fires nonstop.
        //   (5, 0)    STRICT — exactly every 5. A zero `to` means "no upper
        //             bound given", so the cadence is fixed at `from`.
        //   (5, 15)   RANDOM — re-rolled between 5 and 15 after every promotion.
        //   (15, 5)   tolerated: `to` below `from` falls back to STRICT `from`.
        //
        // The two triggers run INDEPENDENTLY: each keeps its own countdown, and
        // whichever comes due fires a promotion and re-arms ONLY itself. Both
        // off = promotions disabled, so there is no separate on/off flag.
        // IDENTITY COMES FROM THE EXE FILE, which is what makes collisions
        // impossible: the settings file is the exe's own path with the extension
        // swapped for .ini —
        //     D:\Screens\qIV_dedicated_Lobby.exe
        //     D:\Screens\qIV_dedicated_Lobby.ini
        // Two copies cannot share one name in one folder, so two instances can
        // never resolve to the same settings file, mutex or window class.
        constexpr const wchar_t *SETTINGS_FILE_EXT = L".ini";

        // A dedicated instance has NO history and NO favorites — it is an
        // appliance showing fixed content, not a browser. Instead it keeps two
        // plain folder lists beside the exe, named after it:
        //     imageLists_<exeName>.qim      folders holding the images
        //     promotionList_<exeName>.qpr   folders holding the promotions
        // The actual names are recorded in the .ini so they can be renamed or
        // shared between instances; missing files are generated on startup.
        constexpr const wchar_t *IMAGE_LIST_PREFIX = L"imageLists_";
        constexpr const wchar_t *PROMO_LIST_PREFIX = L"promotionList_";

        // Distinct extensions, not a shared .txt: the file dialogs then filter
        // to exactly one kind, so an image list can never be picked where a
        // promotion list is meant. Both are plain UTF-16 text inside.
        constexpr const wchar_t *IMAGE_LIST_EXT = L".qim"; // qIV image list
        constexpr const wchar_t *PROMO_LIST_EXT = L".qpr"; // qIV promotion list

        // An exe whose FILE NAME contains this substring triggers dedicated
        // mode: with no .ini beside it, a default one is generated rather than
        // falling back to the registry.
        //
        // MUST BE LOWER CASE — the comparison lower-cases the exe name and then
        // searches for this verbatim, so matching is case-insensitive:
        //   qIV_Dedicated_Lobby.exe   ✓
        //   MyDEDICATEDscreen.exe     ✓
        constexpr const wchar_t *EXE_DEDICATED_MARKER = L"dedicated";

        constexpr int PROMO_DISABLED = 0;

        constexpr int PROMO_IMAGES_MAX  = 65535; // fits a 16-bit counter; plenty for "every N images"
        constexpr int PROMO_SECONDS_MAX = 86400; // one day

        // Default: paced by images (every 3-10), timer off.
        constexpr int PROMO_IMAGES_EVERY_DEFAULT   = 3;
        constexpr int PROMO_IMAGES_UPTO_DEFAULT    = 10;
        constexpr int PROMO_SECONDS_EVERY_DEFAULT  = PROMO_DISABLED;
        constexpr int PROMO_SECONDS_UPTO_DEFAULT   = PROMO_DISABLED;

        // How the next promotion is chosen from the promo playlist.
        namespace PromoOrder {
            constexpr int SEQUENTIAL = 0; // folder order, one after another
            constexpr int WEIGHTED   = 1; // weighted random by priority
            constexpr int COUNT      = 2;
        }
    }

    // Command-line-arguments file + generated shortcut (context menu › CmdArgs).
    namespace CmdArgsFile {
        constexpr const wchar_t *EXPORT_NAME   = L"qIVcmdArgs.txt";
        constexpr const wchar_t *SHORTCUT_NAME = L"QuickImageViewer.lnk";
        constexpr const wchar_t *FILE_HEADER   = L"# QuickImageViewer command-line arguments";
    }

    namespace Wallpaper {
        constexpr int FILL    = 0;
        constexpr int FIT     = 1;
        constexpr int STRETCH = 2;
        constexpr int TILE    = 3;
        constexpr int CENTER  = 4;
        constexpr int SPAN    = 5;
        constexpr int COUNT   = 6;
    }
    // Start maximized/borderless on launch. Was the only persisted setting with
    // no named default — load and Restore Defaults each hardcoded their own.
    constexpr bool IS_START_FULLSCREEN     = false;
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
    // WM_USER+10 is WM_QIV_CENTER_MSG_HIDE, declared near the top of this file.
    // Posted by a remote client thread; LPARAM = new std::shared_ptr<Remote::RemoteCall>*,
    // which the UI thread deletes after executing. See src/Rem_TCP_IP/RemoteServer.h.
    constexpr UINT WM_QIV_REMOTE_COMMAND = WM_USER + 11;
    // Posted by the listener thread when it stops on its own (bind failed, socket
    // died). Lets the panel drop back to "stopped" without polling. WPARAM = a
    // Constants::RemoteTcpIp::ERR_* code, 0 for a clean stop.
    constexpr UINT WM_QIV_REMOTE_STOPPED = WM_USER + 12;
    // Posted by a MIRROR sender thread when an observed target pushed an EVENT
    // line. LPARAM = new std::wstring*, owned and deleted by the handler.
    // Executed on the UI thread under the inbound guard, so an observer never
    // re-forwards what it was shown.
    constexpr UINT WM_QIV_REMOTE_EVENT = WM_USER + 13;
    // Posted by a MIRROR sender thread when a target's reply named a DIFFERENT
    // file than the one we landed on — the two playlists have diverged, so the
    // index we sent meant something else there. WPARAM = target id. The UI
    // thread answers by building a `sync` (it alone may read app.playlist) and
    // resending. See RemoteMirror.h on why indices are sent at all.
    constexpr UINT WM_QIV_REMOTE_DESYNC = WM_USER + 14;

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

    // =========================================================================
    // REMOTE CONTROL over TCP/IP  (src/Rem_TCP_IP)
    //
    // The listener is OFF unless explicitly switched on — either by command-line
    // switches or by a [REMOTE_TCP_IP] section in the instance .ini. There is no
    // third way in, and no default that opens a socket. A viewer that has never
    // been configured for remote control never binds a port.
    //
    // Full design record: docs/REMOTE_TCP_IP_SPEC.md
    // =========================================================================
    namespace RemoteTcpIp {
        // --- .ini section and key names ---
        constexpr const wchar_t *INI_SECTION      = L"REMOTE_TCP_IP";
        constexpr const wchar_t *KEY_ENABLE       = L"Enable";
        constexpr const wchar_t *KEY_NAME         = L"Name";
        constexpr const wchar_t *KEY_IP_ADDRESS   = L"IpAddress";
        constexpr const wchar_t *KEY_PORT_NO      = L"PortNo";
        constexpr const wchar_t *KEY_ALLOW_LIST   = L"AllowList";
        constexpr const wchar_t *KEY_PASSWORD     = L"Password";
        constexpr const wchar_t *KEY_BLACK_LIST   = L"BlackList";
        constexpr const wchar_t *KEY_MAX_CONNS    = L"MaxConnections";

        // --- Defaults ---
        // Never default ENABLE on. A viewer that binds a port because nobody
        // said otherwise is a viewer that surprises its owner.
        constexpr bool ENABLE_DEFAULT = false;
        // Loopback: reachable only from this machine, and Windows Firewall never
        // prompts. A wall screen opts into 0.0.0.0 (every interface) knowingly.
        constexpr const wchar_t *BIND_ADDRESS_DEFAULT = L"127.0.0.1";
        constexpr const wchar_t *BIND_ADDRESS_ANY     = L"0.0.0.0";

        // 0 means "not configured". There is no sensible default port to pick —
        // guessing one would make an unconfigured instance bind something.
        constexpr int PORT_UNSET = 0;
        constexpr int PORT_MIN   = 1;
        constexpr int PORT_MAX   = 65535;

        // Simultaneous clients. Values outside the range fall back to DEFAULT
        // rather than refusing to start.
        constexpr int MAX_CONNECTIONS_MIN     = 1;
        constexpr int MAX_CONNECTIONS_MAX     = 99;
        constexpr int MAX_CONNECTIONS_DEFAULT = 1;

        // Separators accepted inside AllowList / BlackList, so a hand-edited
        // file tolerates either convention.
        constexpr const wchar_t *LIST_SEPARATORS = L",;";

        // --- The driving side's target list (src/Rem_TCP_IP/RemotesFile.*) ---
        // Beside the exe, and DELIBERATELY not the exe-derived name: an .ini
        // called qIV.ini next to qIV.exe is what makes the whole app switch from
        // registry-backed to file-backed (Dedicated::DetectStartupMode). This
        // name is invisible to that check, so writing it changes nothing about
        // how the copy persists everything else.
        constexpr const wchar_t *REMOTES_FILE_NAME = L"qivRemotes.ini";
        constexpr const wchar_t *REMOTES_SECTION   = L"Remotes";

        // Upper bound on rows, so a corrupted file cannot spin the reader. Far
        // beyond any real setup — the use case is a handful of monitors.
        constexpr int REMOTES_MAX = 64;

        // --- Wire protocol -------------------------------------------------
        // Newline-delimited UTF-8 text: "<command> [payload]\n". Deliberately
        // plain, so netcat, curl, telnet and a five-line Python script are all
        // first-class clients with nothing to marshal.
        constexpr int PROTOCOL_VERSION = 1;

        // Hard cap on one received line. A socket must never be allowed to grow
        // a buffer without bound just by never sending a newline.
        constexpr size_t MAX_LINE_LEN = 4096;

        // Response prefixes. A client only ever needs to look at the first token.
        constexpr const wchar_t *RESP_OK  = L"OK";
        constexpr const wchar_t *RESP_ERR = L"ERR";

        // UNSOLICITED line: an observed instance reporting something it just
        // did, pushed down the connection its observer already holds open. The
        // only line the server ever sends that is not an answer to a request.
        //
        // A third prefix rather than an OK variant, because a client sitting in
        // a request/reply exchange has to be able to tell "this is my answer"
        // from "this happened meanwhile" without tracking state. Existing
        // clients that only branch on OK/ERR simply never see one: nothing is
        // ever pushed to a connection that did not send `observe 1`.
        constexpr const wchar_t *RESP_EVENT = L"EVENT";

        // --- Mirroring (src/Rem_TCP_IP/RemoteMirror.*) ----------------------
        // Per-target send queue depth. Bounded, and the OLDEST is dropped when
        // it overflows: a backlog of keystrokes for a machine that stopped
        // answering would replay minutes of stale navigation when it came back,
        // and a mirrored keystroke only means anything at the moment it is made.
        constexpr size_t MIRROR_QUEUE_MAX = 64;

        // How long an idle sender thread waits on the socket for an unsolicited
        // EVENT before looping to re-check its queue and the stop flag. Short
        // enough that F11 responds immediately, long enough not to spin.
        constexpr int MIRROR_IDLE_POLL_MS = 200;

        // Wait before retrying a target that would not connect. A slave that is
        // switched off must not be hammered, and its thread must not spin.
        constexpr int MIRROR_RECONNECT_MS = 3000;

        // Bound on a single mirrored send, so one wedged target cannot stall its
        // own queue indefinitely. Shorter than the server's REPLY_TIMEOUT_MS
        // because on loopback anything slower is already a fault.
        constexpr int MIRROR_SEND_TIMEOUT_MS = 3000;

        // Error codes carried after ERR. Stable numbers: scripts branch on these,
        // so they may be appended to but never renumbered.
        constexpr int ERR_UNKNOWN_COMMAND   = 1;
        constexpr int ERR_PAYLOAD_REQUIRED  = 2;
        constexpr int ERR_PAYLOAD_REJECTED  = 3;
        constexpr int ERR_LINE_TOO_LONG     = 4;
        constexpr int ERR_BAD_PAYLOAD       = 5;
        constexpr int ERR_NOT_AUTHENTICATED = 6;
        constexpr int ERR_AUTH_FAILED       = 7;
        constexpr int ERR_TOO_MANY_CLIENTS  = 8;
        constexpr int ERR_INTERNAL          = 9;
    }

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
        constexpr const wchar_t *OVERLAY_LAYOUT_MODE  = L"qivOverlayLayoutMode";
        constexpr const wchar_t *OVERLAY_SLOT_VISIBLE = L"qivOverlaySlotVisible"; // bitmask, bit N = slot N
        constexpr const wchar_t *OVERLAY_SLOT_COMPACT = L"qivOverlaySlotCompact"; // bitmask, bit N = slot N
        constexpr const wchar_t *OVERLAY_SHOW_DIR_NAME = L"qivOverlayShowDirName";
        constexpr const wchar_t *OVERLAY_FONT_SIZE     = L"qivOverlayFontSize";
        constexpr const wchar_t *OVERLAY_FONT_COLOR    = L"qivOverlayFontColor";
        constexpr const wchar_t *OVERLAY_FONT_FAMILY   = L"qivOverlayFontFamily"; // index into OVERLAY_FONT_FAMILIES
        constexpr const wchar_t *OPEN_DIRWND_ON_START = L"qivOpenDirWndOnStart";
        constexpr const wchar_t *LOCK_VIEWPORT = L"qivLockViewport";
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
        constexpr const wchar_t *KIOSK_LOCK               = L"qivKioskLock";
        constexpr const wchar_t *ALWAYS_ON_TOP            = L"qivAlwaysOnTop";
        constexpr const wchar_t *KEEP_DISPLAY_AWAKE       = L"qivKeepDisplayAwake";
        // Last image on screen at exit — reopened on the next launch so the app
        // resumes where it was left instead of prompting. Routes to the registry
        // or, when the app is .ini-backed, into that file like every other value.
        constexpr const wchar_t *LAST_IMAGE              = L"qivLastImage";
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
        // Layout mode cycled with O key — live value is app.overlayLayoutMode:
        //   0 — default 3×3 grid
        //   1 — the four corners stacked vertically on top-left
        //   2 — the four corners collapsed into a 2-line summary top-left:
        //         line 1: index / total + filename
        //         line 2: zoom% + WxH / size
        // Modes 1 and 2 re-arrange only the corners; the five non-corner slots
        // keep their grid positions in every mode.
        constexpr int DEFAULT_LAYOUT_MODE = 0;
        constexpr int LAYOUT_MODE_COUNT   = 3;

        // BOT_LEFT carries two independent readouts, in this order:
        //   the active-effects list, then the current folder name LAST.
        // The slot is bottom-anchored and grows upward, so the folder name
        // stays pinned to the bottom while effects stack above it. The two
        // toggles are independent — hiding one never hides the other.
        constexpr bool SHOW_DIR_NAME     = false; // persisted (qivOverlayShowDirName)
        constexpr bool SHOW_EFFECTS_LIST = true;  // session-only, not persisted

        // Per-slot visibility / compact state is persisted as one bit per slot,
        // indexed by OverlayManager::Slot. Nine slots → the low 9 bits.
        constexpr unsigned SLOT_MASK_ALL = 0x1FFu;
        constexpr unsigned DEFAULT_SLOT_VISIBLE_MASK = SLOT_MASK_ALL;
        constexpr unsigned DEFAULT_SLOT_COMPACT_MASK =
                IS_COMPACT_OVERLAY_MODE ? SLOT_MASK_ALL : 0u;

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

        // =========================================================================
        // Overlay — user-chosen text style for the eight OUTER slots
        // =========================================================================
        // MID_CENTER is deliberately excluded: it is a transient notification with
        // its own colour and size above, and it has to stay legible no matter what
        // the outer slots are set to.
        constexpr int OVERLAY_FONT_SIZE_MIN     = 1;
        constexpr int OVERLAY_FONT_SIZE_MAX     = 92;
        constexpr int OVERLAY_FONT_SIZE_DEFAULT = static_cast<int>(MSG_ALL_BUT_CENTER_FONT_SIZE);
        // LightGreen — the colour RendererD2D has always created its text brush
        // with, kept as the default so nothing changes until the user picks.
        constexpr COLORREF OVERLAY_FONT_COLOR_DEFAULT = RGB(144, 238, 144);
        // Offered in the Overlays ▸ Font submenu. Persisted as an INDEX into this
        // array, so only ever append — inserting or reordering would silently
        // repoint every saved setting.
        constexpr const wchar_t *OVERLAY_FONT_FAMILIES[] = {
            L"Segoe UI", L"Arial", L"Verdana", L"Tahoma", L"Calibri",
            L"Georgia", L"Times New Roman", L"Courier New", L"Consolas",
            L"Comic Sans MS", L"Impact", L"Trebuchet MS"
        };
        constexpr int OVERLAY_FONT_FAMILY_COUNT   = static_cast<int>(std::size(OVERLAY_FONT_FAMILIES));
        constexpr int OVERLAY_FONT_FAMILY_DEFAULT = 0; // Segoe UI — matches the constant above
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

        // How long the startup folder sweep waits before touching the disk.
        // It runs at background I/O priority anyway, but holding off entirely
        // until the first image is decoded and on screen keeps app launch as fast
        // as it was before the sweep existed. Only applies to the startup kick-off
        // — opening the panel or pressing F5 scans immediately.
        constexpr DWORD HISTORY_SCAN_STARTUP_DELAY_MS = 3000;

        // Window size limits (px at 96 DPI — DPI-scaled at runtime)
        constexpr int HISTORY_MIN_W = 690; // minimum panel width
        constexpr int HISTORY_MAX_W = IS_BASE_WIDTH  - 120; // maximum panel width
        constexpr int HISTORY_MIN_H = 620; // minimum panel height
        constexpr int HISTORY_MAX_H = IS_BASE_HEIGHT - 60; // maximum panel height (also capped to 80% of monitor)
    }

    // =========================================================================
    // Zoom Panel (F2 zoom-to dialog) — geometry constants
    // =========================================================================
    namespace ZoomPanel {
        constexpr float WINDOW_WIDTH = 340.0f;
        constexpr float WINDOW_HEIGHT = 140.0f;
        constexpr float PADDING = 14.0f;
        constexpr float GAP = 8.0f;
        constexpr float FONT_SIZE = 13.0f;
        constexpr float FONT_SIZE_INPUT = 16.0f;
        constexpr float INPUT_BOX_HEIGHT = 34.0f;
        constexpr int LABEL_EXTRA_HEIGHT = 6;     // extra px below label text
        constexpr int HINT_EXTRA_HEIGHT = 4;      // extra px below hint text
        // Base gray values passed to Constants::Theme::ThemedGray at draw time
        constexpr float LABEL_TEXT_GRAY = 0.9020f; // label: "Enter zoom multiplier..."
        constexpr float HINT_TEXT_GRAY = 0.50f;    // hint: "Enter = apply zoom..."
        // Zoom limits, in PERCENT — exactly the number the overlay displays and
        // the zoom panel accepts. 0.1 means 0.1%, 99999 means 99999%.
        //
        // They bound the EFFECTIVE on-screen zoom, NOT the raw app.viewport.zoom
        // multiplier. Effective zoom is (baseScale * viewport.zoom) and baseScale
        // depends on the view mode, so clamping the multiplier alone lets the real
        // zoom drift far past these bounds. Always clamp through
        // ClampZoomToLimits() in AppState.h, never with a bare std::clamp on
        // viewport.zoom.
        //
        // Unit changes go through Converters::PercentToRatio / RatioToPercent.
        constexpr float ZOOM_STEP = 1.1f;     // ratio factor per +/- key or wheel tick
        constexpr float ZOOM_MIN  = 0.01f;     // percent — smallest allowed zoom
        constexpr float ZOOM_MAX  = 99'999.0f; // percent — largest allowed zoom single quote is number separator " ' " like in java "_"
        constexpr int   INPUT_MAX_CHARS = 10; // must be < ZoomWnd::m_input capacity
    }

    // Outcome of ClampZoomToLimits() (AppState.h). Lives here rather than in
    // AppState.h so OverlayManager.h can consume it without pulling in AppState.
    // Callers use it to tell the user WHY a zoom keypress did nothing.
    enum class ZoomClampResult {
        None,       // zoom was already inside the limits — nothing was capped
        ClampedMin, // hit ZOOM_MIN, cannot zoom out further
        ClampedMax  // hit ZOOM_MAX, cannot zoom in further
    };

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

    namespace Cursors {
          inline const HCURSOR CURR_ZOOM =  LoadCursorW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDC_ZOOM_CURSOR_L));
          inline const HCURSOR CURR_GRAB =  LoadCursorW(GetModuleHandleW(nullptr),  MAKEINTRESOURCEW(IDC_GRAB_CURSOR_L));
          inline const HCURSOR CURR_DEFAULT =  LoadCursorW(nullptr, IDC_ARROW);
          inline const HCURSOR CURR_CLICK =  LoadCursorW(nullptr, IDC_HAND);
          inline const HCURSOR CURR_APPSTARTING =  LoadCursorW(nullptr, IDC_APPSTARTING);
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

        // Default TRANS_LIST bitmask: every animated type ticked, bit 0 (Cut)
        // cleared — a "transition list" containing Cut would mean "no transition".
        // Used as the registry default AND by Restore Defaults; keep it here so
        // the two cannot drift apart.
        constexpr uint32_t TRANSITION_LIST_DEFAULT_MASK = 0xFFFFFFFEu;

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
