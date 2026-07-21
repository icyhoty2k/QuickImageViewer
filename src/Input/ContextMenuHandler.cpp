#include "ContextMenuHandler.h"
#include "Command.h"
#include "TrayHandler.h"
#include "Platform/Constants.h"
#include "Platform/ConstantsStrings.h"
#include "AppState.h"
#include "SlideshowTransitions.h" // TransitionDisplayOrder — shared menu/sequential order
#include <string>

#ifndef MF_RADIOCHECK
#define MF_RADIOCHECK 0x00000200L
#endif

extern AppState app;
// The one tray handler instance (created in AppMain.cpp). It owns the dispatch
// for every SETTINGS id — see the id-space note below.
extern Input::TrayHandler trayHandler;

namespace Input {

namespace {
    // =========================================================================
    // ID SPACE — two disjoint ranges, one menu.
    //
    //   < VIEWER_BASE : settings/app ids owned by TrayHandler::DispatchCommand
    //                   (1..120: toggles, prompts, view mode, sort, backup,
    //                   export/import/restore).
    //   >= VIEWER_BASE: viewer actions owned by THIS file — every one maps to a
    //                   Command and runs through InputManager::ExecuteCommand,
    //                   so a menu pick is identical to the keyboard shortcut.
    //
    // Keeping them disjoint is what lets a single menu be dispatched to the
    // right owner without either side knowing about the other's numbering.
    // =========================================================================
    constexpr int VIEWER_BASE = 1000;

    enum MenuId : int {
        ID_BROWSE = VIEWER_BASE, // open-file dialog                (F2)
        ID_HISTORY,              // toggle HistoryListWnd           (Tab)
        ID_THUMB_STRIP,          // toggle DirWnd thumbnail strip   (F6)
        ID_PREV_FOLDER,          // switch to previous folder       (Q)
        ID_PREV_IMAGE,           // switch to previously viewed img (E)
        ID_STATS,                // toggle StatsWnd                 (K)
        ID_METADATA,             // toggle image info / metadata    (M)
        ID_COPY,                 // copy image to clipboard         (Ctrl+C)
        ID_SAVE_AS,              // save image as…                  (Ctrl+S)
        ID_EXPLORER,             // reveal current file in Explorer (L)
        ID_HELP,                 // help / shortcuts window         (F1)
        ID_CLOSE_APP,            // hide to tray if kept in bg      (Esc)
        ID_CLOSE_PANELS,         // hide all panels + spawned DirWnds
        ID_RESTORE_PANELS,       // re-open the panels that hid
        ID_HARD_QUIT,            // full process exit               (Ctrl+Q)
        ID_SS_TOGGLE,            // slideshow start / stop          (Ctrl+F1)
        ID_SS_INTERVAL,          // prompt for slide duration
        ID_SS_LOOP,              // loop toggle                     (R)
        ID_SS_SHUFFLE,           // playlist shuffle toggle         (S)

        // Wallpaper submenu — contiguous, ordered like Constants::Wallpaper.
        ID_WALLPAPER_FIRST = VIEWER_BASE + 100,
        ID_WALLPAPER_LAST  = ID_WALLPAPER_FIRST + Constants::Wallpaper::COUNT - 1,

        // Transition submenu — contiguous, ordered like TransitionType.
        ID_TRANSITION_FIRST = VIEWER_BASE + 200,
        ID_TRANSITION_LAST  = ID_TRANSITION_FIRST + Constants::Slideshow::TRANSITION_COUNT - 1,

        // Transition SOURCE block — contiguous, ordered like TransitionSource.
        ID_TRANS_SRC_FIRST = VIEWER_BASE + 300,
        ID_TRANS_SRC_LAST  = ID_TRANS_SRC_FIRST +
                             Constants::Slideshow::TransitionSource::COUNT - 1,

        // Transition ORDER block — contiguous, ordered like TransitionOrder.
        ID_TRANS_ORD_FIRST = VIEWER_BASE + 400,
        ID_TRANS_ORD_LAST  = ID_TRANS_ORD_FIRST +
                             Constants::Slideshow::TransitionOrder::COUNT - 1,
    };

    // Tray-owned ids this file references by name when building the shared menu.
    // Values must match the cases in TrayHandler::DispatchCommand.
    constexpr int TRAY_ID_RESTORE_WINDOW = 1;

    // Viewer menu id → Command. Contiguous blocks resolve by offset.
    Command CommandForId(int id) {
        if (id >= ID_WALLPAPER_FIRST && id <= ID_WALLPAPER_LAST)
            return static_cast<Command>(static_cast<int>(Command::SetWallpaperFill) +
                                        (id - ID_WALLPAPER_FIRST));
        if (id >= ID_TRANSITION_FIRST && id <= ID_TRANSITION_LAST)
            return static_cast<Command>(static_cast<int>(Command::SetTransitionFirst) +
                                        (id - ID_TRANSITION_FIRST));
        if (id >= ID_TRANS_SRC_FIRST && id <= ID_TRANS_SRC_LAST)
            return static_cast<Command>(static_cast<int>(Command::SetTransitionSourceFirst) +
                                        (id - ID_TRANS_SRC_FIRST));
        if (id >= ID_TRANS_ORD_FIRST && id <= ID_TRANS_ORD_LAST)
            return static_cast<Command>(static_cast<int>(Command::SetTransitionOrderFirst) +
                                        (id - ID_TRANS_ORD_FIRST));
        switch (id) {
            case ID_BROWSE:         return Command::OpenFile;
            case ID_HISTORY:        return Command::ToggleHistory;
            case ID_THUMB_STRIP:    return Command::ToggleDir;
            case ID_PREV_FOLDER:    return Command::ToggleLastDir;
            case ID_PREV_IMAGE:     return Command::ToggleLastImage;
            case ID_STATS:          return Command::ToggleStats;
            case ID_METADATA:       return Command::ShowInfo;
            case ID_COPY:           return Command::CopyToClipboard;
            case ID_SAVE_AS:        return Command::SaveImage;
            case ID_EXPLORER:       return Command::ShowInExplorer;
            case ID_HELP:           return Command::ToggleHelp;
            case ID_CLOSE_APP:      return Command::HideToTray;
            case ID_CLOSE_PANELS:   return Command::CloseAllPanels;
            case ID_RESTORE_PANELS: return Command::RestoreAllPanels;
            case ID_HARD_QUIT:      return Command::HardQuit;
            case ID_SS_TOGGLE:      return Command::SlideshowToggle;
            case ID_SS_INTERVAL:    return Command::SlideshowSetInterval;
            case ID_SS_LOOP:        return Command::SlideshowToggleLoop;
            case ID_SS_SHUFFLE:     return Command::SlideshowToggleShuffle;
            default:                return Command::None;
        }
    }

    UINT CheckFlag(bool on) { return on ? MF_CHECKED : MF_UNCHECKED; }
    UINT RadioFlag(bool on) { return MF_STRING | MF_RADIOCHECK | CheckFlag(on); }

    HMENU BuildTransitionMenu(); // defined below; used by BuildSlideshowMenu

    // True when `id` is a row in the transition list AND ticking it means
    // "toggle membership" rather than "pick this one" — the only case where the
    // user plausibly wants to click again straight away.
    bool IsTransitionListToggle(int id) {
        return id >= ID_TRANSITION_FIRST && id <= ID_TRANSITION_LAST &&
               app.slideshow.transition.source ==
                   Constants::Slideshow::TransitionSource::LIST;
    }

    // One TrackPopupMenu round-trip. Takes ownership of hMenu.
    int TrackMenuAt(HMENU hMenu, HWND hWnd, int x, int y) {
        if (!hMenu) return 0;
        // SetForegroundWindow + the WM_NULL kick are the documented workaround so
        // the popup dismisses on the first click outside it.
        SetForegroundWindow(hWnd);
        const int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                       x, y, 0, hWnd, nullptr);
        PostMessage(hWnd, WM_NULL, 0, 0);
        DestroyMenu(hMenu); // frees the whole tree, submenus included
        return cmd;
    }

    // ── Settings submenu (tray id space) ─────────────────────────────────────
    HMENU BuildSettingsMenu() {
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING | CheckFlag(app.isKeepInBackground),      4,  L"Keep in Background");
        AppendMenuW(m, MF_STRING | CheckFlag(app.isEnableRunOnStartup),    5,  L"Run on Startup");
        AppendMenuW(m, MF_STRING | CheckFlag(app.thumbnailEffectsEnabled), 6,  L"Thumbnail Effects");
        AppendMenuW(m, MF_STRING | CheckFlag(app.historyFullModeEnabled),  7,  L"History: Open Full List");
        AppendMenuW(m, MF_STRING | CheckFlag(app.showOverlayInfoText),     8,  L"Info Overlays");
        AppendMenuW(m, MF_STRING | CheckFlag(app.openDirWndOnStart),       9,  L"Open Thumbnail Strip on Start");
        AppendMenuW(m, MF_STRING | CheckFlag(app.overlayShowBackground),   13, L"Overlay Background");
        AppendMenuW(m, MF_STRING | CheckFlag(app.swapMouseButtons),        14, L"Swap Mouse Buttons");
        AppendMenuW(m, MF_STRING | CheckFlag(app.contextMenuEnabled),      63, L"Right-Click Context Menu");
        AppendMenuW(m, MF_STRING | CheckFlag(app.invertWheelDirection),    15, L"Invert Scroll Direction");
        AppendMenuW(m, MF_STRING | CheckFlag(app.invertWheelDirectionH),   16, L"Invert Horizontal Scroll");
        AppendMenuW(m, MF_STRING | CheckFlag(app.startInFullscreen),       25, L"Start in Fullscreen");
        AppendMenuW(m, MF_STRING | CheckFlag(app.ctrlCEnabled),            49, L"Ctrl+C Copy to Clipboard");
        {
            HMENU caret = CreatePopupMenu();
            AppendMenuW(caret, RadioFlag(app.caretStyle == 0), 60, L"Bar (|)");
            AppendMenuW(caret, RadioFlag(app.caretStyle == 1), 61, L"Underscore (_)");
            AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(caret), L"Input Caret Style");
        }
        {
            HMENU ops = CreatePopupMenu();
            AppendMenuW(ops, MF_STRING | CheckFlag(app.thumbCopyEnabled),   50, L"Copy (Ctrl+C)");
            AppendMenuW(ops, MF_STRING | CheckFlag(app.thumbMoveEnabled),   51, L"Cut / Move (Ctrl+X)");
            AppendMenuW(ops, MF_STRING | CheckFlag(app.thumbDeleteEnabled), 52, L"Delete (Del)");
            AppendMenuW(ops, MF_STRING | CheckFlag(app.thumbPasteEnabled),  53, L"Paste (Ctrl+V)");
            AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(ops), L"Thumbnail Operations");
        }
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        wchar_t buf[80];
        swprintf_s(buf, L"VRAM Cache Size: %d", app.vramCacheCount);
        AppendMenuW(m, MF_STRING, 17, buf);
        const int zc = static_cast<int>(app.zoomClickMultiplier + 0.5f);
        if (zc <= 1) swprintf_s(buf, L"Left-Click Zoom: Off");
        else         swprintf_s(buf, L"Left-Click Zoom: %dx", zc);
        AppendMenuW(m, MF_STRING, 62, buf);
        swprintf_s(buf, L"Window Width: %d",  app.baseWidth);   AppendMenuW(m, MF_STRING, 23, buf);
        swprintf_s(buf, L"Window Height: %d", app.baseHeight);  AppendMenuW(m, MF_STRING, 24, buf);
        swprintf_s(buf, L"History Max Dirs: %d", app.historyMaxDirs); AppendMenuW(m, MF_STRING, 26, buf);
        swprintf_s(buf, L"History Max Favs: %d", app.historyMaxFavs); AppendMenuW(m, MF_STRING, 27, buf);
        swprintf_s(buf, L"Dir Thumb Cache: %d MB", app.dirThumbCacheMB); AppendMenuW(m, MF_STRING, 28, buf);
        swprintf_s(buf, L"Preload Lookaside: %d",  app.preloadLookaside); AppendMenuW(m, MF_STRING, 29, buf);
        swprintf_s(buf, L"Overlay Message Duration: %d ms", app.msgCenterDisplayMs); AppendMenuW(m, MF_STRING, 30, buf);
        swprintf_s(buf, L"History Save Limit: %d", app.historyMaxDirsSave); AppendMenuW(m, MF_STRING, 31, buf);
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, 10, L"Export Settings");
        AppendMenuW(m, MF_STRING, 11, L"Import Settings");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, 12, L"Restore Defaults");
        return m;
    }

    HMENU BuildViewModeMenu() {
        HMENU m = CreatePopupMenu();
        const int vm = static_cast<int>(app.viewMode);
        AppendMenuW(m, RadioFlag(vm == 1), 18, L"1 — Fit to View (preserve aspect)");
        AppendMenuW(m, RadioFlag(vm == 2), 19, L"2 — Fit to Width");
        AppendMenuW(m, RadioFlag(vm == 3), 20, L"3 — Fit to Height");
        AppendMenuW(m, RadioFlag(vm == 4), 21, L"4 — Stretch to Window");
        AppendMenuW(m, RadioFlag(vm == 5), 22, L"5 — Original Size");
        return m;
    }

    // Slideshow is entirely command-driven (viewer id space) so the tray and the
    // viewer cannot drift apart.
    HMENU BuildSlideshowMenu() {
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, ID_SS_TOGGLE,
                    app.slideshow.running ? L"Stop\tCtrl+F1" : L"Start\tCtrl+F1");

        wchar_t buf[64];
        swprintf_s(buf, L"Interval: %d ms", app.slideshow.intervalMs);
        AppendMenuW(m, MF_STRING, ID_SS_INTERVAL, buf);

        AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(BuildTransitionMenu()),
                    L"Transition");

        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | CheckFlag(app.slideshow.loop),    ID_SS_LOOP,    L"Loop\tR");
        AppendMenuW(m, MF_STRING | CheckFlag(app.slideshow.shuffle), ID_SS_SHUFFLE, L"Shuffle\tS");
        return m;
    }

    // Standalone so it can be re-shown on its own after each checkbox tick — see
    // the re-open loop in Show(). A Win32 menu always closes on selection, so
    // multi-select is only possible by putting the menu straight back up.
    HMENU BuildTransitionMenu() {
        HMENU trans = CreatePopupMenu();
        {
            namespace SS = Constants::Slideshow;
            const auto &tr = app.slideshow.transition;

            // Source: which transitions are in play.
            for (int i = 0; i < SS::TransitionSource::COUNT; ++i)
                AppendMenuW(trans, RadioFlag(tr.source == i), ID_TRANS_SRC_FIRST + i,
                            Constants::Messages::TRANSITION_SOURCE_NAMES[i]);

            // Order: how the next one is drawn. Meaningless for NONE (a pool of
            // one), so grey it out there rather than implying it does something.
            AppendMenuW(trans, MF_SEPARATOR, 0, nullptr);
            const bool orderApplies = (tr.source != SS::TransitionSource::NONE);
            for (int i = 0; i < SS::TransitionOrder::COUNT; ++i)
                AppendMenuW(trans,
                            RadioFlag(orderApplies && tr.order == i) |
                            (orderApplies ? 0u : (MF_DISABLED | MF_GRAYED)),
                            ID_TRANS_ORD_FIRST + i,
                            Constants::Messages::TRANSITION_ORDER_NAMES[i]);
            AppendMenuW(trans, MF_SEPARATOR, 0, nullptr);

            // The list, A-Z and numbered. Order comes from TransitionDisplayOrder()
            // — the same function SEQUENTIAL playback walks — so entry N here is
            // the transition slide N will use. Ids stay keyed to the enum value
            // (what gets persisted), so the enum itself must never be reordered.
            //
            // What a row MEANS depends on the source:
            //   LIST → a checkbox: is this transition in the custom pool?
            //   else → a radio:    this is THE transition (and selects source NONE)
            const bool listMode = (tr.source == SS::TransitionSource::LIST);
            const bool noneMode = (tr.source == SS::TransitionSource::NONE);
            const int tt = static_cast<int>(tr.type);
            const int *order = TransitionDisplayOrder();

            wchar_t label[80];
            for (int n = 0; n < SS::TRANSITION_COUNT; ++n) {
                const int i = order[n];
                swprintf_s(label, L"%2d.  %s", n + 1, Constants::Messages::TRANSITION_NAMES[i]);
                UINT flags;
                if (listMode)
                    flags = MF_STRING | CheckFlag((tr.listMask & (1u << i)) != 0u);
                else
                    flags = RadioFlag(noneMode && tt == i);
                AppendMenuW(trans, flags, ID_TRANSITION_FIRST + i, label);
            }
        }
        return trans;
    }

    HMENU BuildSortMenu() {
        HMENU m = CreatePopupMenu();
        HMENU order = CreatePopupMenu();
        const int so = app.fileHandlerDefaultSortOrder;
        AppendMenuW(order, RadioFlag(so == 0), 43, L"Name");
        AppendMenuW(order, RadioFlag(so == 1), 44, L"Date Modified");
        AppendMenuW(order, RadioFlag(so == 2), 45, L"Size");
        AppendMenuW(order, RadioFlag(so == 3), 46, L"Type");
        AppendMenuW(order, RadioFlag(so == 4), 47, L"Disk Order");
        AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(order), L"Sort Order");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | CheckFlag(app.fileHandlerIsReverseSortOrder),
                    48, L"Reverse Order");
        return m;
    }

    HMENU BuildWallpaperMenu() {
        HMENU m = CreatePopupMenu();
        for (int i = 0; i < Constants::Wallpaper::COUNT; ++i)
            AppendMenuW(m, MF_STRING, ID_WALLPAPER_FIRST + i,
                        Constants::Messages::WALLPAPER_NAMES[i]);
        return m;
    }

    HMENU BuildBackupMenu() {
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, 41, L"Backup History && Favorites");
        AppendMenuW(m, MF_STRING, 42, L"Restore History && Favorites");
        return m;
    }
}

// =============================================================================
// BuildMenu — THE menu. Both the tray icon and the main-window right-click show
// exactly this, so behaviour can never diverge between the two.
// =============================================================================
HMENU ContextMenuHandler::BuildMenu(HWND hWnd) {
    HMENU m = CreatePopupMenu();
    if (!m) return nullptr;

    AppendMenuW(m, MF_STRING, ID_BROWSE,      L"Browse…\tF2");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_HISTORY,     L"History\tTab");
    AppendMenuW(m, MF_STRING, ID_THUMB_STRIP, L"Thumbnail Strip\tF6");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_PREV_FOLDER, L"Previous Folder\tQ");
    AppendMenuW(m, MF_STRING, ID_PREV_IMAGE,  L"Previous Image\tE");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_STATS,       L"Statistics\tK");
    AppendMenuW(m, MF_STRING, ID_METADATA,    L"Metadata\tM");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_COPY,        L"Copy\tCtrl+C");
    AppendMenuW(m, MF_STRING, ID_SAVE_AS,     L"Save As…\tCtrl+S");
    AppendMenuW(m, MF_STRING, ID_EXPLORER,    L"Open File in Explorer\tL");
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(BuildWallpaperMenu()),
                L"Set as Desktop Wallpaper");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(BuildSettingsMenu()),  L"Settings");
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(BuildViewModeMenu()),  L"View Mode");
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(BuildSlideshowMenu()), L"Slideshow");
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(BuildSortMenu()),      L"Sort");
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(BuildBackupMenu()),    L"Backup");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    // Only meaningful from the tray, when the window is not on screen.
    if (!IsWindowVisible(hWnd))
        AppendMenuW(m, MF_STRING, TRAY_ID_RESTORE_WINDOW, L"Restore QuickImageViewer");
    AppendMenuW(m, MF_STRING, ID_CLOSE_APP,      L"Close App\tEsc");
    AppendMenuW(m, MF_STRING, ID_CLOSE_PANELS,   L"Close All Panels\tN");
    AppendMenuW(m, MF_STRING, ID_RESTORE_PANELS, L"Restore All Panels\tN");
    AppendMenuW(m, MF_STRING, ID_HARD_QUIT,      L"Hard Quit\tCtrl+Q");

    // Help is last and carries the version — it replaces the old footer caption.
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    const std::wstring help = std::wstring(L"Help v") + Constants::APP_VERSION + L"\tF1";
    AppendMenuW(m, MF_STRING, ID_HELP, help.c_str());
    return m;
}

// =============================================================================
// Dispatch — routes one chosen id to its owner (see the ID SPACE note above).
// =============================================================================
void ContextMenuHandler::Dispatch(HWND hWnd, int id) {
    if (id <= 0) return;
    if (id >= VIEWER_BASE) {
        Command c = CommandForId(id);
        if (c != Command::None)
            InputManager::ExecuteCommand(hWnd, c);
        return;
    }
    trayHandler.DispatchCommand(hWnd, id); // settings / app id space
}

// =============================================================================
// Show — build, track, dispatch. x / y are SCREEN coordinates.
// =============================================================================
void ContextMenuHandler::Show(HWND hWnd, int x, int y) {
    // The slideshow may have hidden the pointer — bring it back for the menu and
    // block the hide timer for as long as any popup is up.
    app.isContextMenuOpen = true;
    if (app.slideshow.cursorHidden) {
        ShowCursor(TRUE);
        app.slideshow.cursorHidden = false;
    }
    KillTimer(hWnd, Constants::Slideshow::CURSOR_TIMER_ID);

    // TPM_RETURNCMD makes TrackPopupMenu return the chosen id instead of posting
    // WM_COMMAND, so everything dispatches inline.
    int cmd = TrackMenuAt(BuildMenu(hWnd), hWnd, x, y);
    Dispatch(hWnd, cmd);

    // Ticking a transition in LIST mode is inherently a multi-select gesture, but
    // Win32 closes the menu on every pick. Put the transition popup straight back
    // up — rebuilt, so the new tick shows — until the user chooses something else
    // or dismisses it. Re-anchored at the ORIGINAL point, not the cursor: a fixed
    // position keeps every row where it was, so ticking several in a row is just
    // moving down the list. Following the cursor would shift the rows each time.
    while (IsTransitionListToggle(cmd)) {
        cmd = TrackMenuAt(BuildTransitionMenu(), hWnd, x, y);
        Dispatch(hWnd, cmd);
    }

    app.isContextMenuOpen = false;
    // Restart the inactivity countdown only if a slideshow is actually running.
    if (app.slideshow.running && !app.slideshow.paused && app.slideshow.cursorHideMs > 0)
        SetTimer(hWnd, Constants::Slideshow::CURSOR_TIMER_ID,
                 app.slideshow.cursorHideMs, nullptr);
}

} // namespace Input
