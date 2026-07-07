#pragma once
//
// Shortcuts.h  —  Single source of truth for all keyboard shortcuts in QIV.
//
// HOW TO ADD A NEW SHORTCUT:
//   1. Define the key/modifier here with a comment describing what it does.
//   2. Use the constant in the WM_KEYDOWN handler (AppMain.cpp) or the
//      relevant WndProc (CacheWindow.cpp, HelpWindow.cpp, etc.).
//   3. Add it to the help text in HelpWindow.cpp.
//
// MODIFIER FLAGS (read at runtime via GetKeyState):\
//   bool shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
//   bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
//
// KEY FORMAT:  SC_<GROUP>_<ACTION>
// =============================================================================

namespace Shortcuts {
    // -------------------------------------------------------------------------
    // Application
    // -------------------------------------------------------------------------

    // Ctrl+Q  —  Hard quit: removes process from RAM completely
    constexpr UINT SC_APP_HARD_QUIT = 'Q'; // requires ctrl

    // Esc  /  Ctrl+W  —  Hide to tray (keeps process alive); kills extra instances
    constexpr UINT SC_APP_HIDE     = VK_ESCAPE;
    constexpr UINT SC_APP_HIDE_ALT = 'W'; // requires ctrl

    // Ctrl+N  —  Spawn a new blank QIV window
    constexpr UINT SC_APP_NEW_WINDOW = 'N'; // requires ctrl

    // Shift+Delete  — Restore default application state
    constexpr UINT SC_APP_RESET_DEFAULTS = VK_DELETE;

    // -------------------------------------------------------------------------
    // Panels / Overlays
    // -------------------------------------------------------------------------

    constexpr UINT SC_PANEL_HELP_TOGGLE    = VK_F1;
    constexpr UINT SC_PANEL_OPEN_FILE      = VK_F2;
    constexpr UINT SC_PANEL_CACHE_TOGGLE   = VK_F3;
    constexpr UINT SC_PANEL_CACHE_MOVE     = VK_F4;
    constexpr UINT SC_PANEL_DIR_TOGGLE     = VK_F5;
    constexpr UINT SC_PANEL_DIR_MOVE       = VK_F6;
    constexpr UINT SC_PANEL_HISTORY_TOGGLE = VK_TAB;
    constexpr UINT SC_PANEL_FULLSCREEN     = VK_F11;
    constexpr UINT SC_PANEL_FULLSCREEN_F   = 'F';
    constexpr UINT SC_PANEL_FULLSCREEN_ENTER = VK_RETURN;
    constexpr UINT SC_PANEL_FULLSCREEN_T   = 'T'; // requires ctrl+shift
    constexpr UINT SC_PANEL_CACHE_CLEAR    = VK_F12;

    // N (no modifier)  —  Master overlay toggle (all slots on/off)
    constexpr UINT SC_PANEL_OVERLAY_TOGGLE = 'N';

    // I (no modifier)  —  Master overlay toggle — same effect as N
    constexpr UINT SC_PANEL_OVERLAY_MASTER = 'I';

    // Per-slot individual toggles — Ctrl+Alt+1..4  (no shift)
    //   Ctrl+Alt+1  —  TOP_RIGHT  (index / filename)
    //   Ctrl+Alt+2  —  TOP_CENTER (zoom)
    //   Ctrl+Alt+3  —  BOT_RIGHT  (dims / file size)
    //   Ctrl+Alt+4  —  BOT_LEFT   (effects list)
    constexpr UINT SC_OVERLAY_SLOT_TOP_RIGHT  = '1'; // requires ctrl+alt
    constexpr UINT SC_OVERLAY_SLOT_TOP_CENTER = '2'; // requires ctrl+alt
    constexpr UINT SC_OVERLAY_SLOT_BOT_RIGHT  = '3'; // requires ctrl+alt
    constexpr UINT SC_OVERLAY_SLOT_BOT_LEFT   = '4'; // requires ctrl+alt

    // -------------------------------------------------------------------------
    // Navigation
    // -------------------------------------------------------------------------

    constexpr UINT SC_NAV_PREV            = VK_LEFT;
    constexpr UINT SC_NAV_NEXT            = VK_RIGHT;
    constexpr UINT SC_NAV_NEXT_SPACE      = VK_SPACE;
    constexpr UINT SC_NAV_SHOW_IN_EXPLORER = 'E';

    // -------------------------------------------------------------------------
    // Zoom
    // -------------------------------------------------------------------------

    constexpr UINT SC_ZOOM_IN_NUMPAD  = VK_ADD;
    constexpr UINT SC_ZOOM_OUT_NUMPAD = VK_SUBTRACT;
    constexpr UINT SC_ZOOM_RESET      = VK_MULTIPLY;

    // -------------------------------------------------------------------------
    // View Modes  (keys '1'–'5')
    // -------------------------------------------------------------------------

    constexpr UINT SC_VIEW_MODE_FIRST = '1';
    constexpr UINT SC_VIEW_MODE_LAST  = '5';

    // -------------------------------------------------------------------------
    // Transform
    // -------------------------------------------------------------------------

    constexpr UINT SC_TRANSFORM_ROTATE = 'R';
    constexpr UINT SC_TRANSFORM_FLIP_H = 'H';
    constexpr UINT SC_TRANSFORM_FLIP_V = 'V';

    // -------------------------------------------------------------------------
    // Panel-local shortcuts (handled in each panel's own WndProc)
    // -------------------------------------------------------------------------

    constexpr UINT SC_LOCAL_HIDE = VK_ESCAPE;

    // -------------------------------------------------------------------------
    // Color Effects
    // -------------------------------------------------------------------------
    namespace ImageEffects {
        constexpr UINT SC_EFFECT_APPLY_TOGGLE      = VK_OEM_3;    // grave key `
        constexpr UINT SC_COLOR_GRAYSCALE          = VK_DELETE;
        constexpr UINT SC_COLOR_INVERT             = VK_INSERT;
        constexpr UINT SC_COLOR_SEPIA              = VK_HOME;
        constexpr UINT SC_COLOR_SOLARIZE           = VK_END;
        constexpr UINT SC_COLOR_OUTLINE            = VK_PRIOR;
        constexpr UINT SC_COLOR_THRESHOLD          = VK_NEXT;
        constexpr UINT SC_COLOR_GAMMA_UP           = VK_OEM_PLUS;
        constexpr UINT SC_COLOR_GAMMA_DOWN         = VK_OEM_MINUS;
        constexpr UINT SC_COLOR_BRIGHTNESS_UP      = VK_OEM_5;    // backslash
        constexpr UINT SC_COLOR_BRIGHTNESS_DOWN    = VK_OEM_7;    // apostrophe
        constexpr UINT SC_COLOR_CONTRAST_UP        = VK_OEM_2;    // forward slash
        constexpr UINT SC_COLOR_CONTRAST_DOWN      = VK_OEM_PERIOD;
        constexpr UINT SC_COLOR_SAT_DOWN           = VK_OEM_4;
        constexpr UINT SC_COLOR_SAT_UP             = VK_OEM_6;
        constexpr UINT SC_COLOR_RESET_ALL_EFFECTS  = VK_NUMPAD0;
        constexpr UINT SC_COLOR_SAVE_TO_DISK       = 'S';         // requires ctrl
    }

    namespace REFERENCE_ONLY::MouseShortcuts {
        // Mouse buttons are NOT remapped via constants like keyboard keys —
        // WM_LBUTTONDOWN / WM_RBUTTONDOWN are intrinsic Windows messages.
        // Which physical button does which job is decided at runtime by
        // Constants::SWAP_MOUSE_BUTTONS.
        //
        // With Constants::SWAP_MOUSE_BUTTONS = true (shipped default):
        //
        // LMB hold          — Quick zoom to Constants::ZOOM_CLICK (3x), centered on cursor
        // LMB drag          — Pan while temporarily zoomed (reverts on release)
        // RMB hold          — Move the window
        // RMB drag          — Move the window
        // RMB held + LMB    — Reveal current file in Explorer
        // MMB click         — Reset zoom/pan/opacity, resize window to default, center on monitor
        // MMB drag          — Live-resize window from top-left corner
        // LMB double-click  — Toggle fullscreen
        // Wheel up/down     — Previous / next image
        // Ctrl+Wheel        — Zoom in / out
        // RMB held + Wheel  — Zoom in / out
        // Shift+Wheel       — Adjust opacity
        // Horizontal wheel  — Adjust opacity
        // RMB held + H-wheel — Live-resize window from center (20px per notch)
    }
} // namespace Shortcuts
