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
    constexpr UINT SC_APP_HIDE = VK_ESCAPE;
    constexpr UINT SC_APP_HIDE_ALT = 'W'; // requires ctrl // used in IPanelWindow too ! to unifi behaviour

    // Ctrl+N  —  Spawn a new blank QIV window
    constexpr UINT SC_APP_NEW_WINDOW = 'N'; // requires ctrl

    // N  —  Toggle all panels: close every floating panel + spawned DirWnd if any
    //       is visible, otherwise restore the set the last close hid (main window
    //       stays). Shares the 'N' key with Ctrl+N (new window), split by modifier.
    constexpr UINT SC_TOGGLE_ALL_PANELS = 'N'; // plain

    // Shift+Delete  — Restore default application state
    constexpr UINT SC_APP_RESET_DEFAULTS = VK_DELETE;
    // -------------------------------------------------------------------------
    // IPanelWindow.h shortcuts for all chield windows
    // -------------------------------------------------------------------------

    constexpr UINT IPANNEL_WINDOW_LOCAL_HIDE = VK_ESCAPE;
    // -------------------------------------------------------------------------
    // Panels / Overlays
    // -------------------------------------------------------------------------

    constexpr UINT SC_SLIDESHOW_TOGGLE = VK_F1; // Ctrl+F1 — start / stop
    constexpr UINT SC_SLIDESHOW_PAUSE_RESUME = VK_SPACE; // Space   — pause / resume (slideshow only)
    constexpr UINT SC_SLIDESHOW_LOOP_TOGGLE = 'R'; // R       — toggle loop/repeat (slideshow only)
    constexpr UINT SC_SLIDESHOW_SHUFFLE_TOGGLE = 'S'; // S       — toggle shuffle (slideshow only)
    constexpr UINT SC_SLIDESHOW_TRANSITION_CYCLE = 'T'; // T       — cycle transition type (slideshow only)
    constexpr UINT SC_PANEL_HELP_TOGGLE = VK_F1; // plain F1 — help window
    constexpr UINT SC_PANEL_OPEN_FILE = VK_F2;
    constexpr UINT SC_PANEL_CACHE_TOGGLE = VK_F3;
    constexpr UINT SC_PANEL_CACHE_MOVE = VK_F4;
    constexpr UINT SC_APP_RELOAD_CURRENT_DIR = VK_F5; //refresh / reload current dir !
    constexpr UINT SC_PANEL_DIR_TOGGLE = VK_F6;
    constexpr BYTE SC_RIGHT_SHIFT_SCANCODE = 0x36; // This is the scanCode of rightShift
    constexpr UINT SC_PANEL_DIR_MOVE = VK_F7;
    // F8 — Dedicated panel: configure and generate named dedicated instances
    // (see src/Dedicated). Deliberately its own window rather than a menu, since
    // it is a form with folders, ranges and toggles.
    constexpr UINT SC_PANEL_DEDICATED_TOGGLE = VK_F8;
    // F9 — Local Server (src/Rem_TCP_IP): start/stop the TCP listener THIS
    // instance runs and edit its configuration. Its own window for the same
    // reason F8 is: it is a form, not a menu.
    constexpr UINT SC_PANEL_REMOTE_TOGGLE = VK_F9;
    // F10 — Remote Servers (src/Rem_TCP_IP): the list of slave instances with
    // their live status, and the buttons to start/stop, observe and sync each.
    // F9 is what others connect TO; F10 is what this connects OUT to.
    // Ctrl+F10 — Send Command (Rem_TCP_IP/RemoteCmdWnd.h): type a command and
    //       send it to the instances under Control. The manual counterpart to
    //       F11 — it sends what you TYPE, including the commands that have no
    //       key at all.
    constexpr UINT SC_PANEL_REMOTES_CONSOLE = VK_F10;
    constexpr UINT SC_PANEL_HISTORY_TOGGLE = VK_TAB;
    // NOTE: F11 no longer toggles fullscreen — it is SC_MIRROR_TOGGLE below.
    // Fullscreen keeps three bindings (F, Enter, Ctrl+Shift+T), so nothing was
    // lost by freeing it.
    constexpr UINT SC_PANEL_FULLSCREEN_F = 'F';
    constexpr UINT SC_PANEL_FULLSCREEN_ENTER = VK_RETURN;
    constexpr UINT SC_PANEL_FULLSCREEN_T = 'T'; // requires ctrl+shift
    constexpr UINT SC_ALWAYS_ON_TOP = 'T'; // Ctrl+T (no shift) — toggle always-on-top
    // Ctrl+F3 — clear the thumbnail cache. Moved off F12 (which is now
    // SC_MIRROR_LOCAL_TOGGLE) onto the modifier form of F3, the key that already
    // toggles the cache panel, so the two cache actions sit on one key.
    constexpr UINT SC_PANEL_CACHE_CLEAR = VK_F3; // requires ctrl

    // -------------------------------------------------------------------------
    // Mirroring to other instances  (src/Rem_TCP_IP)
    // -------------------------------------------------------------------------
    // F11 — forward every mirrorable command to the connected targets. Nothing
    //       but the toggle: it asks no question and puts nothing up, because
    //       this is the path that has to be instant.
    // Ctrl+F11 — the selection panel: WHICH connected instances F11 drives, plus
    //       Sync now. A real window (Rem_TCP_IP/MirrorPickerWnd.h) that can be
    //       left open beside the pictures, not a popup — Ctrl rather than Shift
    //       to match the other panel shortcuts.
    // F12 — while mirroring, ALSO execute locally. With F11 on and F12 off this
    //       viewer is a pure remote control: it drives the other screens and its
    //       own display does not move.
    // Ctrl+F12 — RemoteLog (Rem_TCP_IP/RemoteLogWnd.h): every line that crossed
    //       the wire, both directions, with round-trip times. Recording is OFF
    //       by default and switched on inside that panel — which also pushes the
    //       same switch to the connected instances.
    // Both are session-only and start OFF at every launch. Persisting them would
    // mean a viewer that boots silently driving machines you had forgotten about.
    constexpr UINT SC_MIRROR_TOGGLE = VK_F11;
    constexpr UINT SC_MIRROR_LOCAL_TOGGLE = VK_F12;

    // Ctrl+Enter — PUSH this viewer's folder and current image at the instances
    // under Control (the ticked rows in Ctrl+F11), in one act. The point is
    // preparing a picture here and then putting it on the other screens: F11
    // forwards keystrokes, which only lines two viewers up if they already hold
    // the same folder — this states the folder and the file outright.
    //
    // Deliberately NOT gated on F11. It is an explicit push, like Sync now: a
    // viewer with mirroring off is exactly the case this exists for.
    //
    // Same key as fullscreen, whose PLAIN form is untouched (Enter, and also 'F'
    // and Ctrl+Shift+T) — only the Ctrl form is taken.
    constexpr UINT SC_REMOTE_SEND_POSITION = VK_RETURN; // requires ctrl

    // Alt+Enter — send ONE picture to those same instances to be shown ONCE, and
    // change nothing else about them: no folder, no sort order, no playlist
    // position. It goes up in place of the current slide and the far end carries
    // on exactly as it was — an advert dropped into a running slideshow.
    //
    // The picture's own BYTES travel, so this works to an instance on ANOTHER
    // MACHINE — which Ctrl+Enter cannot, since a folder path and an index are read
    // against the far end's own disk. It is also the half a phone app can speak.
    //
    // Alt+Enter used to be a fullscreen combination. Fullscreen keeps plain Enter,
    // 'F' and Ctrl+Shift+T, so nothing was lost.
    constexpr UINT SC_REMOTE_STREAM_IMAGE_OUT = VK_RETURN; // requires alt

    // Ctrl+Alt+Enter — the same transfer INBOUND: ask one instance for the picture
    // it is displaying and show it here, once. For seeing what a screen in another
    // room has up without walking to it, so it carries bytes for the same reason.
    //
    // All three live on Enter because they are one question at three depths — go
    // there, show this there, show that here.
    constexpr UINT SC_REMOTE_STREAM_IMAGE_IN = VK_RETURN; // requires ctrl+alt

    // N (no modifier)  —  Master overlay toggle (all slots on/off)
    constexpr UINT SC_PANEL_OVERLAY_TOGGLE = 'N'; // not used

    // I (no modifier)  —  Master overlay toggle — same effect as N and Ctrl+0
    constexpr UINT SC_PANEL_OVERLAY_MASTER = 'I';

    // O (no modifier)  —  Cycle overlay layout mode: 0 → 1 → 2 → 0
    constexpr UINT SC_OVERLAY_LAYOUT_CYCLE = 'O';

    // P (no modifier)  —  Toggle overlay text background on/off (text stays visible)
    constexpr UINT SC_OVERLAY_BG_TOGGLE = 'P';

    // Ctrl+0  —  Master overlay toggle (same as N / I)
    constexpr UINT SC_PANEL_OVERLAY_MASTER_CTRL0 = '0'; // requires ctrl

    // ── Per-slot STATE cycle — Ctrl+1 .. Ctrl+9  ─────────────────────────────
    // Grid layout:
    //   [1] TOP_LEFT    [2] TOP_CENTER    [3] TOP_RIGHT
    //   [4] MID_LEFT    [5] MID_CENTER    [6] MID_RIGHT
    //   [7] BOT_LEFT    [8] BOT_CENTER    [9] BOT_RIGHT
    //
    // Each press walks that slot through Compact → Full → Off, matching the
    // Overlays submenu. MID_CENTER has no compact form, so it cycles On → Off.
    // Compact mode has no shortcut of its own — this cycle replaced it.
    // All require ctrl, no alt, no shift.
    constexpr UINT SC_OVERLAY_SLOT_1 = '1'; // Ctrl+1  →  TOP_LEFT
    constexpr UINT SC_OVERLAY_SLOT_2 = '2'; // Ctrl+2  →  TOP_CENTER
    constexpr UINT SC_OVERLAY_SLOT_3 = '3'; // Ctrl+3  →  TOP_RIGHT
    constexpr UINT SC_OVERLAY_SLOT_4 = '4'; // Ctrl+4  →  MID_LEFT
    constexpr UINT SC_OVERLAY_SLOT_5 = '5'; // Ctrl+5  →  MID_CENTER  (message queue)
    constexpr UINT SC_OVERLAY_SLOT_6 = '6'; // Ctrl+6  →  MID_RIGHT
    constexpr UINT SC_OVERLAY_SLOT_7 = '7'; // Ctrl+7  →  BOT_LEFT
    constexpr UINT SC_OVERLAY_SLOT_8 = '8'; // Ctrl+8  →  BOT_CENTER
    constexpr UINT SC_OVERLAY_SLOT_9 = '9'; // Ctrl+9  →  BOT_RIGHT

    // -------------------------------------------------------------------------
    // Navigation
    // -------------------------------------------------------------------------

    // W/A/S/D — viewport pan (plain) / window move (Shift)
    // Note: SC_APP_HIDE_ALT = 'W' (ctrl+W hides; plain W pans up; Shift+W moves window up)
    //       SC_COLOR_SAVE_TO_DISK = 'S' (ctrl+S saves; plain S pans down; Shift+S moves window down)
    constexpr UINT SC_PAN_LEFT = 'A'; // plain: pan left;  Shift: move window left
    constexpr UINT SC_ALWAYS_ON_TOP_A = 'A'; // Ctrl+A — toggle always-on-top
    constexpr UINT SC_PAN_RIGHT = 'D'; // plain: pan right; Shift: move window right

    constexpr UINT SC_NAV_PREV = VK_LEFT;
    constexpr UINT SC_NAV_NEXT = VK_RIGHT;
    constexpr UINT SC_NAV_NEXT_SPACE = VK_SPACE;
    constexpr UINT SC_NAV_SHOW_IN_EXPLORER = 'L';
    // toggle first / last image in folder , Shift+Backspace go to last image which is not first or last
    constexpr UINT SC_NAV_TOGGLE_FIRST_LAST_IMAGE_IN_CURR_FOLDER = VK_BACK;

    // -------------------------------------------------------------------------
    // Zoom
    // -------------------------------------------------------------------------

    constexpr UINT SC_ZOOM_IN_NUMPAD = VK_ADD;
    constexpr UINT SC_ZOOM_TO = '0';
    constexpr UINT SC_ZOOM_OUT_NUMPAD = VK_SUBTRACT;
    constexpr UINT SC_ZOOM_RESET = VK_MULTIPLY;

    // -------------------------------------------------------------------------
    // View Modes  (keys '1'–'5')
    // -------------------------------------------------------------------------

    constexpr UINT SC_VIEW_MODE_FIRST = '1';
    constexpr UINT SC_VIEW_MODE_LAST = '5';

    // -------------------------------------------------------------------------
    // Transform
    // -------------------------------------------------------------------------

    constexpr UINT SC_TRANSFORM_ROTATE = 'R';
    constexpr UINT SC_TRANSFORM_FLIP_H = 'H';
    constexpr UINT SC_TRANSFORM_FLIP_V = 'V';
    constexpr UINT SC_THUMBNAIL_WRAP_TOGGLE = 'B'; // toggle thumbnail strip wheel wrap-around
    constexpr UINT SC_THUMBNAIL_EFFECTS_TOGGLE = 'U'; // U — toggle thumbnail strip visual effects on/off

    // Y (no modifier)  —  Viewport lock: carry zoom + pan across image changes.
    // Rotation and flips are deliberately NOT locked, because EXIF
    // auto-orientation rewrites them per image (ApplyOrientationToViewport).
    constexpr UINT SC_VIEWPORT_LOCK = 'Y';

    // Q (no modifier)  —  Toggle between current and previous folder in history
    constexpr UINT SC_TOGGLE_LAST_DIR = 'Q';

    // E (no modifier)  —  Toggle between current and previously viewed image
    //                     (Alt+E snaps window to top-right quarter)
    constexpr UINT SC_TOGGLE_LAST_IMAGE = 'E';

    // Alt+Z  —  snap window to bottom-left quarter
    // (bottom-right quarter is Alt+C — same key as SC_COPY_TO_CLIPBOARD)
    constexpr UINT SC_SNAP_QUARTER_BOTTOM_LEFT = 'Z';

    // Alt+X  —  restore window to default size / position
    constexpr UINT SC_WINDOW_RESET_DEFAULTS = 'X';


    // -------------------------------------------------------------------------
    // Color Effects
    // -------------------------------------------------------------------------
    namespace ImageEffects {
        constexpr UINT SC_EFFECT_APPLY_TOGGLE = VK_OEM_3; // grave key `

        // ── The six named effects — ALL require ctrl (no alt, no shift) ───────
        // They moved off the plain navigation-cluster keys so Home/End/PageUp/
        // PageDown/Insert/Delete are free for image and folder navigation.
        // The plain-key meanings live in the Navigation section above.
        constexpr UINT SC_COLOR_GRAYSCALE_OR_NAVIGATE_FAVS_NEXT = VK_DELETE; // Ctrl+Delete // Delete   — next favorite
        constexpr UINT SC_COLOR_INVERT_OR_NAVIGATE_FAVS_PREV = VK_INSERT; // Ctrl+Insert , // Insert   — previous favorite
        constexpr UINT SC_COLOR_SEPIA_OR_SC_NAV_FIRST_IMAGE = VK_HOME; // Ctrl+Home , // Home — jump to first image in folder
        constexpr UINT SC_COLOR_SOLARIZE_OR_SC_NAV_LAST_IMAGE = VK_END; // Ctrl+End , // End  — jump to last image in folder
        constexpr UINT SC_COLOR_OUTLINE_OR_NAV_PREV_HISTORY_FOLDER = VK_PRIOR; // Ctrl+PageUp , // PageUp   — previous non-favorite
        constexpr UINT SC_COLOR_THRESHOLD_OR_NAV_NEXT_HISTORY_FOLDER = VK_NEXT; // Ctrl+PageDown , // PageDown — next non-favorite
        // ── The four continuous adjustments — ALL require ctrl (no alt, no shift)
        // Same rule as the six named effects above: every image-manipulation key
        // in this namespace is Ctrl+<key>. Gamma's two keys keep their unrelated
        // Shift= window-resize meaning, which is not an effect.
        constexpr UINT SC_COLOR_GAMMA_UP = VK_OEM_PLUS; // Ctrl++   (Shift+ = window larger)
        constexpr UINT SC_COLOR_GAMMA_DOWN = VK_OEM_MINUS; // Ctrl+-   (Shift+- = window smaller)
        constexpr UINT SC_COLOR_BRIGHTNESS_UP = VK_OEM_5; // Ctrl+\  backslash
        constexpr UINT SC_COLOR_BRIGHTNESS_DOWN = VK_OEM_7; // Ctrl+'  apostrophe
        constexpr UINT SC_COLOR_CONTRAST_UP = VK_OEM_2; // Ctrl+/  forward slash
        constexpr UINT SC_COLOR_CONTRAST_DOWN = VK_OEM_PERIOD; // Ctrl+.
        constexpr UINT SC_COLOR_SAT_DOWN = VK_OEM_4; // Ctrl+[
        constexpr UINT SC_COLOR_SAT_UP = VK_OEM_6; // Ctrl+]
        constexpr UINT SC_COLOR_RESET_ALL_EFFECTS = VK_NUMPAD0;
        constexpr UINT SC_COLOR_SAVE_TO_DISK = 'S'; // requires ctrl
    }

    // -------------------------------------------------------------------------
    // RUNTIME THEME FACTOR  (Ctrl+Alt + Numpad, no Shift — Shift+Numpad0 = VK_INSERT = Invert)
    // -------------------------------------------------------------------------
    constexpr UINT SC_THEME_FACTOR_UP = VK_ADD; // Ctrl+Alt+Numpad+  — step darker→lighter
    constexpr UINT SC_THEME_FACTOR_DOWN = VK_SUBTRACT; // Ctrl+Alt+Numpad-  — step lighter→darker
    constexpr UINT SC_THEME_FACTOR_RESET = VK_NUMPAD0; // Ctrl+Alt+Numpad0  — restore default THEME_FACTOR

    // -------------------------------------------------------------------------
    // WINDOW CHROME TOGGLES  (Ctrl+Shift + Numpad)
    // -------------------------------------------------------------------------
    constexpr UINT SC_CORNER_PREFERENCE_TOGGLE = VK_MULTIPLY; // Numpad*  — round ↔ square
    constexpr UINT SC_BACKDROP_TYPE_CYCLE = VK_DIVIDE; // Numpad/  — cycle backdrop types

    // -------------------------------------------------------------------------
    // Sort order  (Ctrl+Alt+Shift + digit key)
    // -------------------------------------------------------------------------
    // Ctrl+Alt+Shift+0  —  Sort by Name (natural / Explorer order)
    // Ctrl+Alt+Shift+9  —  Sort by Date Modified
    // Ctrl+Alt+Shift+8  —  Sort by File Size
    // Ctrl+Alt+Shift+7  —  Sort by Extension (type)
    // Ctrl+Alt+Shift+6  —  Sort by Physical Disk Order (HDD performance mode)
    constexpr UINT SC_SORT_BY_NAME = '0'; // requires ctrl+alt+shift
    constexpr UINT SC_SORT_BY_DATE = '9'; // requires ctrl+alt+shift
    constexpr UINT SC_SORT_BY_SIZE = '8'; // requires ctrl+alt+shift
    constexpr UINT SC_SORT_BY_TYPE = '7'; // requires ctrl+alt+shift
    constexpr UINT SC_SORT_BY_DISK = '6'; // requires ctrl+alt+shift

    // -------------------------------------------------------------------------
    // Image Info / EXIF panel
    // -------------------------------------------------------------------------
    constexpr UINT SC_SHOW_INFO = 'M'; // M (no modifier) — image info / EXIF window

    // -------------------------------------------------------------------------
    // Multi-monitor
    // -------------------------------------------------------------------------
    // Ctrl+M — move the window to the next monitor, wrapping at the last.
    // Shares 'M' with the plain-key info panel, split by modifier: both are
    // "M for Monitor / Metadata" and neither had a Ctrl form.
    constexpr UINT SC_MOVE_TO_NEXT_MONITOR = 'M'; // requires ctrl

    // -------------------------------------------------------------------------
    // Jump to image by number
    // -------------------------------------------------------------------------
    constexpr UINT SC_NAV_JUMP_TO_IMAGE = 'J'; // J (no modifier) — open jump dialog
    constexpr UINT SC_NAV_JUMP_TO_IMAGE_ALT = 'G'; // Ctrl+G          — open jump dialog

    // -------------------------------------------------------------------------
    // Find image by name  (shares 'F' with SC_PANEL_FULLSCREEN_F)
    // -------------------------------------------------------------------------
    constexpr UINT SC_NAV_FIND = 'F'; // Ctrl+F — open find-by-name dialog

    // -------------------------------------------------------------------------
    // Statistics window
    // -------------------------------------------------------------------------
    constexpr UINT SC_TOGGLE_STATS = 'K'; // K (no modifier) — show statistics window

    // -------------------------------------------------------------------------
    // Clipboard
    // -------------------------------------------------------------------------
    constexpr UINT SC_COPY_TO_CLIPBOARD = 'C'; // Ctrl+C — copy current image to clipboard

    // -------------------------------------------------------------------------
    // HISTORY WINDOW
    // -------------------------------------------------------------------------
    constexpr UINT HISTORY_FAVORITES_TOGGLE_KEY = VK_SPACE;
    // Ctrl+Delete — delete the hovered row. Moved off plain Delete so the
    // navigation cluster (Home/End/PageUp/PageDown/Insert/Delete) passes
    // straight through the panel to the main app.
    constexpr UINT HISTORY_DELETE_HOVERED_ROW = VK_DELETE; // requires ctrl
    constexpr UINT HISTORY_CLEAR_ALL_HISTORY_BUT_NOT_FAVORITES = VK_DELETE; //ctrl+shilf+delete only when historyWnd is open
    constexpr UINT HISTORY_CLEAR_ALL_FAVORITES_BUT_NOT_HISTORY = VK_DELETE; //ctrl+alt+shift+delete only when historyWnd is open
    // Shift+Enter — spawn a DirWnd for the selected history folder (up to DIR_WND_MAX_INSTANCES)
    // Plain Enter still opens the folder in the main viewer as before.
    constexpr UINT HISTORY_OPEN_IN_DIR_WND = VK_RETURN; // requires Shift

    // -------------------------------------------------------------------------
    // Transform
    // -------------------------------------------------------------------------
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
