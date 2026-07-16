#pragma once

// ConstantsStrings.h
// Central repository for all user-visible text used in QIV overlays.
// Keep strings here so they have one place to change for localization.

namespace Constants::Messages {
    // ── MID_CENTER: state-change notifications ──────────────────────────────
    // These are posted via PostCenterMessage and auto-hide after the timer.
    //Inform user has jumped to firs last image in current folder
    constexpr const wchar_t *TOGGLE_FIRST_IMAGE_IN_FOLDER = L"First image: ";
    constexpr const wchar_t *TOGGLE_LAST_IMAGE_IN_FOLDER = L"Last image: ";
    constexpr const wchar_t *GO_TO_LAST_IMAGE_BEFORE_TOGGLE = L"Previous image: ";
    constexpr const wchar_t *CACHE_WINDOW_VISIBLE_MSG = L"Cache Window ON ";
    constexpr const wchar_t *CACHE_WINDOW_HIDDEN_MSG = L"Cache Window OFF ";
    constexpr const wchar_t *CACHE_WINDOW_CLEAR_CACHE_MSG = L"Cache cleared ! ";


    // Overlay master toggle (N / I / Ctrl+0)
    constexpr const wchar_t *INFO_PANELS_ON = L"Info Panels: ON";
    constexpr const wchar_t *INFO_PANELS_OFF = L"Info Panels: OFF";

    // Overlay background toggle (P)
    constexpr const wchar_t *OVERLAY_BG_ON = L"Overlay BG: ON";
    constexpr const wchar_t *OVERLAY_BG_OFF = L"Overlay BG: OFF";

    // Overlay layout cycle (O)
    constexpr const wchar_t *LAYOUT_GRID = L"Layout: Grid";
    constexpr const wchar_t *LAYOUT_STACKED = L"Layout: Stacked";
    constexpr const wchar_t *LAYOUT_SUMMARY = L"Layout: Summary";

    // Reset / effects
    constexpr const wchar_t *RESET_TO_DEFAULTS = L"Reset to Defaults";
    constexpr const wchar_t *ALL_EFFECTS_RESET = L"All Effects Reset";
    // F5 Refresh/Reload current dir
    constexpr const wchar_t *RELOAD_CURRENT_DIR_MSG = L"Refreshed";

    // Q — toggle last/current dir
    constexpr const wchar_t *TOGGLE_DIR_NO_PREV = L"No previous folder";
    constexpr const wchar_t *TOGGLE_DIR_CHANGED = L"→ "; // prefix — append folder name
    constexpr const wchar_t *TOGGLE_DIR_MISSING = L"⚠ Previous folder no longer exists";

    // E — toggle last/current image
    constexpr const wchar_t *TOGGLE_IMAGE_NO_PREV = L"No previous image";
    constexpr const wchar_t *TOGGLE_IMAGE_CHANGED = L"→ "; // prefix — append filename
    constexpr const wchar_t *TOGGLE_IMAGE_MISSING = L"⚠ Previous image no longer exists";

    // Runtime theme factor  (Ctrl+Alt+Shift+Numpad+/-/0)
    constexpr const wchar_t *THEME_FACTOR_PREFIX = L"Theme: ";
    constexpr const wchar_t *THEME_FACTOR_RESET_MSG = L"Theme: Reset";

    // Window chrome toggles  (Ctrl+Shift+Numpad*)
    constexpr const wchar_t *CORNER_ROUND = L"Corners: Round";
    constexpr const wchar_t *CORNER_SQUARE = L"Corners: Square";

    // Backdrop cycle  (Ctrl+Shift+Numpad/)
    constexpr const wchar_t *BACKDROP_NONE = L"Backdrop: None";
    constexpr const wchar_t *BACKDROP_MICA = L"Backdrop: Mica";
    constexpr const wchar_t *BACKDROP_ACRYLIC = L"Backdrop: Acrylic";
    constexpr const wchar_t *BACKDROP_MICA_ALT = L"Backdrop: MicaAlt";

    // Ctrl+F1 / Space / R / S — Slideshow
    constexpr const wchar_t *SLIDESHOW_PLAYING = L"▶ Slideshow"; // prefix; interval/loop/shuffle appended dynamically
    constexpr const wchar_t *SLIDESHOW_PAUSED = L"⏸ Slideshow Paused";
    constexpr const wchar_t *SLIDESHOW_STOPPED = L"■ Slideshow Stopped";
    constexpr const wchar_t *SLIDESHOW_LOOP_ON = L"Loop: ON";
    constexpr const wchar_t *SLIDESHOW_LOOP_OFF = L"Loop: OFF";
    constexpr const wchar_t *SLIDESHOW_SHUFFLE_ON = L"Shuffle: ON";
    constexpr const wchar_t *SLIDESHOW_SHUFFLE_OFF = L"Shuffle: OFF";
    constexpr const wchar_t *TRANSITION_CUT = L"Transition: Cut";
    constexpr const wchar_t *TRANSITION_FADE = L"Transition: Fade";
    constexpr const wchar_t *TRANSITION_DISSOLVE = L"Transition: Dissolve";
    constexpr const wchar_t *TRANSITION_RIPPLE = L"Transition: Ripple";
    constexpr const wchar_t *TRANSITION_PUSH = L"Transition: Push";
    constexpr const wchar_t *TRANSITION_ZOOM = L"Transition: Zoom";

    // Ctrl+T — always on top
    constexpr const wchar_t *ALWAYS_ON_TOP_ON = L"Always on Top: ON";
    constexpr const wchar_t *ALWAYS_ON_TOP_OFF = L"Always on Top: OFF";

    // Alt+W/A/S/D — keyboard snap to screen half
    constexpr const wchar_t *SNAP_LEFT = L"Snap: Left Half";
    constexpr const wchar_t *SNAP_RIGHT = L"Snap: Right Half";
    constexpr const wchar_t *SNAP_TOP = L"Snap: Top Half";
    constexpr const wchar_t *SNAP_BOTTOM = L"Snap: Bottom Half";

    // Alt+Q/E/Z/C — keyboard snap to screen quarter
    constexpr const wchar_t *SNAP_TOP_LEFT = L"Snap: Top-Left Quarter";
    constexpr const wchar_t *SNAP_TOP_RIGHT = L"Snap: Top-Right Quarter";
    constexpr const wchar_t *SNAP_BOTTOM_LEFT = L"Snap: Bottom-Left Quarter";
    constexpr const wchar_t *SNAP_BOTTOM_RIGHT = L"Snap: Bottom-Right Quarter";

    // Sort order  (Ctrl+Alt+Shift+0/6/7/8/9)  — press once: ascending, press again: descending
    constexpr const wchar_t *SORT_BY_NAME = L"Sort: Name (A→Z)";
    constexpr const wchar_t *SORT_BY_NAME_REV = L"Sort: Name (Z→A)";
    constexpr const wchar_t *SORT_BY_DATE = L"Sort: Date (Newest)";
    constexpr const wchar_t *SORT_BY_DATE_REV = L"Sort: Date (Oldest)";
    constexpr const wchar_t *SORT_BY_SIZE = L"Sort: Size (Largest)";
    constexpr const wchar_t *SORT_BY_SIZE_REV = L"Sort: Size (Smallest)";
    constexpr const wchar_t *SORT_BY_TYPE = L"Sort: Extension (A→Z)";
    constexpr const wchar_t *SORT_BY_TYPE_REV = L"Sort: Extension (Z→A)";
    constexpr const wchar_t *SORT_BY_DISK = L"Sort: Disk Order";

    // Spawned DirWnd messages
    constexpr const wchar_t *SPAWN_DIR_TOP = L"DirWnd Spawned: Top";
    constexpr const wchar_t *SPAWN_DIR_LEFT = L"DirWnd Spawned: Left";
    constexpr const wchar_t *SPAWN_DIR_RIGHT = L"DirWnd Spawned: Right";
    constexpr const wchar_t *SPAWN_DIR_BOTTOM = L"DirWnd Spawned: Bottom";
    constexpr const wchar_t *SPAWN_DIR_CLOSED = L"DirWnd Closed";
    constexpr const wchar_t *SPAWN_DIR_NO_SPACE = L"No free positions for DirWnd";
    constexpr const wchar_t *COPIED_TO_CLIPBOARD = L"Copied to Clipboard";
    constexpr const wchar_t *HISTORY_NAV_FOLDER = L"↔ "; // prefix — append folder name
    constexpr const wchar_t *FOLDER_DEAD_MISSING = L"⚠ Folder not found";
    constexpr const wchar_t *FOLDER_DEAD_EMPTY = L"⚠ No images in folder";

    // Thumbnail strip wrap-around
    constexpr const wchar_t *THUMB_STRIP_WRAP_TO_START = L"↩ Start";
    constexpr const wchar_t *THUMB_STRIP_WRAP_TO_END = L"↪ End";
    constexpr const wchar_t *THUMB_STRIP_WRAP_ON = L"Thumbnail Strip Wrap: ON";
    constexpr const wchar_t *THUMB_STRIP_WRAP_OFF = L"Thumbnail Strip Wrap: OFF";
}

namespace Constants::Strings {
    // ── BOT_LEFT slot: active color-effect labels ───────────────────────────
    // Used in OverlayManager::UpdateEffects() via appendLine()

    // Named colour effects (also fed to ToggleEffectChronological)
    constexpr const wchar_t *EFFECT_GRAYSCALE = L"Grayscale";
    constexpr const wchar_t *EFFECT_INVERT = L"Invert";
    constexpr const wchar_t *EFFECT_SEPIA = L"Sepia";
    constexpr const wchar_t *EFFECT_SOLARIZE = L"Solarize";
    constexpr const wchar_t *EFFECT_OUTLINE = L"Outline";
    constexpr const wchar_t *EFFECT_THRESHOLD = L"Threshold";

    // Continuous-parameter labels (prefix before the numeric value)
    constexpr const wchar_t *LABEL_BRIGHTNESS = L"Brightness: ";
    constexpr const wchar_t *LABEL_CONTRAST = L"Contrast: ";
    constexpr const wchar_t *LABEL_SATURATION = L"Saturation: ";
    constexpr const wchar_t *LABEL_GAMMA = L"Gamma: ";

    // Brightness sign prefix (positive values)
    constexpr const wchar_t *SIGN_POSITIVE = L"+";
}
