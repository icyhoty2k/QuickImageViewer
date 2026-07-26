#pragma once

#include "ConstantsTheme.h"

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
    // F5 inside the History panel — re-reads both .txt files and re-scans every
    // folder. Says how many, because the scan is asynchronous and the row markers
    // land a moment later; without it the key looks like it did nothing.
    constexpr const wchar_t *HISTORY_REFRESHED_MSG = L"History refreshed — rescanning ";
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
    // Indexed by TransitionType — single source for the tray submenu, the context
    // menu submenu and the overlay message. Order MUST match the enum in
    // SlideshowTransitions.h; size MUST match Constants::Slideshow::TRANSITION_COUNT.
    constexpr const wchar_t *TRANSITION_NAMES[] = {
        L"Cut", L"Fade", L"Dissolve", L"Ripple",
        L"Slide Left", L"Zoom Out", L"Slide Up", L"Zoom In",
        L"Slide Right", L"Slide Down", L"Soft Zoom", L"Spin",
        L"Spin Zoom", L"Drift Left", L"Drift Up", L"Flicker",
        L"Bounce", L"Swing", L"Slam", L"Iris", L"Slide Diagonal"
    };
    constexpr const wchar_t *TRANSITION_PREFIX = L"Transition: "; // append a NAMES entry
    // Indexed by Constants::Slideshow::TransitionSource::NONE..LIST.
    constexpr const wchar_t *TRANSITION_SOURCE_NAMES[] = {
        L"None (use selected)", L"All", L"List (ticked only)"
    };
    // Indexed by Constants::Slideshow::TransitionOrder::SEQUENTIAL..RANDOM.
    constexpr const wchar_t *TRANSITION_ORDER_NAMES[] = { L"Sequential", L"Random" };
    constexpr const wchar_t *TRANSITION_SOURCE_PREFIX = L"Transitions: ";
    constexpr const wchar_t *TRANSITION_ORDER_PREFIX  = L"Transition Order: ";
    constexpr const wchar_t *TRANSITION_LIST_EMPTY    = L"⚠ Transition list is empty";
    // Runtime-appendable forms of the STR_STATE_* macros, for messages whose
    // subject is only known at run time.
    constexpr const wchar_t *STATE_ON_SUFFIX  = STR_STATE_ON;
    constexpr const wchar_t *STATE_OFF_SUFFIX = STR_STATE_OFF;
    constexpr const wchar_t *SLIDESHOW_INTERVAL_PREFIX = L"Interval: "; // append "<n> ms"

    // Ctrl+T — always on top
    constexpr const wchar_t *ALWAYS_ON_TOP_ON = L"Always on Top" STR_STATE_ON;
    constexpr const wchar_t *ALWAYS_ON_TOP_OFF = L"Always on Top" STR_STATE_OFF;
    // Shown when the lock is turned ON so the operator knows the window went
    // deaf on purpose, and that the tray is the way back.
    constexpr const wchar_t *KIOSK_LOCK_ON  = L"Kiosk Lock" STR_STATE_ON L" — unlock from the tray icon";
    constexpr const wchar_t *KIOSK_LOCK_OFF = L"Kiosk Lock" STR_STATE_OFF;
    constexpr const wchar_t *KEEP_DISPLAY_AWAKE_ON  = L"Keep Display Awake" STR_STATE_ON;
    constexpr const wchar_t *KEEP_DISPLAY_AWAKE_OFF = L"Keep Display Awake" STR_STATE_OFF;

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

    // Desktop wallpaper — NAMES is indexed by Constants::Wallpaper::FILL..SPAN and
    // is the single source for both the submenu labels and the overlay message.
    constexpr const wchar_t *WALLPAPER_NAMES[] = {
        L"Fill", L"Fit", L"Stretch", L"Tile", L"Center", L"Span"
    };
    constexpr const wchar_t *WALLPAPER_SET = L"Wallpaper: "; // prefix — append the style name
    constexpr const wchar_t *WALLPAPER_FAILED = L"⚠ Wallpaper could not be applied";
    // Folder walking — used by ALL three walkers (horizontal wheel, PageUp/Down,
    // Insert/Delete) so the centre message never depends on how you moved.
    // Format is "<prefix><n>/<total> <folder name>", where <n> is the row number
    // the History panel shows for that folder, so overlay and panel always agree.
    // The prefix is chosen from the row itself: starred rows get ★, the rest 📁.
    constexpr const wchar_t *WALK_HISTORY_FOLDER = Constants::ThemeIcons::ICON_FOLDER;
    constexpr const wchar_t *WALK_FAVORITE_FOLDER = Constants::ThemeIcons::ICON_FAVORITES_MARK;
    // Shown when a walk stepped over more than one dead folder to reach its
    // destination — append the count. A single skip names the folder instead.
    // Text only: ThemeIcons entries are constexpr pointers, not macros, so
    // ICON_WARNING cannot be concatenated into the literal here — the caller
    // prepends it at runtime.
    constexpr const wchar_t *WALK_SKIPPED = L" skipped ";
    // History panel footer total, and its hover popup.
    // Footer reads "<size>/<files>" plus "/<n>" only when folders were excluded.
    // The popup spells all three out, then names what was left out and why it
    // could be: an alias whose target is already counted elsewhere in the list.
    // Shown centred in the History panel while the background folder sweep is
    // still running. The panel stays fully usable — the scan does not block it,
    // and closing the panel does not stop the scan.
    constexpr const wchar_t *HISTORY_SCANNING = L"Loading ...";

    // The word for a folder, used everywhere in this popup so it can be changed
    // in one place (dirs / folders / directories).
    constexpr const wchar_t *WORD_DIRS = L"dirs";

    constexpr const wchar_t *TOTAL_HEADER = L"TOTAL:";
    constexpr const wchar_t *TOTAL_SIZE_LABEL = L"Size: ";
    constexpr const wchar_t *TOTAL_FILES_LABEL = L"Files: ";
    constexpr const wchar_t *TOTAL_DIRS_LABEL = L"Dirs: ";
    constexpr const wchar_t *TOTAL_SEPARATOR = L"===========";
    constexpr const wchar_t *TOTAL_EXCLUDED_SUFFIX = L" excluded:"; // "<n> dirs excluded:"
    constexpr const wchar_t *EXCLUDED_BULLET = L"* ";               // before each number
    // Group headings under the excluded count. Each names WHY those folders
    // contribute nothing, then lists them numbered from 1.
    constexpr const wchar_t *EXCLUDED_DUPLICATES = L"already counted (symlinks of a listed folder):";
    constexpr const wchar_t *EXCLUDED_MISSING = L"missing ";  // + WORD_DIRS + ":"
    constexpr const wchar_t *EXCLUDED_EMPTY = L"empty ";      // + WORD_DIRS + ":"

    // History panel — row badge labels. One line each in the hover popup that
    // appears when a row carries more than one badge and they have to share a slot.
    constexpr const wchar_t *BADGE_MISSING = L"Folder not found";
    constexpr const wchar_t *BADGE_EMPTY = L"No images in folder";
    constexpr const wchar_t *BADGE_FAVORITE = L"Favorite";

    // History panel — symlink glyph hover popup. Line 1 is the kind of link,
    // line 2 is the resolved destination (filled in at runtime).
    constexpr const wchar_t *LINK_KIND_JUNCTION = L"Directory junction  (mklink /J)";
    constexpr const wchar_t *LINK_KIND_SYMLINK = L"Directory symlink  (mklink /D)";
    // Path resolves elsewhere but no component is a reparse point — the drive
    // letter itself is redirected (subst, or a mapped network drive).
    constexpr const wchar_t *LINK_KIND_MAPPED = L"Mapped path  (subst / network drive)";
    constexpr const wchar_t *LINK_TARGET_UNKNOWN = L"target could not be resolved";

    constexpr const wchar_t *WALK_NO_HISTORY_FOLDERS = L"No other history folders";
    constexpr const wchar_t *WALK_NO_FAVORITE_FOLDERS = L"No favorite folders";
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

    // Overlay slot names, indexed by OverlayManager::Slot. Single source for
    // every centre message that names a slot, whatever triggered the change.
    constexpr const wchar_t *OVERLAY_SLOT_NAMES[] = {
        L"Top Left", L"Top Center", L"Top Right",
        L"Mid Left", L"Mid Center", L"Mid Right",
        L"Bot Left", L"Bot Center", L"Bot Right"
    };
    constexpr const wchar_t *OVERLAY_PREFIX        = L"Overlay ";
    constexpr const wchar_t *OVERLAY_FONT_PREFIX   = L"Overlay Font: "; // append the family name
    constexpr const wchar_t *OVERLAY_STATE_COMPACT = STR_SEPARATOR L"Compact";
    constexpr const wchar_t *OVERLAY_STATE_FULL    = STR_SEPARATOR L"Full";

    // BOT_LEFT readouts — independent toggles in the Overlays menu
    constexpr const wchar_t *OVERLAY_EFFECTS_LIST_ON  = L"Effects Overlay" STR_STATE_ON;
    constexpr const wchar_t *OVERLAY_EFFECTS_LIST_OFF = L"Effects Overlay" STR_STATE_OFF;
    constexpr const wchar_t *OVERLAY_DIR_NAME_ON      = L"Folder Name" STR_STATE_ON;
    constexpr const wchar_t *OVERLAY_DIR_NAME_OFF     = L"Folder Name" STR_STATE_OFF;

    // Settings-menu-only toggles (no keyboard shortcut counterpart)
    constexpr const wchar_t *KEEP_IN_BG_ON        = L"Keep in Background" STR_STATE_ON;
    constexpr const wchar_t *KEEP_IN_BG_OFF       = L"Keep in Background" STR_STATE_OFF;
    constexpr const wchar_t *RUN_ON_STARTUP_ON    = L"Run on Startup" STR_STATE_ON;
    constexpr const wchar_t *RUN_ON_STARTUP_OFF   = L"Run on Startup" STR_STATE_OFF;
    constexpr const wchar_t *HISTORY_FULL_ON      = L"History: Full List" STR_STATE_ON;
    constexpr const wchar_t *HISTORY_FULL_OFF     = L"History: Full List" STR_STATE_OFF;
    constexpr const wchar_t *OPEN_THUMB_START_ON  = L"Thumbnail Strip on Start" STR_STATE_ON;
    constexpr const wchar_t *OPEN_THUMB_START_OFF = L"Thumbnail Strip on Start" STR_STATE_OFF;
    constexpr const wchar_t *SWAP_MOUSE_ON        = L"Swap Mouse Buttons" STR_STATE_ON;
    constexpr const wchar_t *SWAP_MOUSE_OFF       = L"Swap Mouse Buttons" STR_STATE_OFF;
    constexpr const wchar_t *WHEEL_INVERT_ON      = L"Invert Scroll" STR_STATE_ON;
    constexpr const wchar_t *WHEEL_INVERT_OFF     = L"Invert Scroll" STR_STATE_OFF;
    constexpr const wchar_t *WHEEL_INVERT_H_ON    = L"Invert Horizontal Scroll" STR_STATE_ON;
    constexpr const wchar_t *WHEEL_INVERT_H_OFF   = L"Invert Horizontal Scroll" STR_STATE_OFF;
    constexpr const wchar_t *START_FULLSCREEN_ON  = L"Start in Fullscreen" STR_STATE_ON;
    constexpr const wchar_t *START_FULLSCREEN_OFF = L"Start in Fullscreen" STR_STATE_OFF;
    constexpr const wchar_t *CTRL_C_COPY_ON       = L"Ctrl+C Copy" STR_STATE_ON;
    constexpr const wchar_t *CTRL_C_COPY_OFF      = L"Ctrl+C Copy" STR_STATE_OFF;
    constexpr const wchar_t *THUMB_COPY_OP_ON     = L"Thumbnail Copy" STR_STATE_ON;
    constexpr const wchar_t *THUMB_COPY_OP_OFF    = L"Thumbnail Copy" STR_STATE_OFF;
    constexpr const wchar_t *THUMB_MOVE_OP_ON     = L"Thumbnail Move" STR_STATE_ON;
    constexpr const wchar_t *THUMB_MOVE_OP_OFF    = L"Thumbnail Move" STR_STATE_OFF;
    constexpr const wchar_t *THUMB_DELETE_OP_ON   = L"Thumbnail Delete" STR_STATE_ON;
    constexpr const wchar_t *THUMB_DELETE_OP_OFF  = L"Thumbnail Delete" STR_STATE_OFF;
    constexpr const wchar_t *THUMB_PASTE_OP_ON    = L"Thumbnail Paste" STR_STATE_ON;
    constexpr const wchar_t *THUMB_PASTE_OP_OFF   = L"Thumbnail Paste" STR_STATE_OFF;

    // View modes (keys 1-5)
    constexpr const wchar_t *VIEW_MODE_PREFIX = L"View: ";
    constexpr const wchar_t *VIEW_MODE_NAMES[] = {
        L"Fit to View",
        L"Fit to Width",
        L"Fit to Height",
        L"Fit to Window",
        L"Original Size"
    };

    // Zoom popup
    constexpr const wchar_t *ZOOM_TO_PREFIX = L"Zoom: ";
    // %s pairs — the bounds arrive pre-formatted from
    // Converters::FormatPercentCompact so their precision follows the value.
    // A fixed "%.1f" here printed ZOOM_MIN = 0.01 as "0.0".
    constexpr const wchar_t *ZOOM_TO_INPUT_HINT_FMT = L"Enter zoom in percent (%s%% – %s%%, 0 = reset)";

    // ZoomWnd — hint line below the input box
    // The out-of-range / invalid messages are built at draw time straight from
    // Constants::ZoomPanel::ZOOM_MIN and ZOOM_MAX — both are already percents.
    constexpr const wchar_t *ZOOM_ENTER_HINT = L"Enter = apply zoom  •  Esc = cancel";
    constexpr const wchar_t *ZOOM_OUT_OF_RANGE_FMT = L"Out of range — type a value between %s%% and %s%%";
    constexpr const wchar_t *ZOOM_RESET_MESSAGE = L"Zoom restored to default";
    // Shown centre-screen when a zoom keypress/wheel tick is capped by
    // ZoomPanel::ZOOM_MIN / ZOOM_MAX — otherwise the input looks ignored.
    constexpr const wchar_t *ZOOM_MIN_REACHED = L"Minimum zoom reached";
    constexpr const wchar_t *ZOOM_MAX_REACHED = L"Maximum zoom reached";
}

namespace Constants::Strings {
    // ── BOT_LEFT slot: active color-effect labels ───────────────────────────
    // Used in OverlayManager::UpdateEffects() via appendLine()

    // Named colour effects. These strings are also the KEYS in
    // app.activeEffectsList, so the renderer matches on them — changing a value
    // changes what RendererD2D::ChainEffectByName() compares against.
    //
    // Shown as "Desaturate", not "Grayscale", deliberately: effects stack, so a
    // later Sepia tints this one's output and the picture ends up brown while
    // the label is still listed. "Grayscale" reads as a promise that the final
    // image IS gray and looks broken the moment anything follows it;
    // "Desaturate" names the operation being performed, which is what the entry
    // actually is. The identifier stays EFFECT_GRAYSCALE to match
    // app.effectGrayscale and Command::ToggleGrayscale.
    constexpr const wchar_t *EFFECT_GRAYSCALE = L"Desaturate";
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
