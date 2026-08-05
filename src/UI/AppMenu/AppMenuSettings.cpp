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
#include "Common/Converters.h"
#include "Dedicated/DedicatedSettings.h" // SettingsUseFile / SettingsFilePath
#include "Input/AppCommands.h"
#include "Overlays/OverlayManager.h"
#include "Persistence/RegistryManager.h"
#include "Platform/Constants.h"
#include "Platform/ConstantsStrings.h"
#include "Platform/FileHandler.h"
#include "UI/FloatingPanels/HistoryListWnd.h"
#include "UI/ThemedDialog.h"
#include "UI/UIManager.h"

#include <commdlg.h>     // ChooseColorW — overlay font colour picker
#include <shlobj_core.h> // ILCreateFromPathW / SHOpenFolderAndSelectItems
#include <string>

extern AppState app;
extern OverlayManager g_overlayManager;

// =============================================================================
// AppMenuSettings — everything below VIEWER_BASE.
//
// Toggles, numeric prompts, overlay slot state and sort order: the items whose
// effect is to change a setting rather than to perform a viewer action. Actions
// live on the other side of the id space and go through
// InputManager::ExecuteCommand instead, so the menu and the keyboard cannot
// diverge.
//
// The file-dialog items (export, import, restore defaults, backup, restore
// backup) are a different job again — each opens a shell dialog, touches the
// filesystem and reports an outcome — so they live in AppMenuIO.cpp and are
// called from the switch below.
//
// This code used to be TrayHandler::DispatchCommand, where it was 93% of a file
// named after the tray. It was never about the tray: the tray and the
// right-click menu are the same menu, and this is what half of that menu does.
// =============================================================================

namespace UI::AppMenu::detail {

namespace Id = UI::AppMenu::Ids;

void DispatchSetting(HWND hWnd, int cmd) {
    // ── Overlay slot state and layout ───────────────────────────────────────
    // Handled before the switch so the three contiguous 9-wide runs resolve by
    // arithmetic instead of thirty explicit cases. The bounds and the adjacency
    // this relies on are asserted in AppMenuIds.h, so a renumbering that would
    // break the decode fails the build rather than silently misrouting a click.
    if (cmd >= Id::OVERLAY_BAND_FIRST && cmd <= Id::OVERLAY_BAND_LAST) {
        if (cmd <= Id::SET_OVERLAY_COMPACT_BASE + Id::OVERLAY_SLOT_COUNT - 1) {
            // The three state bands set a slot directly rather than cycling —
            // a radio item names the state it wants. SlotStateMessage then
            // reports whatever the slot actually ended up in, so the menu and
            // Ctrl+1..9 word the result identically.
            const int band = (cmd - Id::SET_OVERLAY_OFF_BASE) / Id::OVERLAY_SLOT_COUNT; // 0 Off, 1 Full, 2 Compact
            const auto s = static_cast<OverlayManager::Slot>(
                (cmd - Id::SET_OVERLAY_OFF_BASE) % Id::OVERLAY_SLOT_COUNT);
            const bool wantVisible = band != 0;
            const bool wantCompact = band == 2;

            g_overlayManager.SetSlotVisible(s, wantVisible);
            if (wantVisible && s != OverlayManager::MID_CENTER &&
                g_overlayManager.IsCompact(s) != wantCompact)
                g_overlayManager.ToggleCompactMode(s);

            g_overlayManager.PostCenterMessage(hWnd, g_overlayManager.SlotStateMessage(s));
        } else {
            // Past the slot runs, so this is a layout mode: Grid / Stacked /
            // Summary, in that order.
            const int mode = cmd - Id::SET_LAYOUT_GRID;
            app.overlayLayoutMode = mode;
            g_overlayManager.OnLayoutModeChanged(hWnd);
            static const wchar_t* const LAYOUT_MSGS[] = {
                Constants::Messages::LAYOUT_GRID,
                Constants::Messages::LAYOUT_STACKED,
                Constants::Messages::LAYOUT_SUMMARY
            };
            g_overlayManager.PostCenterMessage(hWnd, LAYOUT_MSGS[mode]);
        }
        InvalidateRect(hWnd, nullptr, FALSE);
        return;
    }

    // ── Overlay font family (contiguous band) ───────────────────────────────
    if (cmd >= Id::SET_OVERLAY_FONT_FAMILY_BASE &&
        cmd <  Id::SET_OVERLAY_FONT_FAMILY_BASE + Constants::Overlay::OVERLAY_FONT_FAMILY_COUNT) {
        app.overlayFontFamily = cmd - Id::SET_OVERLAY_FONT_FAMILY_BASE;
        Persistence::Registry::SaveSetting(Constants::Registry::OVERLAY_FONT_FAMILY,
            static_cast<DWORD>(app.overlayFontFamily));
        // Rebuilds the base format and every per-slot format derived from it.
        g_overlayManager.UpdateTextFormat();
        g_overlayManager.InvalidateLayouts();
        g_overlayManager.PostCenterMessage(hWnd,
            std::wstring(Constants::Messages::OVERLAY_FONT_PREFIX) +
            Constants::Overlay::OVERLAY_FONT_FAMILIES[app.overlayFontFamily]);
        InvalidateRect(hWnd, nullptr, FALSE);
        return;
    }

    switch (cmd) {

    // ── Restore / Help / Exit ───────────────────────────────────────────────
    case Id::SET_RESTORE_WINDOW:
        ShowWindow(hWnd, SW_SHOW);
        ShowWindow(hWnd, SW_RESTORE);
        SetForegroundWindow(hWnd);
        break;

    case Id::SET_SHOW_HELP:
        ShowWindow(hWnd, SW_SHOW);
        ShowWindow(hWnd, SW_RESTORE);
        SetForegroundWindow(hWnd);
        uiManager.getHelpWindow().Show();
        break;

    case Id::SET_EXIT:
        AppCommands::RemoveTrayIcon(hWnd);
        DestroyWindow(hWnd);
        break;

    // ── Boolean toggles ─────────────────────────────────────────────────────
    case Id::SET_KEEP_IN_BG:
        app.isKeepInBackground = !app.isKeepInBackground;
        Persistence::Registry::SaveSetting(Constants::Registry::KEEP_IN_BACKGROUND,
            static_cast<DWORD>(app.isKeepInBackground));
        g_overlayManager.PostCenterMessage(hWnd,
            app.isKeepInBackground ? Constants::Messages::KEEP_IN_BG_ON
                                   : Constants::Messages::KEEP_IN_BG_OFF);
        break;

    case Id::SET_RUN_ON_STARTUP:
        app.isEnableRunOnStartup = !app.isEnableRunOnStartup;
        Persistence::Registry::SaveSetting(Constants::Registry::RUN_ON_STARTUP,
            static_cast<DWORD>(app.isEnableRunOnStartup));
        Persistence::Registry::EnableRunOnStartup(app.isEnableRunOnStartup);
        g_overlayManager.PostCenterMessage(hWnd,
            app.isEnableRunOnStartup ? Constants::Messages::RUN_ON_STARTUP_ON
                                     : Constants::Messages::RUN_ON_STARTUP_OFF);
        break;

    case Id::SET_THUMB_EFFECTS:
        app.thumbnailEffectsEnabled = !app.thumbnailEffectsEnabled;
        Persistence::Registry::SaveSetting(Constants::Registry::THUMBNAIL_EFFECTS,
            static_cast<DWORD>(app.thumbnailEffectsEnabled));
        g_overlayManager.PostCenterMessage(hWnd,
            app.thumbnailEffectsEnabled ? Constants::Messages::THUMB_EFFECTS_ON
                                        : Constants::Messages::THUMB_EFFECTS_OFF);
        uiManager.RepaintAllPanels();
        break;

    case Id::SET_HISTORY_FULL:
        app.historyFullModeEnabled = !app.historyFullModeEnabled;
        Persistence::Registry::SaveSetting(Constants::Registry::HISTORY_FULL_MODE,
            static_cast<DWORD>(app.historyFullModeEnabled));
        // The History panel reads this flag directly, so an open panel must be
        // rebuilt and refitted now — otherwise it keeps showing the old row set
        // until something unrelated invalidates it.
        UI::RefreshHistoryFullMode();
        g_overlayManager.PostCenterMessage(hWnd,
            app.historyFullModeEnabled ? Constants::Messages::HISTORY_FULL_ON
                                       : Constants::Messages::HISTORY_FULL_OFF);
        break;

    // KIOSK lock. Reached from the tray while the main window is deaf, which is
    // the only way back out — see Constants::IS_KIOSK_LOCK_ENABLED.
    case Id::SET_KIOSK_LOCK:
        app.isLocked = !app.isLocked;
        Persistence::Registry::SaveSetting(Constants::Registry::KIOSK_LOCK,
            static_cast<DWORD>(app.isLocked));
        g_overlayManager.PostCenterMessage(hWnd,
            app.isLocked ? Constants::Messages::KIOSK_LOCK_ON
                         : Constants::Messages::KIOSK_LOCK_OFF);
        break;

    // Mirrors Command::ToggleAlwaysOnTop (Ctrl+T) so both routes behave the same.
    case Id::SET_ALWAYS_ON_TOP:
        app.isAlwaysOnTop = !app.isAlwaysOnTop;
        SetWindowPos(hWnd, app.isAlwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                     0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        uiManager.ApplyAlwaysOnTop(app.isAlwaysOnTop);
        Persistence::Registry::SaveSetting(Constants::Registry::ALWAYS_ON_TOP,
            static_cast<DWORD>(app.isAlwaysOnTop));
        g_overlayManager.PostCenterMessage(hWnd,
            app.isAlwaysOnTop ? Constants::Messages::ALWAYS_ON_TOP_ON
                              : Constants::Messages::ALWAYS_ON_TOP_OFF);
        break;

    // Screensaver / display-sleep hold. ApplyDisplayAwake owns the actual
    // SetThreadExecutionState call — see AppCommands.h.
    case Id::SET_KEEP_AWAKE:
        app.keepDisplayAwake = !app.keepDisplayAwake;
        Persistence::Registry::SaveSetting(Constants::Registry::KEEP_DISPLAY_AWAKE,
            static_cast<DWORD>(app.keepDisplayAwake));
        AppCommands::ApplyDisplayAwake(hWnd);
        g_overlayManager.PostCenterMessage(hWnd,
            app.keepDisplayAwake ? Constants::Messages::KEEP_DISPLAY_AWAKE_ON
                                 : Constants::Messages::KEEP_DISPLAY_AWAKE_OFF);
        break;

    // Viewport lock (Y) — carry zoom + pan across image changes.
    case Id::SET_LOCK_VIEWPORT:
        app.lockViewport = !app.lockViewport;
        Persistence::Registry::SaveSetting(Constants::Registry::LOCK_VIEWPORT,
            static_cast<DWORD>(app.lockViewport));
        g_overlayManager.PostCenterMessage(hWnd,
            app.lockViewport ? Constants::Messages::VIEWPORT_LOCK_ON
                             : Constants::Messages::VIEWPORT_LOCK_OFF);
        break;

    // ── BOT_LEFT readouts ───────────────────────────────────────────────────
    // Session-only by design — no SaveSetting here, unlike the folder name.
    case Id::SET_OVERLAY_EFFECTS_LIST:
        app.overlayShowEffectsList = !app.overlayShowEffectsList;
        g_overlayManager.UpdateEffects();
        g_overlayManager.PostCenterMessage(hWnd,
            app.overlayShowEffectsList ? Constants::Messages::OVERLAY_EFFECTS_LIST_ON
                                       : Constants::Messages::OVERLAY_EFFECTS_LIST_OFF);
        InvalidateRect(hWnd, nullptr, FALSE);
        break;

    case Id::SET_OVERLAY_DIR_NAME:
        app.overlayShowDirName = !app.overlayShowDirName;
        Persistence::Registry::SaveSetting(Constants::Registry::OVERLAY_SHOW_DIR_NAME,
            static_cast<DWORD>(app.overlayShowDirName));
        g_overlayManager.RefreshFolderNameLine();
        g_overlayManager.PostCenterMessage(hWnd,
            app.overlayShowDirName ? Constants::Messages::OVERLAY_DIR_NAME_ON
                                   : Constants::Messages::OVERLAY_DIR_NAME_OFF);
        InvalidateRect(hWnd, nullptr, FALSE);
        break;

    // ── Overlay font size / colour (outer slots only) ───────────────────────
    case Id::SET_OVERLAY_FONT_SIZE: {
        wchar_t prompt[128];
        swprintf_s(prompt, L"Overlay text size in points (%d – %d):",
                   Constants::Overlay::OVERLAY_FONT_SIZE_MIN,
                   Constants::Overlay::OVERLAY_FONT_SIZE_MAX);
        int v = UI::ThemedDialog::PromptInt(hWnd, L"Overlay Font Size", prompt,
            app.overlayFontSize,
            Constants::Overlay::OVERLAY_FONT_SIZE_MIN,
            Constants::Overlay::OVERLAY_FONT_SIZE_MAX,
            Constants::Overlay::OVERLAY_FONT_SIZE_DEFAULT);
        if (v >= 0) {
            app.overlayFontSize = v;
            Persistence::Registry::SaveSetting(Constants::Registry::OVERLAY_FONT_SIZE,
                static_cast<DWORD>(app.overlayFontSize));
            g_overlayManager.UpdateTextFormat();
            g_overlayManager.InvalidateLayouts();
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        break;
    }

    case Id::SET_OVERLAY_FONT_COLOR: {
        // Custom swatches persist for the lifetime of the process only —
        // ChooseColor writes back into whatever array it is given.
        static COLORREF customColors[16] = {};
        CHOOSECOLORW cc{};
        cc.lStructSize  = sizeof(cc);
        cc.hwndOwner    = hWnd;
        cc.rgbResult    = app.overlayFontColor;
        cc.lpCustColors = customColors;
        cc.Flags        = CC_FULLOPEN | CC_RGBINIT;
        if (ChooseColorW(&cc)) {
            app.overlayFontColor = cc.rgbResult;
            Persistence::Registry::SaveSetting(Constants::Registry::OVERLAY_FONT_COLOR,
                static_cast<DWORD>(app.overlayFontColor));
            g_overlayManager.ApplyTextColor();
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        break;
    }

    case Id::SET_OVERLAY_MASTER:
        app.showOverlayInfoText = !app.showOverlayInfoText;
        Persistence::Registry::SaveSetting(Constants::Registry::OVERLAY_VISIBLE,
            static_cast<DWORD>(app.showOverlayInfoText));
        g_overlayManager.SetAllVisible(app.showOverlayInfoText);
        g_overlayManager.PostCenterMessage(hWnd,
            app.showOverlayInfoText ? Constants::Messages::INFO_PANELS_ON
                                    : Constants::Messages::INFO_PANELS_OFF);
        InvalidateRect(hWnd, nullptr, FALSE);
        break;

    case Id::SET_OPEN_DIRWND:
        app.openDirWndOnStart = !app.openDirWndOnStart;
        Persistence::Registry::SaveSetting(Constants::Registry::OPEN_DIRWND_ON_START,
            static_cast<DWORD>(app.openDirWndOnStart));
        g_overlayManager.PostCenterMessage(hWnd,
            app.openDirWndOnStart ? Constants::Messages::OPEN_THUMB_START_ON
                                  : Constants::Messages::OPEN_THUMB_START_OFF);
        break;

    case Id::SET_OVERLAY_BG:
        app.overlayShowBackground = !app.overlayShowBackground;
        Persistence::Registry::SaveSetting(Constants::Registry::OVERLAY_SHOW_BG,
            static_cast<DWORD>(app.overlayShowBackground));
        g_overlayManager.PostCenterMessage(hWnd,
            app.overlayShowBackground ? Constants::Messages::OVERLAY_BG_ON
                                      : Constants::Messages::OVERLAY_BG_OFF);
        InvalidateRect(hWnd, nullptr, FALSE);
        break;

    case Id::SET_SWAP_MOUSE:
        app.swapMouseButtons = !app.swapMouseButtons;
        Persistence::Registry::SaveSetting(Constants::Registry::SWAP_MOUSE_BUTTONS,
            static_cast<DWORD>(app.swapMouseButtons));
        g_overlayManager.PostCenterMessage(hWnd,
            app.swapMouseButtons ? Constants::Messages::SWAP_MOUSE_ON
                                 : Constants::Messages::SWAP_MOUSE_OFF);
        break;

    case Id::SET_WHEEL_INVERT:
        app.invertWheelDirection = !app.invertWheelDirection;
        Persistence::Registry::SaveSetting(Constants::Registry::WHEEL_INVERT,
            static_cast<DWORD>(app.invertWheelDirection));
        g_overlayManager.PostCenterMessage(hWnd,
            app.invertWheelDirection ? Constants::Messages::WHEEL_INVERT_ON
                                     : Constants::Messages::WHEEL_INVERT_OFF);
        break;

    case Id::SET_WHEEL_INVERT_H:
        app.invertWheelDirectionH = !app.invertWheelDirectionH;
        Persistence::Registry::SaveSetting(Constants::Registry::WHEEL_INVERT_H,
            static_cast<DWORD>(app.invertWheelDirectionH));
        g_overlayManager.PostCenterMessage(hWnd,
            app.invertWheelDirectionH ? Constants::Messages::WHEEL_INVERT_H_ON
                                      : Constants::Messages::WHEEL_INVERT_H_OFF);
        break;

    case Id::SET_START_FULLSCREEN:
        app.startInFullscreen = !app.startInFullscreen;
        Persistence::Registry::SaveSetting(Constants::Registry::START_FULLSCREEN,
            static_cast<DWORD>(app.startInFullscreen));
        g_overlayManager.PostCenterMessage(hWnd,
            app.startInFullscreen ? Constants::Messages::START_FULLSCREEN_ON
                                  : Constants::Messages::START_FULLSCREEN_OFF);
        break;

    case Id::SET_CTRL_C:
        app.ctrlCEnabled = !app.ctrlCEnabled;
        Persistence::Registry::SaveSetting(Constants::Registry::CTRL_C_ENABLED,
            static_cast<DWORD>(app.ctrlCEnabled));
        g_overlayManager.PostCenterMessage(hWnd,
            app.ctrlCEnabled ? Constants::Messages::CTRL_C_COPY_ON
                             : Constants::Messages::CTRL_C_COPY_OFF);
        break;

    case Id::SET_CONTEXT_MENU:
        app.contextMenuEnabled = !app.contextMenuEnabled;
        Persistence::Registry::SaveSetting(Constants::Registry::CONTEXT_MENU_ENABLED,
            static_cast<DWORD>(app.contextMenuEnabled));
        g_overlayManager.PostCenterMessage(hWnd,
            app.contextMenuEnabled ? L"Right-Click Menu: On" : L"Right-Click Menu: Off");
        break;

    case Id::SET_CARET_BAR:
    case Id::SET_CARET_UNDERSCORE: {
        const int newStyle = (cmd == Id::SET_CARET_BAR) ? 0 : 1;
        if (app.caretStyle != newStyle) {
            app.caretStyle = newStyle;
            Persistence::Registry::SaveSetting(Constants::Registry::INPUTBOX_CARET_STYLE,
                static_cast<DWORD>(app.caretStyle));
            g_overlayManager.PostCenterMessage(hWnd,
                newStyle == 0 ? L"Input Caret: Bar" : L"Input Caret: Underscore");
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        break;
    }

    case Id::SET_THUMB_COPY:
        app.thumbCopyEnabled = !app.thumbCopyEnabled;
        Persistence::Registry::SaveSetting(Constants::Registry::THUMB_COPY_ENABLED,
            static_cast<DWORD>(app.thumbCopyEnabled));
        g_overlayManager.PostCenterMessage(hWnd,
            app.thumbCopyEnabled ? Constants::Messages::THUMB_COPY_OP_ON
                                 : Constants::Messages::THUMB_COPY_OP_OFF);
        break;

    case Id::SET_THUMB_MOVE:
        app.thumbMoveEnabled = !app.thumbMoveEnabled;
        Persistence::Registry::SaveSetting(Constants::Registry::THUMB_MOVE_ENABLED,
            static_cast<DWORD>(app.thumbMoveEnabled));
        g_overlayManager.PostCenterMessage(hWnd,
            app.thumbMoveEnabled ? Constants::Messages::THUMB_MOVE_OP_ON
                                 : Constants::Messages::THUMB_MOVE_OP_OFF);
        break;

    case Id::SET_THUMB_DELETE:
        app.thumbDeleteEnabled = !app.thumbDeleteEnabled;
        Persistence::Registry::SaveSetting(Constants::Registry::THUMB_DELETE_ENABLED,
            static_cast<DWORD>(app.thumbDeleteEnabled));
        g_overlayManager.PostCenterMessage(hWnd,
            app.thumbDeleteEnabled ? Constants::Messages::THUMB_DELETE_OP_ON
                                   : Constants::Messages::THUMB_DELETE_OP_OFF);
        break;

    case Id::SET_THUMB_PASTE:
        app.thumbPasteEnabled = !app.thumbPasteEnabled;
        Persistence::Registry::SaveSetting(Constants::Registry::THUMB_PASTE_ENABLED,
            static_cast<DWORD>(app.thumbPasteEnabled));
        g_overlayManager.PostCenterMessage(hWnd,
            app.thumbPasteEnabled ? Constants::Messages::THUMB_PASTE_OP_ON
                                  : Constants::Messages::THUMB_PASTE_OP_OFF);
        break;

    // ── Numeric prompts ─────────────────────────────────────────────────────
    case Id::SET_ZOOM_CLICK: {
        wchar_t prompt[128];
        swprintf_s(prompt, L"Left-click zoom multiplier (%.2f = off .. %.2f):",
                   Constants::ZOOM_CLICK_MIN, Constants::ZOOM_CLICK_MAX);
        int v = UI::ThemedDialog::PromptFloat(hWnd, L"Left-Click Zoom",
            prompt,
            app.zoomClickMultiplier, Constants::ZOOM_CLICK_MIN, Constants::ZOOM_CLICK_MAX,
            Constants::ZOOM_CLICK);
        if (v >= 0) {
            app.zoomClickMultiplier = Converters::toZoomFloat(v);
            Persistence::Registry::SaveSetting(Constants::Registry::ZOOM_CLICK_MULT,
                static_cast<DWORD>(v));
        }
        break;
    }
    case Id::SET_VRAM_CACHE: {
        int v = UI::ThemedDialog::PromptInt(hWnd, L"VRAM Image Cache",
            L"Number of images to cache in VRAM (0 – 999):",
            app.vramCacheCount, 0, 999, Constants::IS_VRAM_CACHE_IMAGES_COUNT);
        if (v >= 0) {
            app.vramCacheCount = v;
            Persistence::Registry::SaveSetting(Constants::Registry::VRAM_CACHE_COUNT,
                static_cast<DWORD>(app.vramCacheCount));
        }
        break;
    }
    case Id::SET_WINDOW_WIDTH: {
        int v = UI::ThemedDialog::PromptInt(hWnd, L"Window Width",
            L"Default window width in pixels (240 – 16000):",
            app.baseWidth, 240, 16000, Constants::IS_BASE_WIDTH);
        if (v >= 0) {
            app.baseWidth = v;
            Persistence::Registry::SaveSetting(Constants::Registry::BASE_WIDTH_KEY,
                static_cast<DWORD>(app.baseWidth));
        }
        break;
    }
    case Id::SET_WINDOW_HEIGHT: {
        int v = UI::ThemedDialog::PromptInt(hWnd, L"Window Height",
            L"Default window height in pixels (240 – 16000):",
            app.baseHeight, 240, 16000, Constants::IS_BASE_HEIGHT);
        if (v >= 0) {
            app.baseHeight = v;
            Persistence::Registry::SaveSetting(Constants::Registry::BASE_HEIGHT_KEY,
                static_cast<DWORD>(app.baseHeight));
        }
        break;
    }
    case Id::SET_HISTORY_MAX_DIRS: {
        int v = UI::ThemedDialog::PromptInt(hWnd, L"History Max Dirs",
            L"Maximum history folders to show (0 – 999):",
            app.historyMaxDirs, 0, 999,
            Constants::History::IS_HISTORY_MAX_DIRS_TO_SHOW);
        if (v >= 0) {
            app.historyMaxDirs = v;
            Persistence::Registry::SaveSetting(Constants::Registry::HISTORY_MAX_DIRS,
                static_cast<DWORD>(app.historyMaxDirs));
        }
        break;
    }
    case Id::SET_HISTORY_MAX_FAVS: {
        int v = UI::ThemedDialog::PromptInt(hWnd, L"History Max Favs",
            L"Maximum favorite folders to show (0 – 999):",
            app.historyMaxFavs, 0, 999,
            Constants::History::IS_HISTORY_MAX_FAVORITES_TO_SHOW);
        if (v >= 0) {
            app.historyMaxFavs = v;
            Persistence::Registry::SaveSetting(Constants::Registry::HISTORY_MAX_FAVS,
                static_cast<DWORD>(app.historyMaxFavs));
        }
        break;
    }
    case Id::SET_DIR_THUMB_CACHE: {
        int v = UI::ThemedDialog::PromptInt(hWnd, L"Dir Thumb Cache Budget",
            L"Thumbnail cache budget in MB (100 – 64000):",
            app.dirThumbCacheMB, 100, 64000,
            Constants::IS_DIR_THUMB_CACHE_BUDGET_MB);
        if (v >= 0) {
            app.dirThumbCacheMB = v;
            Persistence::Registry::SaveSetting(Constants::Registry::DIR_THUMB_CACHE_MB,
                static_cast<DWORD>(app.dirThumbCacheMB));
        }
        break;
    }
    case Id::SET_PRELOAD_LOOKASIDE: {
        int v = UI::ThemedDialog::PromptInt(hWnd, L"Preload Lookaside",
            L"Images to preload ahead and behind (1 – 99):",
            app.preloadLookaside, 1, 99,
            Constants::IS_PRELOAD_LOOKASIDE_COUNT);
        if (v >= 0) {
            app.preloadLookaside = v;
            Persistence::Registry::SaveSetting(Constants::Registry::PRELOAD_LOOKASIDE,
                static_cast<DWORD>(app.preloadLookaside));
        }
        break;
    }
    case Id::SET_MSG_DURATION: {
        int v = UI::ThemedDialog::PromptInt(hWnd, L"Overlay Message Duration",
            L"How long center messages are shown in ms (250 – 10000):",
            app.msgCenterDisplayMs, 250, 10000,
            static_cast<int>(Constants::Overlay::IS_MSG_CENTER_DISPLAY_MS));
        if (v >= 0) {
            app.msgCenterDisplayMs = v;
            Persistence::Registry::SaveSetting(Constants::Registry::MSG_CENTER_MS,
                static_cast<DWORD>(app.msgCenterDisplayMs));
        }
        break;
    }
    case Id::SET_HISTORY_SAVE_MAX: {
        int v = UI::ThemedDialog::PromptInt(hWnd, L"History Save Limit",
            L"Maximum folders to remember on disk (1 – 99999):",
            app.historyMaxDirsSave, 1, 99999,
            Constants::History::IS_HISTORY_MAX_DIRS_TO_SAVE);
        if (v >= 0) {
            app.historyMaxDirsSave = v;
            Persistence::Registry::SaveSetting(Constants::Registry::HISTORY_MAX_DIRS_SAVE,
                static_cast<DWORD>(app.historyMaxDirsSave));
        }
        break;
    }

    // Open wherever settings actually live: Explorer with the .ini selected, or
    // regedit at qIV's key.
    //
    // Regedit is driven through its LastKey value, which is the only way to make
    // it open somewhere specific — it has no command line for a key. HKCU, so no
    // elevation is involved and nothing else is disturbed.
    case Id::SET_LOCATION: {
        if (Dedicated::SettingsUseFile()) {
            const std::wstring &path = Dedicated::SettingsFilePath();
            if (path.empty()) break;
            if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str())) {
                SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
                ILFree(pidl);
            }
        } else {
            HKEY k = nullptr;
            if (RegCreateKeyExW(HKEY_CURRENT_USER,
                                L"Software\\Microsoft\\Windows\\CurrentVersion\\Applets\\Regedit",
                                0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
                const std::wstring last =
                    std::wstring(L"Computer\\HKEY_CURRENT_USER\\") + Constants::Registry::ROOT_KEY;
                RegSetValueExW(k, L"LastKey", 0, REG_SZ,
                               reinterpret_cast<const BYTE *>(last.c_str()),
                               static_cast<DWORD>((last.size() + 1) * sizeof(wchar_t)));
                RegCloseKey(k);
            }
            ShellExecuteW(nullptr, L"open", L"regedit.exe", nullptr, nullptr, SW_SHOWNORMAL);
        }
        break;
    }

    // ── File-dialog actions — see AppMenuIO.cpp ─────────────────────────────
    case Id::SET_EXPORT:           ExportSettings(hWnd);            break;
    case Id::SET_IMPORT:           ImportSettings(hWnd);            break;
    case Id::SET_RESTORE_DEFAULTS: RestoreDefaults(hWnd);           break;
    case Id::SET_BACKUP:           BackupHistoryAndFavorites(hWnd); break;
    case Id::SET_RESTORE_BACKUP:   RestoreHistoryAndFavorites(hWnd);break;

    default:
        // NOTE: slideshow controls (start/stop, interval, loop, shuffle,
        // transition type and transition mode) and the VIEW MODE picks live in
        // the viewer id space and run through InputManager::ExecuteCommand —
        // see AppMenuBuilders.cpp. Neither is dispatched here.
        //
        // View mode moved there because the settings copy only assigned
        // app.viewMode and saved it, while the command also re-clamps the pan
        // offset. Picking "Fit to View" from the menu after panning in Original
        // Size therefore left the image off-centre, where the 1 key centred it.

        // ── Sort ────────────────────────────────────────────────────────────
        if (cmd >= Id::SET_SORT_FIRST && cmd <= Id::SET_SORT_LAST) {
            app.fileHandlerDefaultSortOrder   = cmd - Id::SET_SORT_FIRST;
            app.fileHandlerIsReverseSortOrder = false;
            Persistence::Registry::SaveSetting(Constants::Registry::SORT_ORDER,
                static_cast<DWORD>(app.fileHandlerDefaultSortOrder));
            Persistence::Registry::SaveSetting(Constants::Registry::SORT_REVERSE, 0u);
            ReSortPlaylistAndRebuildMap(hWnd);
        }
        else if (cmd == Id::SET_SORT_REVERSE) {
            app.fileHandlerIsReverseSortOrder = !app.fileHandlerIsReverseSortOrder;
            Persistence::Registry::SaveSetting(Constants::Registry::SORT_REVERSE,
                static_cast<DWORD>(app.fileHandlerIsReverseSortOrder));
            ReSortPlaylistAndRebuildMap(hWnd);
        }
        break;

    } // switch
}

} // namespace UI::AppMenu::detail
