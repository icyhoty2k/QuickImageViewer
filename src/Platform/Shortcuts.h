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

    // Shift+Delete  — Restore default application state
    constexpr UINT SC_APP_RESET_DEFAULTS = VK_DELETE; // Shift+Delete — Reset window layout, center the window, and clear all image effects.

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

    // Left Arrow / Up Arrow  —  Previous image
    constexpr UINT SC_NAV_PREV = VK_LEFT;
    constexpr UINT SC_NAV_PREV_A = VK_UP;

    // Right Arrow / Down Arrow  —  Next image
    constexpr UINT SC_NAV_NEXT_A = VK_DOWN;
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

    // Numpad+ / +  —  Zoom in
    constexpr UINT SC_ZOOM_IN_NUMPAD = VK_ADD;


    // Numpad- / -  —  Zoom out
    constexpr UINT SC_ZOOM_OUT_NUMPAD = VK_SUBTRACT;

    constexpr UINT SC_ZOOM_RESET = VK_MULTIPLY; // —  Reset zoom and pan to 1:1, centered

    // Mouse: Ctrl+Wheel / RMB(view-control)+Wheel  —  Zoom in/out (handled in WM_MOUSEWHEEL)
    // See Shortcuts::REFERENCE_ONLY::MouseShortcuts below for the full, accurate
    // mouse map — it depends on Constants::SWAP_MOUSE_BUTTONS.

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
    // Panel-local shortcuts (handled in each panel's own WndProc)
    // -------------------------------------------------------------------------

    // Esc  —  Hide the panel that currently has focus
    constexpr UINT SC_LOCAL_HIDE = VK_ESCAPE;

    // F1  —  (HelpWindow) close help
    // F3  —  (CacheWindow) toggle cache panel
    // F4  —  (CacheWindow) cycle cache panel position
    // F5  —  (DirWindow)   toggle dir panel
    // F6  —  (DirWindow)   cycle dir panel position
    // These reuse the SC_PANEL_*

    // -------------------------------------------------------------------------
    // Color Effects
    // -------------------------------------------------------------------------
    namespace ImageEffects {
        // Dedicated image effect keys (do NOT use letters here — letters are
        // reserved for transform/navigation; effects live on Insert, Delete,
        // Home, End, Page Up, Page Down, plus the punctuation cluster for
        // continuous adjustments). The old 'I' grayscale toggle and the old
        // B / Shift+B / C / Shift+C brightness/contrast keys have been
        // removed — AppMain.cpp must only use the constants below.
        constexpr UINT SC_COLOR_GRAYSCALE = VK_DELETE; // Toggle grayscale
        constexpr UINT SC_COLOR_INVERT = VK_INSERT; // Toggle invert colors
        constexpr UINT SC_COLOR_SEPIA = VK_HOME; // Toggle sepia
        constexpr UINT SC_COLOR_SOLARIZE = VK_END; // Toggle solarize
        constexpr UINT SC_COLOR_OUTLINE = VK_PRIOR; // Toggle image outline
        constexpr UINT SC_COLOR_THRESHOLD = VK_NEXT; // Toggle black & white threshold

        constexpr UINT SC_COLOR_GAMMA_UP = VK_OEM_PLUS; // +/- 0.1 // but not the numpad plus i use it for zoom+
        constexpr UINT SC_COLOR_GAMMA_DOWN = VK_OEM_MINUS; // +/- 0.1 // but not the numpad plus i use it for zoom-
        constexpr UINT SC_COLOR_BRIGHTNESS_UP = '\''; // +/- 0.1
        constexpr UINT SC_COLOR_BRIGHTNESS_DOWN = '\\';
        constexpr UINT SC_COLOR_CONTRAST_UP = '.'; // +/- 0.1
        constexpr UINT SC_COLOR_CONTRAST_DOWN = '/';
        constexpr UINT SC_COLOR_SAT_DOWN = VK_OEM_4; // +/- 0.1
        constexpr UINT SC_COLOR_SAT_UP = VK_OEM_6;

        constexpr UINT SC_COLOR_RESET_ALL_EFFECTS = VK_NUMPAD0; // Reset all color effects
        constexpr UINT SC_COLOR_SAVE_TO_DISK = 'S'; //Ctrl + s Save image with effects to disc, don't change size and aspect ratio just save it with effects
    }

    namespace REFERENCE_ONLY::MouseShortcuts {
        // Here I will put all mouse shortcuts just for reference!
        //
        // Mouse buttons are NOT remapped via constants like keyboard keys —
        // WM_LBUTTONDOWN / WM_RBUTTONDOWN are intrinsic Windows messages.
        // Which physical button does which job is decided at runtime by
        // Constants::SWAP_MOUSE_BUTTONS (see MouseHandler::IsDragAction /
        // IsViewControlAction). This list documents the CURRENT behavior
        // with Constants::SWAP_MOUSE_BUTTONS = true (the shipped default).
        //
        // -------------------------------------------------------------
        // "View-control button"  = LMB   (RMB if SWAP_MOUSE_BUTTONS = false)
        // "Window-drag button"   = RMB   (LMB if SWAP_MOUSE_BUTTONS = false)
        // -------------------------------------------------------------
        //
        // View-control button, click+hold —  Quick zoom to Constants::ZOOM_CLICK
        //                                     (3x) centered on the cursor.
        // View-control button, drag       —  While held, pans the temporarily
        //                                     zoomed image (offset only; zoom
        //                                     and pan revert on release).
        //
        // Window-drag button, click+hold  —  Begins moving the window
        //                                     (disabled while fullscreen).
        // Window-drag button, drag        —  Moves the window.
        //
        // Window-drag button HELD + View-control button CLICK
        //                                  —  Reveals the current file in
        //                                     Windows Explorer (same as E / Tab).
        //
        // Middle Mouse Button, click (no movement)
        //                                  —  Reset: zoom/pan to 1:1, opacity
        //                                     to full, window resized to
        //                                     Constants::BASE_WIDTH x
        //                                     BASE_HEIGHT and centered on the
        //                                     current monitor.
        // Middle Mouse Button, drag       —  Live-resizes the window from its
        //                                     top-left corner (disabled while
        //                                     fullscreen).
        //
        // LMB double-click                —  Toggle fullscreen (same as
        //                                     SC_PANEL_FULLSCREEN).
        //
        // Wheel (vertical), no modifier   —  Navigate: forward/up = previous
        //                                     image, back/down = next image.
        // Ctrl + Wheel (vertical)         —  Zoom in (up) / out (down) by
        //                                     Constants::ZOOM_STEP.
        // RMB held + Wheel (vertical)     —  Same as Ctrl+Wheel: zoom in/out.
        // Shift + Wheel (vertical)        —  Adjust window/image opacity by
        //                                     Constants::OPACITY_STEP.
        //
        // Wheel (horizontal), no modifier —  Adjust window/image opacity
        //                                     (same as Shift+vertical wheel).
        // RMB held + Wheel (horizontal)   —  Live-resize the window from its
        //                                     center, 20px per notch.
    }
} // namespace Shortcuts
