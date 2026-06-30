#pragma once
#include <intsafe.h>
//
// Shortcuts.h  —  Single source of truth for all keyboard shortcuts in QIV.
//
// HOW TO ADD A NEW SHORTCUT:
//   1. Define the key/modifier here with a comment describing what it does.
//   2. Use the constant in the WM_KEYDOWN handler (AppMain.cpp) or the
//      relevant WndProc (CacheWindow.cpp, HelpWindow.cpp, etc.).
//   3. Add it to the help text in HelpWindow.cpp.
//
// MODIFIER FLAGS (read at runtime via GetKeyState):
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
    constexpr UINT SC_APP_HIDE = VK_ESCAPE;
    constexpr UINT SC_APP_HIDE_ALT = 'W'; // requires ctrl

    // Ctrl+N  —  Spawn a new blank QIV window
    constexpr UINT SC_APP_NEW_WINDOW = 'N'; // requires ctrl

    // -------------------------------------------------------------------------
    // Panels / Overlays SC_PANEL = CacheWindow
    // -------------------------------------------------------------------------

    // F1  —  Toggle Help overlay
    constexpr UINT SC_PANEL_HELP_TOGGLE = VK_F1;

    // F2  —  Open file picker dialog
    constexpr UINT SC_PANEL_OPEN_FILE = VK_F2;

    // F3  —  Toggle VRAM Cache panel
    constexpr UINT SC_PANEL_CACHE_TOGGLE = VK_F3;

    // F4  —  Cycle VRAM Cache panel position (bottom / top / left / right)
    //        Handled inside CacheWindow's own WndProc when the panel has focus.
    constexpr UINT SC_PANEL_CACHE_MOVE = VK_F4;

    // F5  —  Toggle Directory panel (shows all images in current folder)
    constexpr UINT SC_PANEL_DIR_TOGGLE = VK_F5;

    // F6  —  Cycle Directory panel position (bottom / top / left / right)
    //        Handled inside DirWindow's own WndProc when the panel has focus.
    constexpr UINT SC_PANEL_DIR_MOVE = VK_F6;

    // F11 / F / Enter / Ctrl+Shift+T  —  Toggle fullscreen
    constexpr UINT SC_PANEL_FULLSCREEN = VK_F11;
    constexpr UINT SC_PANEL_FULLSCREEN_F = 'F';
    constexpr UINT SC_PANEL_FULLSCREEN_ENTER = VK_RETURN;
    constexpr UINT SC_PANEL_FULLSCREEN_T = 'T'; // requires ctrl+shift

    // F12 —  Clear VRAM Cache and reset cache window UI
    constexpr UINT SC_PANEL_CACHE_CLEAR = VK_F12;

    // N  —  Toggle on-screen info text overlay
    constexpr UINT SC_PANEL_OVERLAY_TOGGLE = 'N'; // no modifier

    // -------------------------------------------------------------------------
    // Navigation
    // -------------------------------------------------------------------------

    // Left Arrow  —  Previous image
    constexpr UINT SC_NAV_PREV = VK_LEFT;

    // Right Arrow  —  Next image
    constexpr UINT SC_NAV_NEXT = VK_RIGHT;

    // Space       —  Next image  /  Shift+Space  —  Previous image
    constexpr UINT SC_NAV_NEXT_SPACE = VK_SPACE; // no modifier = next
    // shift = prev

    // E / Tab  —  Open current file in Explorer (select in folder)
    constexpr UINT SC_NAV_SHOW_IN_EXPLORER = 'E';
    constexpr UINT SC_NAV_SHOW_IN_EXPLORER_TAB = VK_TAB;

    // -------------------------------------------------------------------------
    // Zoom
    // -------------------------------------------------------------------------

    // Up / Numpad+ / +  —  Zoom in
    constexpr UINT SC_ZOOM_IN = VK_UP;
    constexpr UINT SC_ZOOM_IN_NUMPAD = VK_ADD;
    constexpr UINT SC_ZOOM_IN_OEM = VK_OEM_PLUS;

    // Down / Numpad- / -  —  Zoom out
    constexpr UINT SC_ZOOM_OUT = VK_DOWN;
    constexpr UINT SC_ZOOM_OUT_NUMPAD = VK_SUBTRACT;
    constexpr UINT SC_ZOOM_OUT_OEM = VK_OEM_MINUS;

    // 0 / Numpad0  —  Reset zoom and pan to 1:1, centered
    constexpr UINT SC_ZOOM_RESET = '0';
    constexpr UINT SC_ZOOM_RESET_NUMPAD = VK_NUMPAD0;

    // Mouse: Ctrl+Wheel  —  Zoom in/out (handled in WM_MOUSEWHEEL)
    // Mouse: RMB drag    —  Pan image
    // Mouse: LMB click   —  Quick zoom-to ZOOM_CLICK and back

    // -------------------------------------------------------------------------
    // View Modes  (keys '1'–'5')
    // -------------------------------------------------------------------------
    // 1  —  Fit to view, preserve aspect ratio         (default)
    // 2  —  Fit to width, ignore aspect ratio
    // 3  —  Fit to height, ignore aspect ratio
    // 4  —  Fit to window, ignore aspect ratio
    // 5  —  Original 1:1 pixel size, preserve aspect ratio
    // Handled as:  wParam >= '1' && wParam <= '5'

    // -------------------------------------------------------------------------
    // Transform
    // -------------------------------------------------------------------------

    // R          —  Rotate clockwise 90°
    // Shift+R    —  Rotate counter-clockwise 90°
    constexpr UINT SC_TRANSFORM_ROTATE = 'R';

    // H  —  Flip horizontal
    constexpr UINT SC_TRANSFORM_FLIP_H = 'H';

    // V  —  Flip vertical
    constexpr UINT SC_TRANSFORM_FLIP_V = 'V';

    // -------------------------------------------------------------------------
    // Color Effects
    // -------------------------------------------------------------------------

    // [  —  Saturation -0.1
    constexpr UINT SC_COLOR_SAT_DOWN = VK_OEM_4;

    // ]  —  Saturation +0.1
    constexpr UINT SC_COLOR_SAT_UP = VK_OEM_6;

    // I  —  Toggle grayscale (saturation 0 ↔ 1)
    constexpr UINT SC_COLOR_GRAYSCALE = 'I';

    // B          —  Brightness +0.1
    // Shift+B    —  Brightness -0.1
    constexpr UINT SC_COLOR_BRIGHT = 'B';

    // C          —  Contrast +0.1
    // Shift+C    —  Contrast -0.1
    constexpr UINT SC_COLOR_CONTRAST = 'C';

    // Shift+Delete  —  Reset all color effects to defaults
    constexpr UINT SC_COLOR_RESET = VK_DELETE; // requires shift

    // -------------------------------------------------------------------------
    // Panel-local shortcuts (handled in each panel's own WndProc)
    // -------------------------------------------------------------------------

    // Esc  —  Hide the panel that currently has focus
    constexpr UINT SC_LOCAL_HIDE = VK_ESCAPE;

    // F1  —  (HelpWindow) close help
    // F3  —  (CacheWindow) toggle cache panel
    // F4  —  (CacheWindow) cycle cache panel position
    // F5  —  (DirWindow)   toggle dir panel
    // F6  —  (DirWindow)   cycle dir panel position
    // These reuse the SC_PANEL_* constants above.
} // namespace Shortcuts
