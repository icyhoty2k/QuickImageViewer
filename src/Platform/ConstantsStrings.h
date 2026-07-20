#pragma once

// ConstantsStrings.h
// Central repository for all user-visible text used in QIV overlays.
// Keep strings here so they have one place to change for localization.

// Internal compile-time string fragments
// Do not use directly outside this header.
#define STR_THUMBNAIL_STRIP         L"Thumbnail strip"
#define STR_CACHE_WINDOW            L"VRAM strip"
#define STR_SEPARATOR               L": "
#define STR_STATE_ON STR_SEPARATOR  L"ON"
#define STR_STATE_OFF STR_SEPARATOR L"OFF"


namespace Constants::Messages {
    // ── MID_CENTER: state-change notifications ──────────────────────────────
    // These are posted via PostCenterMessage and auto-hide after the timer.
    //Inform user has jumped to firs last image in current folder
    constexpr const wchar_t *TOGGLE_FIRST_IMAGE_IN_FOLDER = L"First image: ";
    constexpr const wchar_t *TOGGLE_LAST_IMAGE_IN_FOLDER = L"Last image: ";
    constexpr const wchar_t *GO_TO_LAST_IMAGE_BEFORE_TOGGLE = L"Previous image: ";
    constexpr const wchar_t *CACHE_WINDOW_VISIBLE_MSG = STR_CACHE_WINDOW  STR_STATE_ON;
    constexpr const wchar_t *CACHE_WINDOW_HIDDEN_MSG = STR_CACHE_WINDOW  STR_STATE_OFF;
    constexpr const wchar_t *CACHE_WINDOW_CLEAR_CACHE_MSG = L"Cache cleared!";
    constexpr const wchar_t *DIR_WINDOW_VISIBLE_MSG = STR_THUMBNAIL_STRIP  STR_STATE_ON;
    constexpr const wchar_t *DIR_WINDOW_HIDDEN_MSG = STR_THUMBNAIL_STRIP  STR_STATE_OFF;


    // Overlay master toggle (N / I / Ctrl+0)
    constexpr const wchar_t *INFO_PANELS_ON = L"Info Panels" STR_STATE_ON;
    constexpr const wchar_t *INFO_PANELS_OFF = L"Info Panels" STR_STATE_OFF;

    // Overlay background toggle (P)
    constexpr const wchar_t *OVERLAY_BG_ON = L"Overlay BG" STR_STATE_ON;
    constexpr const wchar_t *OVERLAY_BG_OFF = L"Overlay BG" STR_STATE_OFF;

    // Overlay layout cycle (O)
    constexpr const wchar_t *LAYOUT_GRID = L"Layout: Grid";
    constexpr const wchar_t *LAYOUT_STACKED = L"Layout: Stacked";
    constexpr const wchar_t *LAYOUT_SUMMARY = L"Layout: Summary";

    // Reset / effects
    constexpr const wchar_t *RESET_TO_DEFAULTS = L"Reset to Defaults";
    constexpr const wchar_t *ALL_EFFECTS_RESET = L"All Effects Reset";
    // F5 Refresh/Reload current dir
    constexpr const wchar_t *RELOAD_CURRENT_DIR_MSG = L"Refreshed";
    // Empty-dir placeholder shown in DirWnd / SpawnedDirWnd when the folder has no images
    constexpr const wchar_t *EMPTY_DIR_NO_IMAGES = L"No Images:";
    // Placeholder shown when the directory itself has been deleted
    constexpr const wchar_t *EMPTY_DIR_MISSING = L"⚠  Directory Missing";
    // Placeholder shown in CacheWnd when the VRAM thumbnail cache is empty
    constexpr const wchar_t *EMPTY_CACHE = L"Thumbnail Cache Empty";

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
    constexpr const wchar_t *SLIDESHOW_LOOP_ON = L"Loop" STR_STATE_ON;
    constexpr const wchar_t *SLIDESHOW_LOOP_OFF = L"Loop" STR_STATE_OFF;
    constexpr const wchar_t *SLIDESHOW_SHUFFLE_ON = L"Shuffle" STR_STATE_ON;
    constexpr const wchar_t *SLIDESHOW_SHUFFLE_OFF = L"Shuffle" STR_STATE_OFF;
    constexpr const wchar_t *TRANSITION_CUT = L"Transition: Cut";
    constexpr const wchar_t *TRANSITION_FADE = L"Transition: Fade";
    constexpr const wchar_t *TRANSITION_DISSOLVE = L"Transition: Dissolve";
    constexpr const wchar_t *TRANSITION_RIPPLE = L"Transition: Ripple";
    constexpr const wchar_t *TRANSITION_PUSH = L"Transition: Push";
    constexpr const wchar_t *TRANSITION_ZOOM = L"Transition: Zoom";

    // Ctrl+T — always on top
    constexpr const wchar_t *ALWAYS_ON_TOP_ON = L"Always on Top" STR_STATE_ON;
    constexpr const wchar_t *ALWAYS_ON_TOP_OFF = L"Always on Top" STR_STATE_OFF;

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

    // Ctrl+Space — fill available screen space (work area minus visible panels) / restore
    constexpr const wchar_t *AUTOSIZE_TO_WORK_AREA = L"Fit to Screen";
    constexpr const wchar_t *AUTOSIZE_RESTORE = L"Default Size";

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



    constexpr const wchar_t *SPAWN_DIR_TOP =  STR_THUMBNAIL_STRIP STR_SEPARATOR L"Top";
    constexpr const wchar_t *SPAWN_DIR_LEFT = STR_THUMBNAIL_STRIP STR_SEPARATOR L"Left";
    constexpr const wchar_t *SPAWN_DIR_RIGHT = STR_THUMBNAIL_STRIP STR_SEPARATOR L"Right";
    constexpr const wchar_t *SPAWN_DIR_BOTTOM = STR_THUMBNAIL_STRIP STR_SEPARATOR L"Bottom";
    constexpr const wchar_t *SPAWN_DIR_CLOSED = STR_THUMBNAIL_STRIP STR_SEPARATOR L"Closed";
    constexpr const wchar_t *SPAWN_DIR_NO_SPACE = L"No free positions for " STR_THUMBNAIL_STRIP;
    constexpr const wchar_t *COPIED_TO_CLIPBOARD = L"Copied to Clipboard";
    constexpr const wchar_t *HISTORY_NAV_FOLDER = L"↔ "; // prefix — append folder name
    constexpr const wchar_t *FOLDER_DEAD_MISSING = L"⚠ Folder not found";
    constexpr const wchar_t *FOLDER_DEAD_EMPTY = L"⚠ No images in folder";
    constexpr const wchar_t *FOLDER_DELETED_NOTIFY = L"⚠ Folder deleted";

    // Thumbnail strip wrap-around
    constexpr const wchar_t *THUMB_STRIP_WRAP_TO_START = L"↩ Start";
    constexpr const wchar_t *THUMB_STRIP_WRAP_TO_END = L"↪ End";
    constexpr const wchar_t *THUMB_STRIP_WRAP_ON = STR_THUMBNAIL_STRIP L" Wrap" STR_STATE_ON;
    constexpr const wchar_t *THUMB_STRIP_WRAP_OFF = STR_THUMBNAIL_STRIP L" Wrap" STR_STATE_OFF;

    // Thumbnail strip visual effects
    constexpr const wchar_t *THUMB_EFFECTS_ON  = L"Thumbnail Effects" STR_STATE_ON;
    constexpr const wchar_t *THUMB_EFFECTS_OFF = L"Thumbnail Effects" STR_STATE_OFF;
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
// Prevent helper macros leaking outside this header
#undef STR_STATE_OFF
#undef STR_STATE_ON
#undef STR_SEPARATOR
#undef STR_CACHE_WINDOW
#undef STR_THUMBNAIL_STRIP
