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
#include "Input/AppCommands.h"
#include "Overlays/OverlayManager.h"
#include "Persistence/HistoryFoldersManager.h"
#include "Persistence/IniFile.h"    // PathBesideExe — locates the logs\ tree
#include "Persistence/RegistryManager.h"
#include "Rem_TCP_IP/RemoteLog.h"   // the file sink has to be told what an import changed
#include "Platform/AppLog.h"        // and so does the General one
#include "Platform/Constants.h"
#include "Platform/ConstantsStrings.h"
#include "Platform/FileHandler.h"
#include "SlideshowTransitions.h"
#include "UI/FloatingPanels/HistoryListWnd.h"
#include "UI/ThemedDialog.h"
#include "UI/UIManager.h"

#include <algorithm>
#include <fstream>
#include <miniz.h>
#include <shobjidl.h>
#include <string>
#include <vector>

extern AppState app;
extern OverlayManager g_overlayManager;

// =============================================================================
// AppMenuIO — the menu items that open a file dialog and touch the disk.
//
// Separated from AppMenuSettings.cpp because they are a different kind of work:
// a toggle flips a field and reports it, while each of these runs a shell
// dialog, reads or writes a file, and has to say whether it worked. Mixing them
// is what made the original 1,145-line switch hard to read — two thirds of its
// length was five items.
//
// A note on what these actually round-trip:
//   Export / Import      the REGISTRY settings, as an .ini of key=value.
//   Backup / Restore     history and favorites, as a .zip.
//   Restore Defaults     every persisted setting, back to Constants.
// Distinct from the cmdArgs file (CMDArgs.h), which emits a COMMAND LINE rather
// than settings — "how you launch it a particular way" versus "how it persists
// itself".
// =============================================================================

namespace UI::AppMenu::detail {

namespace {
    // Both dialogs below want the same thing: run a shell picker, hand back the
    // chosen path or an empty string. Written once rather than twice, which is
    // how the original acquired four near-identical 25-line blocks.
    std::wstring PickFile(HWND hWnd, bool save,
                          const COMDLG_FILTERSPEC *filters, UINT filterCount,
                          const wchar_t *defaultExt, const wchar_t *defaultName) {
        std::wstring result;
        IFileDialog *pfd = nullptr;
        const CLSID clsid = save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog;
        if (FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&pfd))) || !pfd)
            return result;

        pfd->SetFileTypes(filterCount, filters);
        if (defaultExt)  pfd->SetDefaultExtension(defaultExt);
        if (defaultName) pfd->SetFileName(defaultName);
        if (!save) {
            DWORD opts = 0;
            pfd->GetOptions(&opts);
            pfd->SetOptions(opts | FOS_FILEMUSTEXIST);
        }

        if (SUCCEEDED(pfd->Show(hWnd))) {
            IShellItem *psi = nullptr;
            if (SUCCEEDED(pfd->GetResult(&psi)) && psi) {
                PWSTR path = nullptr;
                if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    result = path;
                    CoTaskMemFree(path);
                }
                psi->Release();
            }
        }
        pfd->Release();
        return result;
    }

    std::string ToUtf8(const wchar_t *ws) {
        // `n` INCLUDES the terminator because the length is -1, so the string is
        // sized n-1 — which turns into std::string(SIZE_MAX) the moment the call
        // returns 0, and that is a length_error rather than an empty result.
        // Today every caller passes a compile-time constant, so this cannot fire;
        // it is one line and the next caller will not necessarily be one.
        if (!ws) return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
        if (n <= 1) return {};
        std::string s(static_cast<size_t>(n - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, ws, -1, s.data(), n, nullptr, nullptr);
        return s;
    }

    constexpr COMDLG_FILTERSPEC INI_FILTERS[] = {
        {L"INI Settings", L"*.ini"},
        {L"All Files",    L"*.*" }
    };
    constexpr COMDLG_FILTERSPEC ZIP_FILTERS[] = {
        {L"ZIP Archive", L"*.zip"},
        {L"All Files",   L"*.*" }
    };
}

// =============================================================================
// Export Settings
// =============================================================================
void ExportSettings(HWND hWnd) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t defaultName[MAX_PATH];
    swprintf_s(defaultName, L"%s%04d%02d%02d%s",
               Constants::SettingsFile::EXPORT_PREFIX,
               st.wYear, st.wMonth, st.wDay,
               Constants::SettingsFile::EXPORT_EXTENSION);

    const std::wstring exportPath =
        PickFile(hWnd, true, INI_FILTERS, ARRAYSIZE(INI_FILTERS), L"ini", defaultName);
    if (exportPath.empty()) return;

    FILE *f = nullptr;
    if (_wfopen_s(&f, exportPath.c_str(), L"w,ccs=UTF-8") == 0 && f) {
        fwprintf(f, L"[QuickImageViewer]\n");
        // The key list lives in Persistence::Registry::ForEachSetting, so this
        // and the remote panel's .ini seeding cannot fall out of step.
        // Everything persists as an unsigned DWORD, which is exactly how the
        // registry holds it — no value is reinterpreted on the way out.
        Persistence::Registry::ForEachSetting(
            app,
            [](const wchar_t *key, DWORD value, void *ctx) {
                fwprintf(static_cast<FILE *>(ctx), L"%s=%lu\n", key,
                         static_cast<unsigned long>(value));
            },
            f);
        fclose(f);
        UI::ThemedDialog::Message(hWnd, L"Settings exported successfully.", L"Export Settings");
    } else {
        UI::ThemedDialog::Message(hWnd, L"Failed to write the settings file.", L"Export Settings");
    }
}

// =============================================================================
// Import Settings
//
// Every value is CLAMPED on the way in. The file is hand-editable text, so a
// number outside its range is expected rather than exceptional — and an
// unclamped one would be written straight back to the registry, making a typo
// permanent.
// =============================================================================
void ImportSettings(HWND hWnd) {
    const std::wstring importPath =
        PickFile(hWnd, false, INI_FILTERS, ARRAYSIZE(INI_FILTERS), nullptr, nullptr);
    if (importPath.empty()) return;

    if (!UI::ThemedDialog::Confirm(hWnd,
            L"Importing will overwrite all current settings. Continue?",
            L"Import Settings"))
        return;

    FILE *f = nullptr;
    if (_wfopen_s(&f, importPath.c_str(), L"r,ccs=UTF-8") != 0 || !f) {
        UI::ThemedDialog::Message(hWnd, L"Failed to read the settings file.", L"Import Settings");
        return;
    }

    wchar_t line[512];
    bool anyKey = false;
    while (fgetws(line, 512, f)) {
        size_t len = wcslen(line);
        while (len > 0 && (line[len - 1] == L'\n' || line[len - 1] == L'\r'))
            line[--len] = L'\0';
        if (len == 0 || line[0] == L'[') continue;
        wchar_t *eq = wcschr(line, L'=');
        if (!eq) continue;
        *eq = L'\0';
        const wchar_t *key = line;
        int val = _wtoi(eq + 1);
        anyKey  = true;

        auto applyBool = [&](const wchar_t *regKey, bool &field) {
            if (wcscmp(key, regKey) == 0) {
                field = val != 0;
                Persistence::Registry::SaveSetting(regKey, static_cast<DWORD>(field));
            }
        };
        applyBool(Constants::Registry::KEEP_IN_BACKGROUND,   app.isKeepInBackground);
        applyBool(Constants::Registry::RUN_ON_STARTUP,       app.isEnableRunOnStartup);
        applyBool(Constants::Registry::THUMBNAIL_EFFECTS,    app.thumbnailEffectsEnabled);
        applyBool(Constants::Registry::LOCK_VIEWPORT,        app.lockViewport);
        applyBool(Constants::Registry::REMEMBER_WINDOW_POS,  app.rememberWindowPosition);
        applyBool(Constants::Registry::HISTORY_FULL_MODE,    app.historyFullModeEnabled);
        applyBool(Constants::Registry::OVERLAY_VISIBLE,      app.showOverlayInfoText);
        applyBool(Constants::Registry::OPEN_DIRWND_ON_START, app.openDirWndOnStart);
        applyBool(Constants::Registry::FOLDER_WALK_WRAP,     app.folderWalkWrap);
        applyBool(Constants::Registry::OVERLAY_SHOW_BG,      app.overlayShowBackground);
        applyBool(Constants::Registry::OVERLAY_SHOW_DIR_NAME, app.overlayShowDirName);
        applyBool(Constants::Registry::OVERLAY_SHOW_FULL_PATH, app.overlayShowFullPath);
        // Same guard LoadAllSettings applies, for the same reason: an imported
        // file is as hand-editable as the registry. Full path wins — it already
        // contains the folder name.
        if (app.overlayShowFullPath) app.overlayShowDirName = false;
        applyBool(Constants::Registry::OVERLAY_SHOW_EFFECTS,  app.overlayShowEffectsList);
        applyBool(Constants::Registry::HISTORY_ENABLED,       app.historyEnabled);
        applyBool(Constants::Registry::HISTORY_IMAGES_ONLY,   app.historyImagesOnly);
        applyBool(Constants::Registry::SWAP_MOUSE_BUTTONS,   app.swapMouseButtons);
        applyBool(Constants::Registry::CONTEXT_MENU_ENABLED, app.contextMenuEnabled);
        applyBool(Constants::Registry::KIOSK_LOCK,           app.isLocked);
        applyBool(Constants::Registry::ALWAYS_ON_TOP,        app.isAlwaysOnTop);
        applyBool(Constants::Registry::KEEP_DISPLAY_AWAKE,   app.keepDisplayAwake);
        applyBool(Constants::Registry::REMOTE_BEACON,        app.remoteBeacon);
        applyBool(Constants::Registry::REMOTE_LOG_FILE,      app.remoteLogToFile);
        applyBool(Constants::Registry::GENERAL_LOG,          app.generalLog);
        applyBool(Constants::Registry::WHEEL_INVERT,         app.invertWheelDirection);
        applyBool(Constants::Registry::WHEEL_INVERT_H,       app.invertWheelDirectionH);
        applyBool(Constants::Registry::START_FULLSCREEN,     app.startInFullscreen);
        applyBool(Constants::Registry::CTRL_C_ENABLED,       app.ctrlCEnabled);
        applyBool(Constants::Registry::THUMB_COPY_ENABLED,   app.thumbCopyEnabled);
        applyBool(Constants::Registry::THUMB_MOVE_ENABLED,   app.thumbMoveEnabled);
        applyBool(Constants::Registry::THUMB_DELETE_ENABLED, app.thumbDeleteEnabled);
        applyBool(Constants::Registry::THUMB_PASTE_ENABLED,  app.thumbPasteEnabled);
        applyBool(Constants::Registry::SLIDESHOW_LOOP,       app.slideshow.loop);
        applyBool(Constants::Registry::SLIDESHOW_SHUFFLE,    app.slideshow.shuffle);

        if (wcscmp(key, Constants::Registry::VRAM_CACHE_COUNT) == 0) {
            app.vramCacheCount = std::max(0, std::min(999, val));
            Persistence::Registry::SaveSetting(Constants::Registry::VRAM_CACHE_COUNT,
                static_cast<DWORD>(app.vramCacheCount));
        }
        if (wcscmp(key, Constants::Registry::VIEW_MODE) == 0) {
            int m = std::max(1, std::min(5, val));
            app.viewMode = static_cast<Constants::ViewModes::ViewMode>(m);
            Persistence::Registry::SaveSetting(Constants::Registry::VIEW_MODE,
                static_cast<DWORD>(m));
        }
        if (wcscmp(key, Constants::Registry::BASE_WIDTH_KEY) == 0) {
            app.baseWidth = std::max(240, std::min(16000, val));
            Persistence::Registry::SaveSetting(Constants::Registry::BASE_WIDTH_KEY,
                static_cast<DWORD>(app.baseWidth));
        }
        if (wcscmp(key, Constants::Registry::BASE_HEIGHT_KEY) == 0) {
            app.baseHeight = std::max(240, std::min(16000, val));
            Persistence::Registry::SaveSetting(Constants::Registry::BASE_HEIGHT_KEY,
                static_cast<DWORD>(app.baseHeight));
        }
        if (wcscmp(key, Constants::Registry::HISTORY_MAX_DIRS) == 0) {
            app.historyMaxDirs = std::max(0, std::min(999, val));
            Persistence::Registry::SaveSetting(Constants::Registry::HISTORY_MAX_DIRS,
                static_cast<DWORD>(app.historyMaxDirs));
        }
        if (wcscmp(key, Constants::Registry::HISTORY_MAX_FAVS) == 0) {
            app.historyMaxFavs = std::max(0, std::min(999, val));
            Persistence::Registry::SaveSetting(Constants::Registry::HISTORY_MAX_FAVS,
                static_cast<DWORD>(app.historyMaxFavs));
        }
        if (wcscmp(key, Constants::Registry::HISTORY_MAX_FAVS_SHOWN) == 0) {
            app.historyMaxFavsShown = std::max(0, std::min(999, val));
            Persistence::Registry::SaveSetting(Constants::Registry::HISTORY_MAX_FAVS_SHOWN,
                static_cast<DWORD>(app.historyMaxFavsShown));
        }
        if (wcscmp(key, Constants::Registry::DIR_THUMB_CACHE_MB) == 0) {
            // Floor 0, matching RegistryManager and the dialog: 0 is the "off"
            // setting. Clamping to 100 here would have turned an exported "off"
            // back into 100 MB on import, silently — the failure this file's
            // whole clamp column exists to prevent.
            app.dirThumbCacheMB = std::max(Constants::IS_DIR_THUMB_CACHE_MIN_MB,
                std::min(Constants::IS_DIR_THUMB_CACHE_MAX_MB, val));
            Persistence::Registry::SaveSetting(Constants::Registry::DIR_THUMB_CACHE_MB,
                static_cast<DWORD>(app.dirThumbCacheMB));
        }
        if (wcscmp(key, Constants::Registry::PRELOAD_LOOKASIDE) == 0) {
            app.preloadLookaside = std::max(1, std::min(99, val));
            Persistence::Registry::SaveSetting(Constants::Registry::PRELOAD_LOOKASIDE,
                static_cast<DWORD>(app.preloadLookaside));
        }
        if (wcscmp(key, Constants::Registry::MSG_CENTER_MS) == 0) {
            app.msgCenterDisplayMs = std::max(250, std::min(10000, val));
            Persistence::Registry::SaveSetting(Constants::Registry::MSG_CENTER_MS,
                static_cast<DWORD>(app.msgCenterDisplayMs));
        }
        if (wcscmp(key, Constants::Registry::HISTORY_MAX_DIRS_SAVE) == 0) {
            app.historyMaxDirsSave = std::max(1, std::min(99999, val));
            Persistence::Registry::SaveSetting(Constants::Registry::HISTORY_MAX_DIRS_SAVE,
                static_cast<DWORD>(app.historyMaxDirsSave));
        }
        if (wcscmp(key, Constants::Registry::SLIDESHOW_INTERVAL_MS) == 0) {
            // Re-arms a running slideshow rather than only storing the value;
            // it also does the clamping, so the bounds live in one place now.
            AppCommands::applySlideshowInterval(hWnd, val);
            Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_INTERVAL_MS,
                static_cast<DWORD>(app.slideshow.intervalMs));
        }
        if (wcscmp(key, Constants::Registry::SLIDESHOW_TRANSITION) == 0) {
            int t = std::max(0, std::min(Constants::Slideshow::TRANSITION_COUNT - 1, val));
            app.slideshow.transition.type = static_cast<TransitionType>(t);
            Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_TRANSITION,
                static_cast<DWORD>(t));
        }
        if (wcscmp(key, Constants::Registry::SLIDESHOW_TRANS_SOURCE) == 0) {
            int s = std::max(0, std::min(Constants::Slideshow::TransitionSource::COUNT - 1, val));
            app.slideshow.transition.source = s;
            Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_TRANS_SOURCE,
                static_cast<DWORD>(s));
        }
        if (wcscmp(key, Constants::Registry::SLIDESHOW_TRANS_ORDER) == 0) {
            int o = std::max(0, std::min(Constants::Slideshow::TransitionOrder::COUNT - 1, val));
            app.slideshow.transition.order = o;
            Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_TRANS_ORDER,
                static_cast<DWORD>(o));
        }
        if (wcscmp(key, Constants::Registry::OVERLAY_FONT_SIZE) == 0) {
            app.overlayFontSize = std::max(Constants::Overlay::OVERLAY_FONT_SIZE_MIN,
                std::min(Constants::Overlay::OVERLAY_FONT_SIZE_MAX, val));
            Persistence::Registry::SaveSetting(Constants::Registry::OVERLAY_FONT_SIZE,
                static_cast<DWORD>(app.overlayFontSize));
        }
        // A COLORREF tops out at 0x00FFFFFF, so the int `val` already holds
        // it; only the sign needs guarding against a hand-edited file.
        if (wcscmp(key, Constants::Registry::OVERLAY_FONT_COLOR) == 0) {
            app.overlayFontColor = static_cast<COLORREF>(std::max(0, val) & 0x00FFFFFF);
            Persistence::Registry::SaveSetting(Constants::Registry::OVERLAY_FONT_COLOR,
                static_cast<DWORD>(app.overlayFontColor));
        }
        // Clamped — a file written by a build with more families listed
        // would otherwise index past the end of the array.
        if (wcscmp(key, Constants::Registry::OVERLAY_FONT_FAMILY) == 0) {
            app.overlayFontFamily = std::max(0,
                std::min(Constants::Overlay::OVERLAY_FONT_FAMILY_COUNT - 1, val));
            Persistence::Registry::SaveSetting(Constants::Registry::OVERLAY_FONT_FAMILY,
                static_cast<DWORD>(app.overlayFontFamily));
        }
        // BOTH OF THESE WERE MISSING. ForEachSetting has emitted them for a
        // while, so they were written to every exported file and then silently
        // dropped on the way back in — an import that looked like it worked and
        // quietly reverted two settings to whatever the machine already had.
        //
        // The clamps deliberately match RegistryManager::LoadAllSettings. Import
        // writes straight to the registry, so a bound that is looser here would
        // persist a value the loader then has to fix on every launch.
        if (wcscmp(key, Constants::Registry::INPUTBOX_CARET_STYLE) == 0) {
            app.caretStyle = std::max(0, std::min(1, val));
            Persistence::Registry::SaveSetting(Constants::Registry::INPUTBOX_CARET_STYLE,
                static_cast<DWORD>(app.caretStyle));
        }
        // Stored as an integer percent, not as the float it becomes — the file
        // holds what toZoomInt produced, so it is clamped in that same integer
        // domain before being converted back.
        if (wcscmp(key, Constants::Registry::ZOOM_CLICK_MULT) == 0) {
            const int raw = std::max(Converters::toZoomInt(Constants::ZOOM_CLICK_MIN),
                            std::min(Converters::toZoomInt(Constants::ZOOM_CLICK_MAX), val));
            app.zoomClickMultiplier = Converters::toZoomFloat(raw);
            Persistence::Registry::SaveSetting(Constants::Registry::ZOOM_CLICK_MULT,
                static_cast<DWORD>(raw));
        }

        if (wcscmp(key, Constants::Registry::OVERLAY_LAYOUT_MODE) == 0) {
            app.overlayLayoutMode = std::max(0, std::min(
                Constants::Overlay::LAYOUT_MODE_COUNT - 1, val));
            Persistence::Registry::SaveSetting(Constants::Registry::OVERLAY_LAYOUT_MODE,
                static_cast<DWORD>(app.overlayLayoutMode));
        }
        // Masks carry one bit per slot; drop anything above the nine that
        // exist so a hand-edited file cannot set phantom slots.
        if (wcscmp(key, Constants::Registry::OVERLAY_SLOT_VISIBLE) == 0) {
            app.overlaySlotVisibleMask =
                static_cast<unsigned>(val) & Constants::Overlay::SLOT_MASK_ALL;
            Persistence::Registry::SaveSetting(Constants::Registry::OVERLAY_SLOT_VISIBLE,
                static_cast<DWORD>(app.overlaySlotVisibleMask));
        }
        if (wcscmp(key, Constants::Registry::OVERLAY_SLOT_COMPACT) == 0) {
            app.overlaySlotCompactMask =
                static_cast<unsigned>(val) & Constants::Overlay::SLOT_MASK_ALL;
            Persistence::Registry::SaveSetting(Constants::Registry::OVERLAY_SLOT_COMPACT,
                static_cast<DWORD>(app.overlaySlotCompactMask));
        }
        if (wcscmp(key, Constants::Registry::SLIDESHOW_TRANS_LIST) == 0) {
            app.slideshow.transition.listMask = static_cast<uint32_t>(val);
            Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_TRANS_LIST,
                static_cast<DWORD>(app.slideshow.transition.listMask));
        }
        if (wcscmp(key, Constants::Registry::SORT_ORDER) == 0) {
            app.fileHandlerDefaultSortOrder = std::max(0, std::min(4, val));
            Persistence::Registry::SaveSetting(Constants::Registry::SORT_ORDER,
                static_cast<DWORD>(app.fileHandlerDefaultSortOrder));
        }
        if (wcscmp(key, Constants::Registry::SORT_REVERSE) == 0) {
            app.fileHandlerIsReverseSortOrder = val != 0;
            Persistence::Registry::SaveSetting(Constants::Registry::SORT_REVERSE,
                static_cast<DWORD>(app.fileHandlerIsReverseSortOrder));
        }
        if (wcscmp(key, Constants::Registry::THEME_FACTOR) == 0) {
            // Clamp like every other imported numeric key. _wtoi accepts a
            // negative, and the unclamped cast to DWORD stored 4294967295.
            const int pct = std::max(
                static_cast<int>(Constants::Theme::THEME_FACTOR_MIN *
                                 Constants::Theme::THEME_FACTOR_STORE_SCALE),
                std::min(static_cast<int>(Constants::Theme::THEME_FACTOR_MAX *
                                          Constants::Theme::THEME_FACTOR_STORE_SCALE), val));
            app.themeFactor = static_cast<float>(pct) /
                              Constants::Theme::THEME_FACTOR_STORE_SCALE;
            Persistence::Registry::SaveSetting(Constants::Registry::THEME_FACTOR,
                static_cast<DWORD>(pct));
        }
    }
    fclose(f);

    if (!anyKey) {
        UI::ThemedDialog::Message(hWnd,
            L"The file appears to be empty or invalid.", L"Import Settings");
        return;
    }

    Persistence::Registry::LoadAllSettings(app);

    // RECORDED BEFORE THE SINKS BELOW ARE RECONCILED, and that ordering is the
    // point. An imported file can switch the General log OFF — a line written
    // after AppLog::SetEnabled would then be dropped by the very switch it was
    // meant to explain, and the file would simply stop with nothing saying why.
    // Written here, the log that is still running records the event that
    // changed it, which is the last line anybody reading it will need.
    if (AppLog::IsEnabled())
        AppLog::Info(AppLog::COMP_SETTINGS, L"settings imported from " + importPath);

    // Apply side effects
    Persistence::Registry::EnableRunOnStartup(app.isEnableRunOnStartup);
    // An imported value is only a number in AppState until the sink is told —
    // importing a file with this on and having nothing written would be exactly
    // the silent half-application this block exists to prevent.
    Remote::Log::SetFileLogging(app.remoteLogToFile);
    AppLog::SetEnabled(app.generalLog);
    g_overlayManager.SetAllVisible(app.showOverlayInfoText);
    // Per-slot state and layout mode live inside OverlayManager, so the
    // reloaded AppState has to be pushed in — LoadAllSettings only fills
    // the masks, it cannot reach the slots.
    g_overlayManager.ApplyPersistedState(hWnd);
    // Both of these are STATE the window must be pushed into — loading
    // the value alone changes nothing on screen.
    SetWindowPos(hWnd, app.isAlwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    uiManager.ApplyAlwaysOnTop(app.isAlwaysOnTop);
    AppCommands::ApplyDisplayAwake(hWnd);
    AppCommands::changeAppThemeFactor(hWnd, app.themeFactor);
    ReSortPlaylistAndRebuildMap(hWnd);
    uiManager.RepaintAllPanels();
    InvalidateRect(hWnd, nullptr, FALSE);
    UI::ThemedDialog::Message(hWnd, L"Settings imported successfully.", L"Import Settings");
}

// =============================================================================
// Restore Defaults
//
// Assign to `app` from Constants, then write every one back. The values are read
// from `app` on the way out rather than repeating the constants — a literal in
// the save half silently drifts the moment one of the defaults above changes.
// =============================================================================
void RestoreDefaults(HWND hWnd) {
    if (!UI::ThemedDialog::Confirm(hWnd,
            L"All settings will be reset to their default values.\nThis cannot be undone.",
            L"Restore Defaults"))
        return;

    app.isKeepInBackground      = Constants::IS_KEEP_IN_BACKGROUND;
    app.isEnableRunOnStartup    = Constants::IS_ENABLE_RUN_ON_STARTUP;
    app.thumbnailEffectsEnabled = Constants::ThumbnailPanel::ThumbnailEffects::EFFECTS_MASTER_ENABLED;
    app.lockViewport            = Constants::IS_LOCK_VIEWPORT;
    app.rememberWindowPosition  = Constants::IS_REMEMBER_WINDOW_POSITION;
    app.historyFullModeEnabled  = Constants::History::HISTORY_SHOW_FULL_HISTORY;
    app.showOverlayInfoText     = Constants::Overlay::DEFAULT_SHOW_OVERLAY;
    app.openDirWndOnStart       = Constants::IS_OPEN_DIRWND_ON_START;
    app.folderWalkWrap          = Constants::IS_FOLDER_WALK_WRAP;
    app.overlayShowBackground   = Constants::Overlay::IS_OVERLAY_SHOW_BACKGROUND;
    app.overlayLayoutMode       = Constants::Overlay::DEFAULT_LAYOUT_MODE;
    app.overlaySlotVisibleMask  = Constants::Overlay::DEFAULT_SLOT_VISIBLE_MASK;
    app.overlaySlotCompactMask  = Constants::Overlay::DEFAULT_SLOT_COMPACT_MASK;
    app.overlayShowDirName      = Constants::Overlay::SHOW_DIR_NAME;
    app.overlayShowFullPath     = Constants::Overlay::SHOW_FULL_PATH;
    app.historyEnabled          = Constants::History::HISTORY_ENABLED;
    app.historyImagesOnly       = Constants::History::HISTORY_IMAGES_ONLY;
    app.overlayShowEffectsList  = Constants::Overlay::SHOW_EFFECTS_LIST;
    app.overlayFontSize         = Constants::Overlay::OVERLAY_FONT_SIZE_DEFAULT;
    app.overlayFontColor        = Constants::Overlay::OVERLAY_FONT_COLOR_DEFAULT;
    app.overlayFontFamily       = Constants::Overlay::OVERLAY_FONT_FAMILY_DEFAULT;
    app.caretStyle              = Constants::InputBox::CARET_STYLE;
    app.zoomClickMultiplier     = Constants::ZOOM_CLICK;
    app.swapMouseButtons        = Constants::IS_SWAP_MOUSE_BUTTONS;
    app.contextMenuEnabled      = Constants::IS_CONTEXT_MENU_ENABLED;
    app.isLocked                = Constants::IS_KIOSK_LOCK_ENABLED;
    app.isAlwaysOnTop           = Constants::IS_ALWAYS_ON_TOP;
    app.keepDisplayAwake        = Constants::IS_KEEP_DISPLAY_AWAKE;
    app.remoteBeacon            = Constants::IS_REMOTE_BEACON_ENABLED;
    app.remoteLogToFile         = Constants::IS_TCP_IP_LOG;
    app.generalLog              = Constants::IS_GENERAL_LOG;
    app.invertWheelDirection    = Constants::IS_MOUSE_VERTICAL_REVERSE_SCROLL_DIRECTION;
    app.invertWheelDirectionH   = Constants::IS_MOUSE_HORIZONTAL_REVERSE_SCROLL_DIRECTION;
    app.vramCacheCount          = Constants::IS_VRAM_CACHE_IMAGES_COUNT;
    app.viewMode                = Constants::ViewModes::defaultViewMode;
    app.baseWidth               = Constants::IS_BASE_WIDTH;
    app.baseHeight              = Constants::IS_BASE_HEIGHT;
    app.startInFullscreen       = Constants::IS_START_FULLSCREEN;
    app.themeFactor             = Constants::Theme::DEFAULT_THEME_FACTOR;
    app.historyMaxDirs          = Constants::History::IS_HISTORY_MAX_DIRS_TO_SHOW;
    app.historyMaxFavs          = Constants::History::IS_HISTORY_MAX_FAVORITES_TO_SHOW;
    app.historyMaxFavsShown     = Constants::History::IS_HISTORY_MAX_FAVORITES_SHOWN;
    app.dirThumbCacheMB         = Constants::IS_DIR_THUMB_CACHE_BUDGET_MB;
    app.preloadLookaside        = Constants::IS_PRELOAD_LOOKASIDE_COUNT;
    app.msgCenterDisplayMs      = static_cast<int>(Constants::Overlay::IS_MSG_CENTER_DISPLAY_MS);
    app.historyMaxDirsSave      = Constants::History::IS_HISTORY_MAX_DIRS_TO_SAVE;
    // Through the helper like every other interval change: Restore Defaults is
    // reachable from the tray while a slideshow is running, and a plain
    // assignment would leave that show pacing itself by the value the user just
    // discarded.
    AppCommands::applySlideshowInterval(hWnd, Constants::Slideshow::IS_INTERVAL_MS);
    app.slideshow.loop          = Constants::Slideshow::IS_LOOP;
    app.slideshow.shuffle       = Constants::Slideshow::IS_SHUFFLE;
    app.slideshow.transition.type     = TransitionType::Cut;
    app.slideshow.transition.source   = Constants::Slideshow::TransitionSource::NONE;
    app.slideshow.transition.order    = Constants::Slideshow::TransitionOrder::SEQUENTIAL;
    app.slideshow.transition.listMask = Constants::Slideshow::TRANSITION_LIST_DEFAULT_MASK;
    app.slideshow.transition.seqIndex = 0;
    app.fileHandlerDefaultSortOrder   = Constants::FileHandler::FILE_HANDLER_DEFAULT_SORT_ORDER;
    app.fileHandlerIsReverseSortOrder = Constants::FileHandler::FILE_HANDLER_SORT_TYPE_IS_REVERSE;
    app.ctrlCEnabled                  = Constants::IS_CTRL_C_ENABLED;
    app.thumbCopyEnabled              = Constants::IS_THUMB_COPY_ENABLED;
    app.thumbMoveEnabled              = Constants::IS_THUMB_MOVE_ENABLED;
    app.thumbDeleteEnabled            = Constants::IS_THUMB_DELETE_ENABLED;
    app.thumbPasteEnabled             = Constants::IS_THUMB_PASTE_ENABLED;

    Persistence::Registry::SaveSetting(Constants::Registry::KEEP_IN_BACKGROUND,   static_cast<DWORD>(app.isKeepInBackground));
    Persistence::Registry::SaveSetting(Constants::Registry::RUN_ON_STARTUP,        static_cast<DWORD>(app.isEnableRunOnStartup));
    Persistence::Registry::SaveSetting(Constants::Registry::THUMBNAIL_EFFECTS,     static_cast<DWORD>(app.thumbnailEffectsEnabled));
    Persistence::Registry::SaveSetting(Constants::Registry::LOCK_VIEWPORT,         static_cast<DWORD>(app.lockViewport));
    Persistence::Registry::SaveSetting(Constants::Registry::REMEMBER_WINDOW_POS,   static_cast<DWORD>(app.rememberWindowPosition));
    Persistence::Registry::SaveSetting(Constants::Registry::HISTORY_FULL_MODE,     static_cast<DWORD>(app.historyFullModeEnabled));
    Persistence::Registry::SaveSetting(Constants::Registry::OVERLAY_VISIBLE,       static_cast<DWORD>(app.showOverlayInfoText));
    Persistence::Registry::SaveSetting(Constants::Registry::OPEN_DIRWND_ON_START,  static_cast<DWORD>(app.openDirWndOnStart));
    Persistence::Registry::SaveSetting(Constants::Registry::FOLDER_WALK_WRAP,      static_cast<DWORD>(app.folderWalkWrap));
    Persistence::Registry::SaveSetting(Constants::Registry::OVERLAY_SHOW_BG,       static_cast<DWORD>(app.overlayShowBackground));
    Persistence::Registry::SaveSetting(Constants::Registry::OVERLAY_LAYOUT_MODE,   static_cast<DWORD>(app.overlayLayoutMode));
    Persistence::Registry::SaveSetting(Constants::Registry::OVERLAY_SLOT_VISIBLE,  static_cast<DWORD>(app.overlaySlotVisibleMask));
    Persistence::Registry::SaveSetting(Constants::Registry::OVERLAY_SLOT_COMPACT,  static_cast<DWORD>(app.overlaySlotCompactMask));
    Persistence::Registry::SaveSetting(Constants::Registry::OVERLAY_SHOW_DIR_NAME, static_cast<DWORD>(app.overlayShowDirName));
    Persistence::Registry::SaveSetting(Constants::Registry::OVERLAY_SHOW_FULL_PATH, static_cast<DWORD>(app.overlayShowFullPath));
    Persistence::Registry::SaveSetting(Constants::Registry::HISTORY_ENABLED,       static_cast<DWORD>(app.historyEnabled));
    Persistence::Registry::SaveSetting(Constants::Registry::HISTORY_IMAGES_ONLY,   static_cast<DWORD>(app.historyImagesOnly));
    Persistence::Registry::SaveSetting(Constants::Registry::OVERLAY_SHOW_EFFECTS,  static_cast<DWORD>(app.overlayShowEffectsList));
    Persistence::Registry::SaveSetting(Constants::Registry::OVERLAY_FONT_SIZE,     static_cast<DWORD>(app.overlayFontSize));
    Persistence::Registry::SaveSetting(Constants::Registry::OVERLAY_FONT_COLOR,    static_cast<DWORD>(app.overlayFontColor));
    Persistence::Registry::SaveSetting(Constants::Registry::OVERLAY_FONT_FAMILY,   static_cast<DWORD>(app.overlayFontFamily));
    Persistence::Registry::SaveSetting(Constants::Registry::INPUTBOX_CARET_STYLE,  static_cast<DWORD>(app.caretStyle));
    Persistence::Registry::SaveSetting(Constants::Registry::ZOOM_CLICK_MULT,       static_cast<DWORD>(Converters::toZoomInt(app.zoomClickMultiplier)));
    Persistence::Registry::SaveSetting(Constants::Registry::SWAP_MOUSE_BUTTONS,    static_cast<DWORD>(app.swapMouseButtons));
    Persistence::Registry::SaveSetting(Constants::Registry::CONTEXT_MENU_ENABLED,  static_cast<DWORD>(app.contextMenuEnabled));
    Persistence::Registry::SaveSetting(Constants::Registry::KIOSK_LOCK,            static_cast<DWORD>(app.isLocked));
    Persistence::Registry::SaveSetting(Constants::Registry::ALWAYS_ON_TOP,         static_cast<DWORD>(app.isAlwaysOnTop));
    Persistence::Registry::SaveSetting(Constants::Registry::KEEP_DISPLAY_AWAKE,    static_cast<DWORD>(app.keepDisplayAwake));
    Persistence::Registry::SaveSetting(Constants::Registry::REMOTE_BEACON,         static_cast<DWORD>(app.remoteBeacon));
    Persistence::Registry::SaveSetting(Constants::Registry::REMOTE_LOG_FILE,       static_cast<DWORD>(app.remoteLogToFile));
    Persistence::Registry::SaveSetting(Constants::Registry::GENERAL_LOG,           static_cast<DWORD>(app.generalLog));
    Persistence::Registry::SaveSetting(Constants::Registry::WHEEL_INVERT,          static_cast<DWORD>(app.invertWheelDirection));
    Persistence::Registry::SaveSetting(Constants::Registry::WHEEL_INVERT_H,        static_cast<DWORD>(app.invertWheelDirectionH));
    Persistence::Registry::SaveSetting(Constants::Registry::VRAM_CACHE_COUNT,      static_cast<DWORD>(app.vramCacheCount));
    Persistence::Registry::SaveSetting(Constants::Registry::VIEW_MODE,             static_cast<DWORD>(app.viewMode));
    Persistence::Registry::SaveSetting(Constants::Registry::BASE_WIDTH_KEY,        static_cast<DWORD>(app.baseWidth));
    Persistence::Registry::SaveSetting(Constants::Registry::BASE_HEIGHT_KEY,       static_cast<DWORD>(app.baseHeight));
    Persistence::Registry::SaveSetting(Constants::Registry::START_FULLSCREEN,      static_cast<DWORD>(app.startInFullscreen));
    Persistence::Registry::SaveSetting(Constants::Registry::HISTORY_MAX_DIRS,      static_cast<DWORD>(app.historyMaxDirs));
    Persistence::Registry::SaveSetting(Constants::Registry::HISTORY_MAX_FAVS,      static_cast<DWORD>(app.historyMaxFavs));
    Persistence::Registry::SaveSetting(Constants::Registry::HISTORY_MAX_FAVS_SHOWN, static_cast<DWORD>(app.historyMaxFavsShown));
    Persistence::Registry::SaveSetting(Constants::Registry::DIR_THUMB_CACHE_MB,    static_cast<DWORD>(app.dirThumbCacheMB));
    Persistence::Registry::SaveSetting(Constants::Registry::PRELOAD_LOOKASIDE,     static_cast<DWORD>(app.preloadLookaside));
    Persistence::Registry::SaveSetting(Constants::Registry::MSG_CENTER_MS,         static_cast<DWORD>(app.msgCenterDisplayMs));
    Persistence::Registry::SaveSetting(Constants::Registry::HISTORY_MAX_DIRS_SAVE, static_cast<DWORD>(app.historyMaxDirsSave));
    Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_INTERVAL_MS, static_cast<DWORD>(app.slideshow.intervalMs));
    Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_LOOP,        static_cast<DWORD>(app.slideshow.loop));
    Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_SHUFFLE,     static_cast<DWORD>(app.slideshow.shuffle));
    // Write what was just assigned to app above, not a bare 0 — a literal
    // here silently drifts the moment one of those defaults changes.
    Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_TRANSITION,
        static_cast<DWORD>(app.slideshow.transition.type));
    Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_TRANS_SOURCE,
        static_cast<DWORD>(app.slideshow.transition.source));
    Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_TRANS_ORDER,
        static_cast<DWORD>(app.slideshow.transition.order));
    Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_TRANS_LIST,
        static_cast<DWORD>(app.slideshow.transition.listMask));
    Persistence::Registry::SaveSetting(Constants::Registry::SORT_ORDER,            static_cast<DWORD>(app.fileHandlerDefaultSortOrder));
    Persistence::Registry::SaveSetting(Constants::Registry::SORT_REVERSE,          static_cast<DWORD>(app.fileHandlerIsReverseSortOrder));
    Persistence::Registry::SaveSetting(Constants::Registry::CTRL_C_ENABLED,        static_cast<DWORD>(app.ctrlCEnabled));
    Persistence::Registry::SaveSetting(Constants::Registry::THUMB_COPY_ENABLED,    static_cast<DWORD>(app.thumbCopyEnabled));
    Persistence::Registry::SaveSetting(Constants::Registry::THUMB_MOVE_ENABLED,    static_cast<DWORD>(app.thumbMoveEnabled));
    Persistence::Registry::SaveSetting(Constants::Registry::THUMB_DELETE_ENABLED,  static_cast<DWORD>(app.thumbDeleteEnabled));
    Persistence::Registry::SaveSetting(Constants::Registry::THUMB_PASTE_ENABLED,   static_cast<DWORD>(app.thumbPasteEnabled));
    Persistence::Registry::SaveSetting(Constants::Registry::THEME_FACTOR,
        static_cast<DWORD>(app.themeFactor * Constants::Theme::THEME_FACTOR_STORE_SCALE));

    // Same ordering as ImportSettings, for the same reason: restoring defaults
    // can switch a log off, so the line goes in while that log is still running.
    if (AppLog::IsEnabled())
        AppLog::Info(AppLog::COMP_SETTINGS, L"all settings restored to defaults");

    // THE TWO LOG SINKS MUST BE TOLD, exactly as ImportSettings does. This was
    // missing: the assignments above set app.generalLog and app.remoteLogToFile
    // from the defaults and the registry was written, but nothing reached the
    // writers — so restoring defaults with a log running left it RUNNING and
    // still writing to disk while the menu showed it unticked, and restoring
    // with one enabled by default produced a ticked menu and an empty folder
    // until the next launch. A setting is a number until its sink is told; the
    // import path already says so in its own comment.
    Remote::Log::SetFileLogging(app.remoteLogToFile);
    AppLog::SetEnabled(app.generalLog);

    Persistence::Registry::EnableRunOnStartup(app.isEnableRunOnStartup);
    g_overlayManager.SetAllVisible(app.showOverlayInfoText);

    // Push the three overlay values assigned above into the live slots.
    g_overlayManager.ApplyPersistedState(hWnd);

    SetWindowPos(hWnd, app.isAlwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    uiManager.ApplyAlwaysOnTop(app.isAlwaysOnTop);
    AppCommands::ApplyDisplayAwake(hWnd);
    // Theme is STATE the renderer and every panel must be pushed into —
    // assigning app.themeFactor alone repaints nothing. Import does this too.
    AppCommands::changeAppThemeFactor(hWnd, app.themeFactor);

    // THE SAME OMISSION AS THE TWO LOG SINKS ABOVE. SORT_ORDER and SORT_REVERSE
    // are reset and saved with everything else, but the playlist already in
    // memory was built with the OLD order and keeps it — so the folder on
    // screen stayed sorted the previous way until the next folder change or a
    // restart, with the menu showing the new setting. ImportSettings has always
    // called this; RestoreDefaults is the parallel path that did not.
    ReSortPlaylistAndRebuildMap(hWnd);

    uiManager.RepaintAllPanels();
    InvalidateRect(hWnd, nullptr, FALSE);
    UI::ThemedDialog::Message(hWnd,
        L"All settings have been restored to defaults.", L"Restore Defaults");
}

// =============================================================================
// Backup History & Favorites  →  .zip
// =============================================================================
void BackupHistoryAndFavorites(HWND hWnd) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t defaultName[MAX_PATH];
    swprintf_s(defaultName, L"%s%04d%02d%02d.zip",
               Constants::Backup::BACKUP_PREFIX,
               st.wYear, st.wMonth, st.wDay);

    const std::wstring zipPath =
        PickFile(hWnd, true, ZIP_FILTERS, ARRAYSIZE(ZIP_FILTERS), L"zip", defaultName);
    if (zipPath.empty()) return;

    HistoryFoldersManager hfm;
    auto readBytes = [](const std::wstring &p) -> std::vector<char> {
        std::ifstream f(p, std::ios::binary);
        if (!f.is_open()) return {};
        return {std::istreambuf_iterator<char>(f), {}};
    };
    auto histBytes = readBytes(hfm.GetFilePath());
    auto favBytes  = readBytes(hfm.GetFavoritesFilePath());

    const std::string histEntry = ToUtf8(Constants::History::HISTORY_FILE_NAME);
    const std::string favEntry  = ToUtf8(Constants::History::FAVORITES_FILE_NAME);

    mz_zip_archive zip{};
    bool ok = mz_zip_writer_init_heap(&zip, 0, 65536) == MZ_TRUE;
    if (ok && !histBytes.empty())
        ok = mz_zip_writer_add_mem(&zip, histEntry.c_str(),
                                   histBytes.data(), histBytes.size(),
                                   MZ_BEST_SPEED) == MZ_TRUE;
    if (ok && !favBytes.empty())
        ok = mz_zip_writer_add_mem(&zip, favEntry.c_str(),
                                   favBytes.data(), favBytes.size(),
                                   MZ_BEST_SPEED) == MZ_TRUE;
    void  *pBuf  = nullptr;
    size_t bufSz = 0;
    if (ok)
        ok = mz_zip_writer_finalize_heap_archive(&zip, &pBuf, &bufSz) == MZ_TRUE;
    mz_zip_writer_end(&zip);

    if (ok && pBuf) {
        FILE *fz = nullptr;
        _wfopen_s(&fz, zipPath.c_str(), L"wb");
        if (fz) {
            fwrite(pBuf, 1, bufSz, fz);
            fclose(fz);
            UI::ThemedDialog::Message(hWnd, L"Backup created successfully.", L"Backup");
        } else {
            UI::ThemedDialog::Message(hWnd, L"Failed to write backup file.", L"Backup");
        }
    } else {
        UI::ThemedDialog::Message(hWnd, L"Failed to create backup archive.", L"Backup");
    }
    mz_free(pBuf);
}

// =============================================================================
// TCP/IP configuration  ⇄  .zip
//
// THE THREE FILES THAT DECIDE WHO MAY DRIVE THIS INSTANCE AND WHO IT DRIVES:
//
//   qivLocalServer.ini            this instance's own listener — port, bind
//                                 address, allow-list, TLS, password
//   qivRemoteServers.ini          the instances this copy connects OUT to
//   qivRemoteServerBlacklist.ini  addresses refused, by the brute-force guard
//                                 and by hand from the My Clients panel
//
// Worth a backup of their own rather than riding along with the settings export:
// that export is registry values, and none of these are in the registry. They
// are hand-editable text holding the work of setting up a network of screens,
// and re-typing an allow-list and a password across several machines is exactly
// the job nobody wants to do twice.
//
// THIS ARCHIVE CONTAINS A PASSWORD HASH. qivLocalServer.ini stores the remote
// password as PBKDF2, not plaintext, so the zip is not a plaintext credential —
// but it is still the material an offline guess would be run against, and it
// names every address permitted to connect. It is NOT the logs archive, which
// exists to be attached to bug reports. Keep it as you would the config itself.
// =============================================================================
namespace {
    // One list, used by both directions, so a file can never be backed up and
    // then not restored (or the reverse) because someone added it to one
    // function and forgot the other.
    const wchar_t *const REMOTE_INI_FILES[] = {
        Constants::RemoteTcpIp::LOCAL_SERVER_FILE_NAME,
        Constants::RemoteTcpIp::REMOTES_FILE_NAME,
        Constants::RemoteTcpIp::BLACKLIST_FILE_NAME,
    };
}

void BackupRemoteConfig(HWND hWnd) {
    auto readBytes = [](const std::wstring &p) -> std::vector<char> {
        std::ifstream f(p, std::ios::binary);
        if (!f.is_open()) return {};
        return {std::istreambuf_iterator<char>(f), {}};
    };

    // Read BEFORE the dialog: none of the three has to exist — a copy that only
    // ever connects out has no qivLocalServer.ini, one that has never banned
    // anybody has no blacklist — so "nothing to back up" is a real state and is
    // reported without asking for a destination first.
    struct Item { std::string entry; std::vector<char> bytes; };
    std::vector<Item> items;
    for (const wchar_t *name : REMOTE_INI_FILES) {
        std::vector<char> bytes = readBytes(Persistence::Ini::PathBesideExe(name));
        if (bytes.empty()) continue;
        items.push_back({ToUtf8(name), std::move(bytes)});
    }

    if (items.empty()) {
        UI::ThemedDialog::Message(hWnd,
            L"No TCP/IP configuration to back up.\n\n"
            L"None of qivLocalServer.ini, qivRemoteServers.ini or "
            L"qivRemoteServerBlacklist.ini exists beside the executable yet.",
            L"Backup TCP/IP Config");
        return;
    }

    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t defaultName[MAX_PATH];
    swprintf_s(defaultName, L"%s%04d%02d%02d.zip",
               Constants::Backup::REMOTE_BACKUP_PREFIX,
               st.wYear, st.wMonth, st.wDay);

    const std::wstring zipPath =
        PickFile(hWnd, true, ZIP_FILTERS, ARRAYSIZE(ZIP_FILTERS), L"zip", defaultName);
    if (zipPath.empty()) return;

    mz_zip_archive zip{};
    bool ok = mz_zip_writer_init_heap(&zip, 0, 65536) == MZ_TRUE;
    for (const Item &it : items) {
        if (!ok) break;
        ok = mz_zip_writer_add_mem(&zip, it.entry.c_str(), it.bytes.data(),
                                   it.bytes.size(), MZ_BEST_COMPRESSION) == MZ_TRUE;
    }
    void  *pBuf  = nullptr;
    size_t bufSz = 0;
    if (ok) ok = mz_zip_writer_finalize_heap_archive(&zip, &pBuf, &bufSz) == MZ_TRUE;
    mz_zip_writer_end(&zip);

    if (ok && pBuf) {
        FILE *fz = nullptr;
        _wfopen_s(&fz, zipPath.c_str(), L"wb");
        if (fz) {
            fwrite(pBuf, 1, bufSz, fz);
            fclose(fz);
            const std::wstring msg =
                L"Backed up " + std::to_wstring(items.size()) +
                L" configuration file(s).\n\n"
                L"This archive holds your allow-list and the stored password hash — "
                L"keep it as private as the configuration itself.";
            UI::ThemedDialog::Message(hWnd, msg.c_str(), L"Backup TCP/IP Config");
        } else {
            UI::ThemedDialog::Message(hWnd, L"Failed to write the archive.", L"Backup TCP/IP Config");
        }
    } else {
        UI::ThemedDialog::Message(hWnd, L"Failed to create the archive.", L"Backup TCP/IP Config");
    }
    mz_free(pBuf);
}

void RestoreRemoteConfig(HWND hWnd) {
    // NO BLANKET "are you sure" HERE. The real decisions are per file, and they
    // are asked below, once each, only for files that actually exist and would
    // therefore be destroyed. An up-front confirm on top of those would be a
    // fourth dialog that decides nothing — and the kind users learn to click
    // through, which is how the per-file prompts would stop being read too.
    const std::wstring zipPath =
        PickFile(hWnd, false, ZIP_FILTERS, ARRAYSIZE(ZIP_FILTERS), L"zip", nullptr);
    if (zipPath.empty()) return;

    std::vector<char> zipBytes;
    {
        std::ifstream fz(zipPath, std::ios::binary);
        if (!fz.is_open()) {
            UI::ThemedDialog::Message(hWnd, L"Failed to open the archive.", L"Restore TCP/IP Config");
            return;
        }
        zipBytes = {std::istreambuf_iterator<char>(fz), {}};
    }

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, zipBytes.data(), zipBytes.size(), 0)) {
        UI::ThemedDialog::Message(hWnd,
            L"The selected file is not a valid archive.", L"Restore TCP/IP Config");
        return;
    }

    // What each file governs, so the replace prompt names what is at stake
    // rather than just a filename. Same order as REMOTE_INI_FILES.
    const wchar_t *const WHAT_IT_HOLDS[] = {
        L"this instance's listener — port, bind address, allow-list, TLS and password",
        L"the saved list of instances this copy connects out to",
        L"the addresses refused by the brute-force guard and by hand",
    };
    static_assert(std::size(WHAT_IT_HOLDS) == std::size(REMOTE_INI_FILES),
                  "every remote .ini needs a description for the replace prompt");

    int restored = 0;
    int skipped  = 0;
    for (size_t i = 0; i < std::size(REMOTE_INI_FILES); ++i) {
        const wchar_t *name = REMOTE_INI_FILES[i];
        const std::string entry = ToUtf8(name);
        size_t sz    = 0;
        void  *pData = mz_zip_reader_extract_file_to_heap(&zip, entry.c_str(), &sz, 0);
        // ABSENT IS NOT AN ERROR. The backup only stores the files that existed,
        // so a copy that never ran a listener produces a two-file archive and
        // restoring it must leave the third alone rather than truncate it.
        if (!pData) continue;

        const std::wstring dest = Persistence::Ini::PathBesideExe(name);

        // ASKED PER FILE, and ONLY when something would actually be destroyed.
        // Writing a file that is not there replaces nothing, so prompting for it
        // would be asking permission to do the thing that was just requested.
        std::error_code ec;
        if (fs::exists(dest, ec) && !ec) {
            const std::wstring q =
                std::wstring(L"Replace ") + name + L"?\n\n"
                L"The current file holds " + WHAT_IT_HOLDS[i] + L".\n\n"
                L"Choose No to keep it and carry on with the rest.";
            if (!UI::ThemedDialog::Confirm(hWnd, q.c_str(), L"Restore TCP/IP Config")) {
                mz_free(pData);
                ++skipped;
                continue;
            }
        }

        FILE *f = nullptr;
        _wfopen_s(&f, dest.c_str(), L"wb");
        if (f) {
            if (fwrite(pData, 1, sz, f) == sz) ++restored;
            fclose(f);
        }
        mz_free(pData);
    }
    mz_zip_reader_end(&zip);

    if (restored > 0 || skipped > 0) {
        // NOT reloaded into the running instance, and the dialog says so. The
        // listener, the mirror targets and the blacklist each read their file at
        // startup and hold the result; re-reading them under a live connection
        // would mean rebinding a socket and re-authenticating peers mid-session.
        // A restart is one action the user understands and cannot get half done.
        std::wstring msg = L"Restored " + std::to_wstring(restored) +
                           L" configuration file(s).";
        if (skipped > 0)
            msg += L"\nKept " + std::to_wstring(skipped) + L" existing file(s) unchanged.";
        if (restored > 0)
            msg += L"\n\nRestart qIV for the restored settings to take effect — a "
                   L"running listener keeps the ones it started with.";
        UI::ThemedDialog::Message(hWnd, msg.c_str(), L"Restore TCP/IP Config");
    } else {
        UI::ThemedDialog::Message(hWnd,
            L"That archive holds no TCP/IP configuration files.",
            L"Restore TCP/IP Config");
    }
}

// =============================================================================
// Backup Logs  →  .zip
//
// The whole logs\ tree in one archive, general and network together, because
// the question being answered is almost always "what was the app doing" and the
// answer is usually spread across both — a network fault that ends in a decode
// failure has half its story in each.
//
// WRITTEN WHEREVER THE USER SAYS, not into QivBackup/ beside the exe like the
// history backup. This one exists to be handed to somebody else: attached to a
// bug report, dropped in a chat. A fixed location beside a portable exe is the
// wrong default for a file whose purpose is to leave the machine.
//
// No Restore counterpart, deliberately. Logs are a record, not a setting — there
// is nothing to put back, and an app that could overwrite its own log history
// from a file would make every log it wrote afterwards untrustworthy.
// =============================================================================
void BackupLogs(HWND hWnd) {
    const std::wstring logRoot =
        Persistence::Ini::PathBesideExe(Constants::Logging::DIR_NAME);

    std::error_code ec;
    if (logRoot.empty() || !fs::is_directory(logRoot, ec) || ec) {
        // Said plainly rather than offering a dialog that can only produce an
        // empty zip: both logs are off by default, so "no folder" is the normal
        // state for most installs and is not a failure.
        UI::ThemedDialog::Message(hWnd,
            L"No logs to back up — nothing has been written yet.\n\n"
            L"Switch on General log or Server log to file under Logging first.",
            L"Backup Logs");
        return;
    }

    // Collected BEFORE the file dialog, so an empty tree is reported without
    // making the user pick a destination for nothing.
    struct Entry { fs::path full; std::string name; };
    std::vector<Entry> files;
    uint64_t totalBytes = 0;
    for (fs::recursive_directory_iterator it(
             logRoot, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || ec) { ec.clear(); continue; }

        // Path INSIDE the archive, relative to logs\ and with forward slashes,
        // so the zip carries "general/qIV_General_20260808.log" and any unzipper
        // recreates the two subfolders rather than dumping every file together.
        std::wstring rel = fs::relative(it->path(), logRoot, ec).wstring();
        if (ec) { ec.clear(); continue; }
        for (wchar_t &c : rel) if (c == L'\\') c = L'/';

        files.push_back({it->path(), ToUtf8(rel.c_str())});
        totalBytes += static_cast<uint64_t>(it->file_size(ec));
        ec.clear();
    }

    if (files.empty()) {
        UI::ThemedDialog::Message(hWnd,
            L"The logs folder is empty — nothing has been written yet.",
            L"Backup Logs");
        return;
    }

    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t defaultName[MAX_PATH];
    swprintf_s(defaultName, L"%s%04d%02d%02d.zip",
               Constants::Backup::LOGS_BACKUP_PREFIX,
               st.wYear, st.wMonth, st.wDay);

    const std::wstring zipPath =
        PickFile(hWnd, true, ZIP_FILTERS, ARRAYSIZE(ZIP_FILTERS), L"zip", defaultName);
    if (zipPath.empty()) return;

    auto readBytes = [](const fs::path &p) -> std::vector<char> {
        std::ifstream f(p, std::ios::binary);
        if (!f.is_open()) return {};
        return {std::istreambuf_iterator<char>(f), {}};
    };

    mz_zip_archive zip{};
    bool ok = mz_zip_writer_init_heap(&zip, 0, 65536) == MZ_TRUE;
    int added = 0;
    for (const Entry &e : files) {
        if (!ok) break;
        const std::vector<char> bytes = readBytes(e.full);
        // A log the writer still holds open can come back short or empty; that
        // is worth skipping, not worth failing the whole archive over.
        if (bytes.empty()) continue;
        // MZ_BEST_COMPRESSION, not BEST_SPEED as the history backup uses: these
        // are line-oriented text that deflates to a fraction of its size, and
        // the archive is meant to be attached to something.
        ok = mz_zip_writer_add_mem(&zip, e.name.c_str(), bytes.data(), bytes.size(),
                                   MZ_BEST_COMPRESSION) == MZ_TRUE;
        if (ok) ++added;
    }

    void  *pBuf  = nullptr;
    size_t bufSz = 0;
    if (ok) ok = mz_zip_writer_finalize_heap_archive(&zip, &pBuf, &bufSz) == MZ_TRUE;
    mz_zip_writer_end(&zip);

    if (ok && pBuf) {
        FILE *fz = nullptr;
        _wfopen_s(&fz, zipPath.c_str(), L"wb");
        if (fz) {
            fwrite(pBuf, 1, bufSz, fz);
            fclose(fz);
            const std::wstring msg =
                L"Backed up " + std::to_wstring(added) + L" log file(s), " +
                std::to_wstring((totalBytes + 1023) / 1024) + L" KB uncompressed.";
            UI::ThemedDialog::Message(hWnd, msg.c_str(), L"Backup Logs");
        } else {
            UI::ThemedDialog::Message(hWnd, L"Failed to write the log archive.", L"Backup Logs");
        }
    } else {
        UI::ThemedDialog::Message(hWnd, L"Failed to create the log archive.", L"Backup Logs");
    }
    mz_free(pBuf);
}

// =============================================================================
// Restore History & Favorites  ←  .zip
// =============================================================================
void RestoreHistoryAndFavorites(HWND hWnd) {
    if (!UI::ThemedDialog::Confirm(hWnd,
            L"This will overwrite your current history and favorites with the selected backup.\nContinue?",
            L"Restore Backup"))
        return;

    const std::wstring zipPath =
        PickFile(hWnd, false, ZIP_FILTERS, ARRAYSIZE(ZIP_FILTERS), L"zip", nullptr);
    if (zipPath.empty()) return;

    std::vector<char> zipBytes;
    {
        std::ifstream fz(zipPath, std::ios::binary);
        if (!fz.is_open()) {
            UI::ThemedDialog::Message(hWnd,
                L"Failed to open backup file.", L"Restore Backup");
            return;
        }
        zipBytes = {std::istreambuf_iterator<char>(fz), {}};
    }

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, zipBytes.data(), zipBytes.size(), 0)) {
        UI::ThemedDialog::Message(hWnd,
            L"The selected file is not a valid backup archive.", L"Restore Backup");
        return;
    }

    HistoryFoldersManager hfm;
    const std::string histEntry = ToUtf8(Constants::History::HISTORY_FILE_NAME);
    const std::string favEntry  = ToUtf8(Constants::History::FAVORITES_FILE_NAME);

    auto extractEntry = [&](const std::string &entry, const std::wstring &destPath) -> bool {
        size_t sz    = 0;
        void  *pData = mz_zip_reader_extract_file_to_heap(&zip, entry.c_str(), &sz, 0);
        if (!pData) return false;
        FILE *f = nullptr;
        _wfopen_s(&f, destPath.c_str(), L"wb");
        bool wrote = false;
        if (f) {
            wrote = fwrite(pData, 1, sz, f) == sz;
            fclose(f);
        }
        mz_free(pData);
        return wrote;
    };

    const bool histOk = extractEntry(histEntry, hfm.GetFilePath());
    const bool favOk  = extractEntry(favEntry,  hfm.GetFavoritesFilePath());
    mz_zip_reader_end(&zip);

    if (histOk || favOk) {
        UI::LoadFolderHistoryFromDisk();
        UI::ThemedDialog::Message(hWnd, L"Backup restored successfully.", L"Restore Backup");
    } else {
        UI::ThemedDialog::Message(hWnd,
            L"No history or favorites entries were found in the archive.",
            L"Restore Backup");
    }
}

} // namespace UI::AppMenu::detail
