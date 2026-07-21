#pragma once
#include <windows.h>
#include <string>
#include "PromotionPlaylist.h"

// =============================================================================
// DedicatedInstance — identity and configuration of one named dedicated copy.
//
// WHY A NAME: today "dedicated" is a single boolean, so every dedicated copy
// shares one mutex, one window class, one registry namespace and one history
// file — a second copy would be treated as a duplicate and forwarded to the
// first. Giving each copy a NAME turns that binary flavour into an identity,
// and every isolation mechanism derives from it. Unlimited copies can then run
// side by side, one per monitor, without colliding.
//
// The main (unnamed) instance keeps every existing name unchanged, so nothing
// about a normal launch moves.
// =============================================================================

namespace Dedicated {

struct InstanceConfig {
    // --- Identity ---
    std::wstring name;        // "" = main instance. Sanitised for use in names.
    std::wstring description; // free text, shown in the menu and the shortcut

    // --- Content ---
    // Folders live in the two list files beside the exe, NOT here — see
    // DedicatedLists.h. A single folder field used to sit alongside them, which
    // meant two competing sources for the same thing; the lists won because
    // they hold any number of folders.

    // --- Promotions ---
    // promoOrder = WHICH promotion is picked (sequential / weighted by priority).
    // The two (from, to) pairs below are the independent WHEN triggers:
    //   images (0,0)=off  (5,0)=every 5th image   (5,15)=random every 5-15
    //   time   (0,0)=off  (60,0)=every 60s        (30,90)=random every 30-90s
    // Setting both runs them independently; either coming due shows a promotion.
    // How long a promotion stays on screen. Independent of the slide interval:
    // a promotion is a message, not a picture, and usually wants its own dwell
    // time. 0 = use the slide interval.
    int promoShowSeconds = 0;

    int promoOrder      = Constants::Dedicated::PromoOrder::WEIGHTED;
    int promoImagesFrom = Constants::Dedicated::PROMO_IMAGES_EVERY_DEFAULT;
    int promoImagesTo   = Constants::Dedicated::PROMO_IMAGES_UPTO_DEFAULT;
    int promoTimeFrom   = Constants::Dedicated::PROMO_SECONDS_EVERY_DEFAULT;
    int promoTimeTo     = Constants::Dedicated::PROMO_SECONDS_UPTO_DEFAULT;

    // --- Presentation ---
    int  monitorNum      = 0;    // 1-based; 0 = wherever it opens
    bool fullscreen      = true;
    bool slideshow       = true;
    bool loop            = true;
    bool hideMouse       = true;
    int  intervalSeconds = 0;    // 0 = leave the saved interval alone

    // --- Mirrored app settings -----------------------------------------------
    // Every persisted app setting, so a config is COMPLETE and SELF-CONTAINED:
    // it fully describes the target instance without inheriting anything from
    // whichever copy happened to author it.
    //
    // Seeded from Constants — never from the running app. Reading live state
    // would silently bake the author machine's preferences into every screen it
    // generates, and editing here would change the running app instead of the
    // config being written.
    bool shuffle          = Constants::Slideshow::IS_SHUFFLE;
    int  transitionType   = 0; // TransitionType::Cut
    int  transitionSource = Constants::Slideshow::TransitionSource::NONE;
    int  transitionOrder  = Constants::Slideshow::TransitionOrder::SEQUENTIAL;

    int  viewMode          = static_cast<int>(Constants::ViewModes::defaultViewMode);
    int  baseWidth         = Constants::IS_BASE_WIDTH;
    int  baseHeight        = Constants::IS_BASE_HEIGHT;
    bool startFullscreen   = false;
    bool alwaysOnTop       = false;

    bool overlaysVisible   = Constants::Overlay::DEFAULT_SHOW_OVERLAY;
    bool overlayBackground = Constants::Overlay::IS_OVERLAY_SHOW_BACKGROUND;
    int  msgDurationMs     = static_cast<int>(Constants::Overlay::IS_MSG_CENTER_DISPLAY_MS);

    int  sortOrder   = Constants::FileHandler::FILE_HANDLER_DEFAULT_SORT_ORDER;
    bool sortReverse = Constants::FileHandler::FILE_HANDLER_SORT_TYPE_IS_REVERSE;

    int  vramCache        = Constants::IS_VRAM_CACHE_IMAGES_COUNT;
    int  preloadLookaside = Constants::IS_PRELOAD_LOOKASIDE_COUNT;
    int  thumbCacheMB     = Constants::IS_DIR_THUMB_CACHE_BUDGET_MB;

    bool swapMouse    = Constants::IS_SWAP_MOUSE_BUTTONS;
    bool invertWheel  = Constants::IS_MOUSE_VERTICAL_REVERSE_SCROLL_DIRECTION;
    bool invertWheelH = Constants::IS_MOUSE_HORIZONTAL_REVERSE_SCROLL_DIRECTION;
    int  zoomClick    = static_cast<int>(Constants::ZOOM_CLICK);
    int  caretStyle   = Constants::InputBox::CARET_STYLE;
    bool ctrlCEnabled = Constants::IS_CTRL_C_ENABLED;
    bool contextMenu  = Constants::IS_CONTEXT_MENU_ENABLED;

    // Which transitions are in the custom set, one bit per TransitionType.
    // Only consulted when transitionSource is LIST.
    uint32_t transitionList = 0xFFFFFFFEu; // every animated type, Cut excluded

    bool runOnStartup   = Constants::IS_ENABLE_RUN_ON_STARTUP;
    bool historyFull    = Constants::History::HISTORY_SHOW_FULL_HISTORY;
    int  historyMaxDirs = Constants::History::IS_HISTORY_MAX_DIRS_TO_SHOW;
    int  historyMaxFavs = Constants::History::IS_HISTORY_MAX_FAVORITES_TO_SHOW;
    int  historyMaxSave = Constants::History::IS_HISTORY_MAX_DIRS_TO_SAVE;

    bool thumbCopy   = Constants::IS_THUMB_COPY_ENABLED;
    bool thumbMove   = Constants::IS_THUMB_MOVE_ENABLED;
    bool thumbDelete = Constants::IS_THUMB_DELETE_ENABLED;
    bool thumbPaste  = Constants::IS_THUMB_PASTE_ENABLED;

    bool keepInBackground = Constants::IS_KEEP_IN_BACKGROUND;
    bool thumbnailEffects =
        Constants::ThumbnailPanel::ThumbnailEffects::EFFECTS_MASTER_ENABLED;
    bool openDirOnStart   = Constants::IS_OPEN_DIRWND_ON_START;
    int  themePercent     =
        static_cast<int>(Constants::Theme::DEFAULT_THEME_FACTOR * 100.0f);

    // Folders alone are not enough — at least one trigger must be armed.
    // Whether any promotion folder actually exists is decided at scan time.
    bool HasPromotions() const {
        return promoImagesFrom > 0 || promoTimeFrom > 0;
    }
    bool IsValid()       const { return !name.empty(); }
};

// Strips characters that cannot appear in a mutex, window-class, registry-value
// or file name, and trims length. Returns "" when nothing usable is left, which
// callers must treat as "not a dedicated instance".
std::wstring SanitizeInstanceName(const std::wstring &raw);

// --- Derived, per-instance identifiers -------------------------------------
// Each takes the sanitised instance name; an EMPTY name yields exactly the
// identifier the main instance has always used, so existing installs are
// untouched.
std::wstring MutexNameFor(const std::wstring &instance);
std::wstring WindowClassNameFor(const std::wstring &instance);

// --- Resolved identity for THIS process -------------------------------------
// Mutex resolution order, applied to every copy including the main app:
//   1. [Instance]Mutex in the .ini, when present — an explicit, hand-editable
//      override, so two copies can be forced to share or split a slot.
//   2. otherwise derived from the EXE FILE NAME.
// Keying on the exe name means renaming a copy gives it its own single-instance
// slot automatically — which is exactly how you get N side-by-side instances
// without configuring anything.
std::wstring ResolveMutexName();

// The mutex name derived purely from this exe (name + a hash of its folder).
// Written into a generated .ini so the identity is pinned from the first run.
std::wstring DefaultMutexName();

// The window class this process registers. The main app keeps the historic
// class name (external tools and the shell forwarding path look for it); a
// dedicated copy gets its own, so FindWindowW can never hand it a file meant
// for the main viewer.
std::wstring ResolveWindowClassName();
std::wstring RegistryPrefixFor(const std::wstring &instance);
std::wstring FilePrefixFor(const std::wstring &instance);
std::wstring RunValueNameFor(const std::wstring &instance);

// Command line that relaunches this exact configuration.
std::wstring BuildCommandLine(const InstanceConfig &cfg);

// --- Config persistence, into this exe's own .ini ---------------------------
// Writes the [Instance] identity block plus every configured value, so the next
// launch of this exe finds an .ini and starts straight into File mode.
void SaveConfig(const InstanceConfig &cfg);
void LoadConfig(InstanceConfig &cfg);

// Same, but targeting an ARBITRARY .ini — used when building a copy for another
// folder, and when testing/rebuilding a config that is not this process's own.
// Creates the file (UTF-16LE + header) if absent. Returns false if unreadable.
void WriteConfigTo(const std::wstring &iniPath, const InstanceConfig &cfg);
bool ReadConfigFrom(const std::wstring &iniPath, InstanceConfig &cfg);

// Writes a .lnk for `cfg` into `targetDir` (e.g. the Startup folder), named
// after the instance so several can coexist. Returns false on failure.
bool CreateInstanceShortcut(const InstanceConfig &cfg, const std::wstring &targetDir,
                            std::wstring &outPath);

// --- Live state for THIS process -------------------------------------------
// Populated at startup from the command line. The promo playlist is only ever
// scanned when this process is a dedicated instance with a promotions folder.
struct RuntimeState {
    InstanceConfig    config;
    PromotionPlaylist promotions;
    bool              active = false; // true when this process is a dedicated instance

    // Set while a promotion is on screen, so the paint path knows to show
    // promoPath instead of the current playlist entry.
    bool         showingPromotion = false;
    std::wstring promoPath;
};

// The one instance-scoped state block for this process.
RuntimeState &State();

// True when THIS process is a dedicated instance — either launched with a named
// -instance, or the legacy -dedicated flag.
bool IsDedicatedProcess();

// --- Promotions runtime ------------------------------------------------------
// Builds the promotion playlist from the loaded config: scans the folder, wires
// the order and both triggers. No-op when not dedicated or no folder is set.
void InitPromotions();

// Shows the next promotion, if one is due and decoded. Returns true when the
// screen now holds a promotion — the caller must then NOT advance the playlist.
//
// The promotion is displayed through the renderer's PATH-keyed cache, so
// app.playlist, app.currentIndex and the whole async decode pipeline are left
// exactly as they were. Nothing else in the app knows a promotion happened.
bool ShowNextPromotion(HWND hWnd);

// Warms the upcoming promotion into the bitmap cache so it is ready when due.
// Exactly one promotion is kept ahead, matching the one-ahead image lookaside.
void PreloadUpcomingPromotion(HWND hWnd);

// The icon resource id every window in this process should use. Single source
// of truth so the main window, the tray, the taskbar button and the floating
// panels can never disagree about which icon they are showing.
UINT AppIconId();

} // namespace Dedicated
