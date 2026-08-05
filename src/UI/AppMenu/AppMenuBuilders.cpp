// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#include "AppMenu.h"
#include "AppMenuIds.h"
#include "AppMenuInternal.h"

#include "AppState.h"
#include "Dedicated/DedicatedSettings.h" // SettingsUseFile — the Location item
#include "Input/Command.h"
#include "Overlays/OverlayManager.h"
#include "Platform/Constants.h"
#include "Platform/ConstantsStrings.h"
#include "SlideshowTransitions.h" // TransitionDisplayOrder — shared menu/sequential order
#include "Rem_TCP_IP/RemoteMirror.h" // ConnectedCount / MirroredLiveCount — the
                                     // status line under RemoteActivation

#include <string>

#ifndef MF_RADIOCHECK
#define MF_RADIOCHECK 0x00000200L
#endif

extern AppState app;
extern OverlayManager g_overlayManager;

// =============================================================================
// AppMenuBuilders — every menu and submenu, and the viewer id → Command map.
//
// Building is separated from dispatching because they change for different
// reasons: this file changes when an item moves or a label is reworded, and
// AppMenuSettings.cpp changes when an action's behaviour changes. Both read the
// same ids from AppMenuIds.h, which is what stops them drifting apart — the
// failure this whole folder exists to prevent.
// =============================================================================

namespace UI::AppMenu {

namespace Id = UI::AppMenu::Ids;

namespace detail {

UINT CheckFlag(bool on) { return on ? MF_CHECKED : MF_UNCHECKED; }
UINT RadioFlag(bool on) { return MF_STRING | MF_RADIOCHECK | CheckFlag(on); }

bool IsTransitionListToggle(int id) {
    return id >= Id::ID_TRANSITION_FIRST && id <= Id::ID_TRANSITION_LAST &&
           app.slideshow.transition.source ==
               Constants::Slideshow::TransitionSource::LIST;
}

Command CommandForId(int id) {
    if (id >= Id::ID_WALLPAPER_FIRST && id <= Id::ID_WALLPAPER_LAST)
        return static_cast<Command>(static_cast<int>(Command::SetWallpaperFill) +
                                    (id - Id::ID_WALLPAPER_FIRST));
    if (id >= Id::ID_TRANSITION_FIRST && id <= Id::ID_TRANSITION_LAST)
        return static_cast<Command>(static_cast<int>(Command::SetTransitionFirst) +
                                    (id - Id::ID_TRANSITION_FIRST));
    if (id >= Id::ID_TRANS_SRC_FIRST && id <= Id::ID_TRANS_SRC_LAST)
        return static_cast<Command>(static_cast<int>(Command::SetTransitionSourceFirst) +
                                    (id - Id::ID_TRANS_SRC_FIRST));
    if (id >= Id::ID_TRANS_ORD_FIRST && id <= Id::ID_TRANS_ORD_LAST)
        return static_cast<Command>(static_cast<int>(Command::SetTransitionOrderFirst) +
                                    (id - Id::ID_TRANS_ORD_FIRST));
    if (id >= Id::ID_VIEW_MODE_FIRST && id <= Id::ID_VIEW_MODE_LAST)
        return static_cast<Command>(static_cast<int>(Command::ViewMode1) +
                                    (id - Id::ID_VIEW_MODE_FIRST));
    switch (id) {
        case Id::ID_BROWSE:          return Command::OpenFile;
        case Id::ID_HISTORY:         return Command::ToggleHistory;
        case Id::ID_THUMB_STRIP:     return Command::ToggleDir;
        case Id::ID_PREV_FOLDER:     return Command::ToggleLastDir;
        case Id::ID_PREV_IMAGE:      return Command::ToggleLastImage;
        case Id::ID_STATS:           return Command::ToggleStats;
        case Id::ID_METADATA:        return Command::ShowInfo;
        case Id::ID_COPY:            return Command::CopyToClipboard;
        case Id::ID_SAVE_AS:         return Command::SaveImage;
        case Id::ID_EXPLORER:        return Command::ShowInExplorer;
        case Id::ID_NEXT_MONITOR:    return Command::MoveToNextMonitor;
        case Id::ID_HELP:            return Command::ToggleHelp;
        case Id::ID_CLOSE_APP:       return Command::HideToTray;
        case Id::ID_CLOSE_PANELS:    return Command::CloseAllPanels;
        case Id::ID_RESTORE_PANELS:  return Command::RestoreAllPanels;
        case Id::ID_HARD_QUIT:       return Command::HardQuit;
        case Id::ID_SS_TOGGLE:       return Command::SlideshowToggle;
        case Id::ID_SS_INTERVAL:     return Command::SlideshowSetInterval;
        case Id::ID_SS_LOOP:         return Command::SlideshowToggleLoop;
        case Id::ID_SS_SHUFFLE:      return Command::SlideshowToggleShuffle;
        case Id::ID_DEDICATED_PANEL: return Command::ToggleDedicatedPanel;
        case Id::ID_REMOTE_PANEL:    return Command::ToggleRemotePanel;
        case Id::ID_REMOTES_CONSOLE: return Command::ToggleRemotesConsole;
        case Id::ID_REMOTES_CONTROL: return Command::MirrorPick;
        case Id::ID_REMOTE_CMD:      return Command::ToggleRemoteCmd;
        case Id::ID_REMOTE_LOG:      return Command::ToggleRemoteLog;
        case Id::ID_REMOTE_BEACON:   return Command::ToggleRemoteBeacon;
        // Straight to the same commands F11 and F12 resolve to, so the menu and
        // the keys cannot drift apart — the overlay, the mirror gate and the
        // "nothing picked" handling all come free.
        case Id::ID_REMOTE_MIRROR:    return Command::MirrorToggle;
        case Id::ID_REMOTE_EXEC_HERE: return Command::MirrorLocalToggle;
        case Id::ID_REMOTE_CLIENTS:   return Command::ToggleRemoteClients;
        // The same commands Ctrl+Enter, Ctrl+Shift+Enter, Alt+Enter and
        // Ctrl+Alt+Enter resolve to. The menu is a second way to press the key,
        // never a second implementation.
        case Id::ID_REMOTE_SYNC_NOW:     return Command::MirrorSyncNow;
        case Id::ID_REMOTE_PUSH_POS:     return Command::SendImagePositionToRemotes;
        case Id::ID_REMOTE_PUSH_POS_ALL: return Command::SendImagePositionToAllRemotes;
        case Id::ID_REMOTE_STREAM_OUT:   return Command::StreamImageToRemotes;
        case Id::ID_REMOTE_STREAM_IN:    return Command::StreamImageFromRemote;
        default:                     return Command::None;
    }
}

// ── Per-slot overlay submenu (settings id space) ────────────────────────────
static HMENU BuildSlotSubmenu(OverlayManager::Slot s) {
    const int   idx  = static_cast<int>(s);
    const bool  vis  = g_overlayManager.IsSlotVisible(s);
    const bool  cmp  = g_overlayManager.IsCompact(s);
    const bool  isOff     = !vis;
    const bool  isFull    = vis && !cmp;
    const bool  isCompact = vis && cmp;
    HMENU m = CreatePopupMenu();
    // BOT_LEFT prints two independent readouts. Their toggles sit above the
    // slot's own radio group because they decide whether there is anything
    // to show at all, which the Compact/Full/Off choice then formats.
    if (s == OverlayManager::BOT_LEFT) {
        AppendMenuW(m, MF_STRING | CheckFlag(app.overlayShowEffectsList),
                    Id::SET_OVERLAY_EFFECTS_LIST, L"Effects");
        AppendMenuW(m, MF_STRING | CheckFlag(app.overlayShowDirName),
                    Id::SET_OVERLAY_DIR_NAME,     L"Folder Name");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    }
    if (s == OverlayManager::MID_CENTER) {
        // MID_CENTER is always single-line — compact toggle is a no-op,
        // so only expose On / Off.
        AppendMenuW(m, RadioFlag(!isOff), Id::SET_OVERLAY_FULL_BASE + idx, L"On");
        AppendMenuW(m, RadioFlag(isOff),  Id::SET_OVERLAY_OFF_BASE  + idx, L"Off");
    } else {
        AppendMenuW(m, RadioFlag(isCompact), Id::SET_OVERLAY_COMPACT_BASE + idx, L"Compact");
        AppendMenuW(m, RadioFlag(isFull),    Id::SET_OVERLAY_FULL_BASE    + idx, L"Full");
        AppendMenuW(m, RadioFlag(isOff),     Id::SET_OVERLAY_OFF_BASE     + idx, L"Off");
    }
    return m;
}

// ── Overlays submenu (settings id space) ────────────────────────────────────
static HMENU BuildOverlaysMenu() {
    HMENU m = CreatePopupMenu();
    using S = OverlayManager::Slot;

    // ── Global overlay settings ─────────────────────────────────────────────
    AppendMenuW(m, MF_STRING | CheckFlag(app.showOverlayInfoText),   Id::SET_OVERLAY_MASTER, L"Info Overlays (Master)\tI");
    AppendMenuW(m, MF_STRING | CheckFlag(app.overlayShowBackground), Id::SET_OVERLAY_BG,     L"Overlay Background\tP");
    {
        HMENU lay = CreatePopupMenu();
        const int mode = app.overlayLayoutMode;
        AppendMenuW(lay, RadioFlag(mode == 0), Id::SET_LAYOUT_GRID,    L"Grid\tO");
        AppendMenuW(lay, RadioFlag(mode == 1), Id::SET_LAYOUT_STACKED, L"Stacked");
        AppendMenuW(lay, RadioFlag(mode == 2), Id::SET_LAYOUT_SUMMARY, L"Summary");
        AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(lay), L"Layout");
    }
    wchar_t buf[64];
    // ── Outer-slot text style ───────────────────────────────────────────────
    // Applies to the eight slots around the edge. MID_CENTER keeps its own
    // colour and size — it has to stay readable whatever these are set to.
    {
        HMENU fonts = CreatePopupMenu();
        for (int i = 0; i < Constants::Overlay::OVERLAY_FONT_FAMILY_COUNT; ++i)
            AppendMenuW(fonts, RadioFlag(app.overlayFontFamily == i),
                        Id::SET_OVERLAY_FONT_FAMILY_BASE + i,
                        Constants::Overlay::OVERLAY_FONT_FAMILIES[i]);
        AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(fonts), L"Font");
    }
    swprintf_s(buf, L"Font Size: %d", app.overlayFontSize);
    AppendMenuW(m, MF_STRING, Id::SET_OVERLAY_FONT_SIZE, buf);
    AppendMenuW(m, MF_STRING, Id::SET_OVERLAY_FONT_COLOR, L"Font Color…");

    swprintf_s(buf, L"Message Duration: %d ms", app.msgCenterDisplayMs);
    AppendMenuW(m, MF_STRING, Id::SET_MSG_DURATION, buf);

    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

    // ── Per-slot submenus ───────────────────────────────────────────────────
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(BuildSlotSubmenu(S::TOP_LEFT)),   L"Top Left  (Index / File)");
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(BuildSlotSubmenu(S::TOP_CENTER)), L"Top Center  (Panel Selection)");
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(BuildSlotSubmenu(S::TOP_RIGHT)),  L"Top Right  (Zoom)");
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(BuildSlotSubmenu(S::MID_LEFT)),   L"Mid Left  (Panel Selection)");
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(BuildSlotSubmenu(S::MID_CENTER)), L"Mid Center  (Messages)");
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(BuildSlotSubmenu(S::MID_RIGHT)),  L"Mid Right  (Panel Selection)");
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(BuildSlotSubmenu(S::BOT_LEFT)),   L"Bot Left  (Effects)");
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(BuildSlotSubmenu(S::BOT_CENTER)), L"Bot Center  (Panel Selection)");
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(BuildSlotSubmenu(S::BOT_RIGHT)),  L"Bot Right  (Dimensions)");

    return m;
}

// ── Settings submenu (settings id space) ────────────────────────────────────
static HMENU BuildSettingsMenu() {
    HMENU m = CreatePopupMenu();

    // TOP, and deliberately: every item below it writes somewhere, and this
    // says where. The two backing stores are indistinguishable from the menu
    // otherwise, and which one is in use decides whether a settings problem is
    // fixed by editing a file or by clearing a registry key.
    AppendMenuW(m, MF_STRING, Id::SET_LOCATION,
                Dedicated::SettingsUseFile() ? L"Location = File — open folder"
                                             : L"Location = Registry — open regedit");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(m, MF_STRING | CheckFlag(app.isKeepInBackground),      Id::SET_KEEP_IN_BG,     L"Keep in Background");
    AppendMenuW(m, MF_STRING | CheckFlag(app.isEnableRunOnStartup),    Id::SET_RUN_ON_STARTUP, L"Run on Startup");
    AppendMenuW(m, MF_STRING | CheckFlag(app.thumbnailEffectsEnabled), Id::SET_THUMB_EFFECTS,  L"Thumbnail Effects");
    AppendMenuW(m, MF_STRING | CheckFlag(app.lockViewport),            Id::SET_LOCK_VIEWPORT,  L"Lock Viewport (keep zoom/pan)\tY");
    AppendMenuW(m, MF_STRING | CheckFlag(app.historyFullModeEnabled),  Id::SET_HISTORY_FULL,   L"History: Open Full List");
    AppendMenuW(m, MF_STRING | CheckFlag(app.openDirWndOnStart),       Id::SET_OPEN_DIRWND,    L"Open Thumbnail Strip on Start");
    AppendMenuW(m, MF_STRING | CheckFlag(app.swapMouseButtons),        Id::SET_SWAP_MOUSE,     L"Swap Mouse Buttons");
    AppendMenuW(m, MF_STRING | CheckFlag(app.contextMenuEnabled),      Id::SET_CONTEXT_MENU,   L"Right-Click Context Menu");
    AppendMenuW(m, MF_STRING | CheckFlag(app.invertWheelDirection),    Id::SET_WHEEL_INVERT,   L"Invert Scroll Direction");
    AppendMenuW(m, MF_STRING | CheckFlag(app.invertWheelDirectionH),   Id::SET_WHEEL_INVERT_H, L"Invert Horizontal Scroll");
    AppendMenuW(m, MF_STRING | CheckFlag(app.startInFullscreen),       Id::SET_START_FULLSCREEN, L"Start in Fullscreen");
    AppendMenuW(m, MF_STRING | CheckFlag(app.isAlwaysOnTop),           Id::SET_ALWAYS_ON_TOP,  L"Always on Top\tCtrl+T");
    AppendMenuW(m, MF_STRING | CheckFlag(app.keepDisplayAwake),        Id::SET_KEEP_AWAKE,     L"Keep Display Awake");
    // The lock's only escape hatch. TrackPopupMenu runs its own message loop,
    // so this item still works when the main window is swallowing input —
    // which is precisely why the lock is safe to persist.
    AppendMenuW(m, MF_STRING | CheckFlag(app.isLocked),                Id::SET_KIOSK_LOCK,     L"Kiosk Lock (blocks all input)");
    AppendMenuW(m, MF_STRING | CheckFlag(app.ctrlCEnabled),            Id::SET_CTRL_C,         L"Ctrl+C Copy to Clipboard");
    {
        HMENU caret = CreatePopupMenu();
        AppendMenuW(caret, RadioFlag(app.caretStyle == 0), Id::SET_CARET_BAR,        L"Bar (|)");
        AppendMenuW(caret, RadioFlag(app.caretStyle == 1), Id::SET_CARET_UNDERSCORE, L"Underscore (_)");
        AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(caret), L"Input Caret Style");
    }
    {
        HMENU ops = CreatePopupMenu();
        AppendMenuW(ops, MF_STRING | CheckFlag(app.thumbCopyEnabled),   Id::SET_THUMB_COPY,   L"Copy (Ctrl+C)");
        AppendMenuW(ops, MF_STRING | CheckFlag(app.thumbMoveEnabled),   Id::SET_THUMB_MOVE,   L"Cut / Move (Ctrl+X)");
        AppendMenuW(ops, MF_STRING | CheckFlag(app.thumbDeleteEnabled), Id::SET_THUMB_DELETE, L"Delete (Del)");
        AppendMenuW(ops, MF_STRING | CheckFlag(app.thumbPasteEnabled),  Id::SET_THUMB_PASTE,  L"Paste (Ctrl+V)");
        AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(ops), L"Thumbnail Operations");
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    wchar_t buf[80];
    swprintf_s(buf, L"VRAM Cache Size: %d", app.vramCacheCount);
    AppendMenuW(m, MF_STRING, Id::SET_VRAM_CACHE, buf);
    if (app.zoomClickMultiplier <= Constants::ZOOM_CLICK_MIN) swprintf_s(buf, L"Left-Click Zoom: Off");
    else         swprintf_s(buf, L"Left-Click Zoom: %.2fx", app.zoomClickMultiplier);
    AppendMenuW(m, MF_STRING, Id::SET_ZOOM_CLICK, buf);
    swprintf_s(buf, L"Window Width: %d",  app.baseWidth);   AppendMenuW(m, MF_STRING, Id::SET_WINDOW_WIDTH, buf);
    swprintf_s(buf, L"Window Height: %d", app.baseHeight);  AppendMenuW(m, MF_STRING, Id::SET_WINDOW_HEIGHT, buf);
    swprintf_s(buf, L"History Max Dirs: %d", app.historyMaxDirs); AppendMenuW(m, MF_STRING, Id::SET_HISTORY_MAX_DIRS, buf);
    swprintf_s(buf, L"History Max Favs: %d", app.historyMaxFavs); AppendMenuW(m, MF_STRING, Id::SET_HISTORY_MAX_FAVS, buf);
    swprintf_s(buf, L"Dir Thumb Cache: %d MB", app.dirThumbCacheMB); AppendMenuW(m, MF_STRING, Id::SET_DIR_THUMB_CACHE, buf);
    swprintf_s(buf, L"Preload Lookaside: %d",  app.preloadLookaside); AppendMenuW(m, MF_STRING, Id::SET_PRELOAD_LOOKASIDE, buf);
    swprintf_s(buf, L"History Save Limit: %d", app.historyMaxDirsSave); AppendMenuW(m, MF_STRING, Id::SET_HISTORY_SAVE_MAX, buf);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, Id::SET_EXPORT, L"Export Settings");
    AppendMenuW(m, MF_STRING, Id::SET_IMPORT, L"Import Settings");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, Id::SET_RESTORE_DEFAULTS, L"Restore Defaults");
    return m;
}

// Viewer id space — a pick here runs Command::ViewMode1..5, the same code
// the 1..5 keys run, so both re-clamp the pan offset identically.
static HMENU BuildViewModeMenu() {
    HMENU m = CreatePopupMenu();
    const int vm = static_cast<int>(app.viewMode);
    AppendMenuW(m, RadioFlag(vm == 1), Id::ID_VIEW_MODE_FIRST + 0, L"1 — Fit to View (preserve aspect)");
    AppendMenuW(m, RadioFlag(vm == 2), Id::ID_VIEW_MODE_FIRST + 1, L"2 — Fit to Width");
    AppendMenuW(m, RadioFlag(vm == 3), Id::ID_VIEW_MODE_FIRST + 2, L"3 — Fit to Height");
    AppendMenuW(m, RadioFlag(vm == 4), Id::ID_VIEW_MODE_FIRST + 3, L"4 — Stretch to Window");
    AppendMenuW(m, RadioFlag(vm == 5), Id::ID_VIEW_MODE_FIRST + 4, L"5 — Original Size");
    return m;
}

// Standalone so it can be re-shown on its own after each checkbox tick — see
// the re-open loop in AppMenu.cpp. A Win32 menu always closes on selection, so
// multi-select is only possible by putting the menu straight back up.
HMENU BuildTransitionMenu() {
    HMENU trans = CreatePopupMenu();
    {
        namespace SS = Constants::Slideshow;
        const auto &tr = app.slideshow.transition;

        // Source: which transitions are in play.
        for (int i = 0; i < SS::TransitionSource::COUNT; ++i)
            AppendMenuW(trans, RadioFlag(tr.source == i), Id::ID_TRANS_SRC_FIRST + i,
                        Constants::Messages::TRANSITION_SOURCE_NAMES[i]);

        // Order: how the next one is drawn. Meaningless for NONE (a pool of
        // one), so grey it out there rather than implying it does something.
        AppendMenuW(trans, MF_SEPARATOR, 0, nullptr);
        const bool orderApplies = (tr.source != SS::TransitionSource::NONE);
        for (int i = 0; i < SS::TransitionOrder::COUNT; ++i)
            AppendMenuW(trans,
                        RadioFlag(orderApplies && tr.order == i) |
                        (orderApplies ? 0u : (MF_DISABLED | MF_GRAYED)),
                        Id::ID_TRANS_ORD_FIRST + i,
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
            AppendMenuW(trans, flags, Id::ID_TRANSITION_FIRST + i, label);
        }
    }
    return trans;
}

// Slideshow is entirely command-driven (viewer id space) so the tray and the
// viewer cannot drift apart.
static HMENU BuildSlideshowMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, Id::ID_SS_TOGGLE,
                app.slideshow.running ? L"Stop\tCtrl+F1" : L"Start\tCtrl+F1");

    wchar_t buf[64];
    swprintf_s(buf, L"Interval: %d ms", app.slideshow.intervalMs);
    AppendMenuW(m, MF_STRING, Id::ID_SS_INTERVAL, buf);

    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(BuildTransitionMenu()),
                L"Transition");

    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | CheckFlag(app.slideshow.loop),    Id::ID_SS_LOOP,    L"Loop\tR");
    AppendMenuW(m, MF_STRING | CheckFlag(app.slideshow.shuffle), Id::ID_SS_SHUFFLE, L"Shuffle\tS");
    return m;
}

static HMENU BuildSortMenu() {
    HMENU m = CreatePopupMenu();
    HMENU order = CreatePopupMenu();
    const int so = app.fileHandlerDefaultSortOrder;
    AppendMenuW(order, RadioFlag(so == 0), Id::SET_SORT_FIRST + 0, L"Name");
    AppendMenuW(order, RadioFlag(so == 1), Id::SET_SORT_FIRST + 1, L"Date Modified");
    AppendMenuW(order, RadioFlag(so == 2), Id::SET_SORT_FIRST + 2, L"Size");
    AppendMenuW(order, RadioFlag(so == 3), Id::SET_SORT_FIRST + 3, L"Type");
    AppendMenuW(order, RadioFlag(so == 4), Id::SET_SORT_FIRST + 4, L"Disk Order");
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(order), L"Sort Order");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | CheckFlag(app.fileHandlerIsReverseSortOrder),
                Id::SET_SORT_REVERSE, L"Reverse Order");
    return m;
}

// RemoteActivation — the two mirroring switches, shown as checkboxes.
//
// The point is READING them, not only setting them. F11 and F12 announce
// themselves on a centre overlay that fades after a couple of seconds, so once
// it had gone the only way to find out whether mirroring was on was to press
// the key — which also changed it. Opening this submenu answers the question
// without touching anything.
//
// Checkable rather than two separate on/off rows: these are toggles, and a tick
// is how Windows spells a toggle everywhere else in this menu.
static HMENU BuildRemoteBindingsMenu() {
    HMENU m = CreatePopupMenu();

    AppendMenuW(m, MF_STRING | CheckFlag(app.passCommandToRemote),
                Id::ID_REMOTE_MIRROR, L"Mirror my commands to remotes\tF11");
    // Says plainly what OFF means, because "also execute here" being off turns
    // this viewer into a pure remote control — its own screen stops moving,
    // which looks broken to anyone who did not set it.
    AppendMenuW(m, MF_STRING | CheckFlag(app.resendCommandToCaller),
                Id::ID_REMOTE_EXEC_HERE, L"…and also run them here\tF12");

    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

    // The ACTS, under the two switches. Every one is a key that already exists;
    // gathering them here is what makes the set visible — the three Enter forms
    // in particular are impossible to discover from the keyboard alone.
    //
    // Greyed with nothing connected rather than hidden: an item that disappears
    // teaches nothing, and "why is this grey" is answered by the status line at
    // the bottom of this same menu.
    const int live   = Remote::Mirror::ConnectedCount();
    const int driven = Remote::Mirror::MirroredLiveCount();
    const UINT actFlag = (live == 0) ? (MF_STRING | MF_GRAYED) : MF_STRING;

    AppendMenuW(m, actFlag, Id::ID_REMOTE_SYNC_NOW,
                L"Sync now — send my folder, image && view");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, actFlag, Id::ID_REMOTE_PUSH_POS,
                L"Go to my picture there\tCtrl+Enter");
    AppendMenuW(m, actFlag, Id::ID_REMOTE_PUSH_POS_ALL,
                L"…on every connected instance\tCtrl+Shift+Enter");
    AppendMenuW(m, actFlag, Id::ID_REMOTE_STREAM_OUT,
                L"Show my picture there\tAlt+Enter");
    AppendMenuW(m, actFlag, Id::ID_REMOTE_STREAM_IN,
                L"Show its picture here\tCtrl+Alt+Enter");

    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

    // Disabled context line, not a clickable item: this is the state the two
    // ticks above cannot show — WHO is being driven. Without it "Mirror" ticked
    // with nothing connected reads as working.
    const std::wstring status =
        live == 0 ? std::wstring(L"Nothing connected — see Remote Servers (F10)")
                  : (L"Driving " + std::to_wstring(driven) + L" of " +
                     std::to_wstring(live) + L" connected");
    AppendMenuW(m, MF_STRING | MF_DISABLED, 0, status.c_str());

    return m;
}

// Everything TCP/IP, in one place. The main menu used to carry six of these as
// top-level rows plus a submenu, which is most of what made it long — and they
// are one subject that most users never touch at all.
//
// Order is the order you meet them: what THIS instance offers, who is on it,
// what it can reach, which of those it drives, then the tools for driving them.
static HMENU BuildTcpIpMenu() {
    HMENU m = CreatePopupMenu();

    // TWO ROLES, AND THE LABELS SAY WHICH. This viewer is a SERVER to the things
    // that dial in (the first pair), and a CLIENT of the servers it dials out to
    // (the second pair). Naming them for the action instead — "Remote Control"
    // for a checkbox list — is what made two panels at opposite ends of two
    // different connections read as views of the same one.
    // FIRST, above the server it advertises. It is the step that decides whether
    // anyone can FIND this machine, so it is the first question rather than a
    // footnote under the panels — and CHECKABLE, because the tick is the only
    // resting indication that this PC is announcing itself. "Am I visible on the
    // network" should be answerable by looking, not by trying.
    //
    // Shown whether the server is running or not. Greying it while stopped would
    // make the setting unreachable exactly when somebody is setting the server
    // up, and the tick means "announce when running", not "announcing now" —
    // Beacon::Refresh reconciles the two.
    AppendMenuW(m, MF_STRING | CheckFlag(app.remoteBeacon),
                Id::ID_REMOTE_BEACON, L"\U0001F5A7 Announce (beacon)");

    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(m, MF_STRING, Id::ID_REMOTE_PANEL,    L"Local Server\tF9");
    AppendMenuW(m, MF_STRING, Id::ID_REMOTE_CLIENTS,  L"My Clients\tCtrl+F9");

    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, Id::ID_REMOTES_CONSOLE, L"Remote Servers\tF10");
    // "Mirroring" rather than "Remote Control": F11 is already the mirroring
    // key, and this panel picks which servers it reaches. Panel and key now
    // share a name instead of describing each other.
    AppendMenuW(m, MF_STRING, Id::ID_REMOTES_CONTROL, L"Mirroring\tCtrl+F11");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, Id::ID_REMOTE_CMD,      L"Remote Commands\tCtrl+F10");
    AppendMenuW(m, MF_STRING, Id::ID_REMOTE_LOG,      L"Server Log\tCtrl+F12");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_POPUP,
                reinterpret_cast<UINT_PTR>(BuildRemoteBindingsMenu()),
                L"Remote Bindings");

    return m;
}

static HMENU BuildWallpaperMenu() {
    HMENU m = CreatePopupMenu();
    for (int i = 0; i < Constants::Wallpaper::COUNT; ++i)
        AppendMenuW(m, MF_STRING, Id::ID_WALLPAPER_FIRST + i,
                    Constants::Messages::WALLPAPER_NAMES[i]);
    return m;
}

static HMENU BuildBackupMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, Id::SET_BACKUP,         L"Backup History && Favorites");
    AppendMenuW(m, MF_STRING, Id::SET_RESTORE_BACKUP, L"Restore History && Favorites");
    return m;
}

} // namespace detail

// =============================================================================
// Build — THE menu. Both the tray icon and the main-window right-click show
// exactly this, so behaviour can never diverge between the two.
// =============================================================================
HMENU Build(HWND hWnd) {
    using namespace UI::AppMenu::detail;

    HMENU m = CreatePopupMenu();
    if (!m) return nullptr;

    AppendMenuW(m, MF_STRING, Id::ID_HISTORY,     L"History\tTab");
    AppendMenuW(m, MF_STRING, Id::ID_BROWSE,      L"Browse…\tF2");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, Id::ID_PREV_FOLDER, L"Previous Folder\tQ");
    AppendMenuW(m, MF_STRING, Id::ID_PREV_IMAGE,  L"Previous Image\tE");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, Id::ID_THUMB_STRIP, L"Thumbnail Strip\tF6");
    AppendMenuW(m, MF_STRING, Id::ID_STATS,           L"Statistics\tK");
    AppendMenuW(m, MF_STRING, Id::ID_METADATA,        L"Metadata\tM");
    AppendMenuW(m, MF_STRING, Id::ID_DEDICATED_PANEL, L"Dedicated\tF8");
    // ONE row for the whole subject. Six top-level rows and a submenu became a
    // submenu — the remote features are one thing, and a viewer that never
    // touches them should not read past them to reach Help.
    AppendMenuW(m, MF_POPUP,
                reinterpret_cast<UINT_PTR>(BuildTcpIpMenu()), L"TCP / IP");
    // Help
    const std::wstring help = std::wstring(L"Help v") + Constants::APP_VERSION + L"\tF1";
    AppendMenuW(m, MF_STRING, Id::ID_HELP, help.c_str());

    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, Id::ID_COPY,        L"Copy\tCtrl+C");
    AppendMenuW(m, MF_STRING, Id::ID_SAVE_AS,     L"Save As…\tCtrl+S");
    AppendMenuW(m, MF_STRING, Id::ID_EXPLORER,    L"Open File in Explorer\tL");
    AppendMenuW(m, MF_STRING, Id::ID_NEXT_MONITOR, L"Move to Next Monitor\tCtrl+M");
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(BuildWallpaperMenu()), L"Set as Desktop Wallpaper");
    //Group
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(BuildSortMenu()),      L"Sort");
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(BuildViewModeMenu()),  L"View Mode");
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(BuildSlideshowMenu()), L"Slideshow");
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(BuildSettingsMenu()),  L"Settings");
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(BuildOverlaysMenu()),  L"Overlays");
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(BuildBackupMenu()),    L"Backup");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    // Only meaningful from the tray, when the window is not on screen.
    if (!IsWindowVisible(hWnd))
        AppendMenuW(m, MF_STRING, Id::SET_RESTORE_WINDOW, L"Restore QuickImageViewer");
    AppendMenuW(m, MF_STRING, Id::ID_CLOSE_APP,      L"Close App\tEsc");
    AppendMenuW(m, MF_STRING, Id::ID_CLOSE_PANELS,   L"Close All Panels\tN");
    AppendMenuW(m, MF_STRING, Id::ID_RESTORE_PANELS, L"Restore All Panels\tN");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, Id::ID_HARD_QUIT,      L"Hard Quit\tCtrl+Q");

    return m;
}

} // namespace UI::AppMenu
