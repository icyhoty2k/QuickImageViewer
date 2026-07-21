#include "TrayHandler.h"
#include "AppCommands.h"
#include "ContextMenuHandler.h"
#include "../AppState.h"
#include "../Overlays/OverlayManager.h"
#include "../Persistence/HistoryFoldersManager.h"
#include "../Persistence/RegistryManager.h"
#include "../Platform/Constants.h"
#include "../Platform/ConstantsStrings.h" // Constants::Messages::TRANSITION_NAMES
#include "../Platform/FileHandler.h"
#include "../SlideshowTransitions.h"
#include "../UI/FloatingPanels/HistoryListWnd.h"
#include "../UI/ThemedDialog.h"
#include "../UI/UIManager.h"
#include <algorithm>
#include <fstream>
#include <miniz.h>
#include <shobjidl.h>
#include <windowsx.h>

#ifndef MF_RADIOCHECK
#define MF_RADIOCHECK 0x00000200L
#endif

extern AppState app;
extern OverlayManager g_overlayManager;

namespace Input {

TrayHandler::TrayHandler(UI::UIManager &uiMgr, OverlayManager &overlayMgr)
    : m_uiManager(uiMgr), m_overlayManager(overlayMgr) {}

// =============================================================================
//  Entry point
// =============================================================================

LRESULT TrayHandler::Handle(HWND hWnd, WPARAM wParam, LPARAM lParam) {
    if (LOWORD(lParam) == WM_LBUTTONDBLCLK) {
        RestoreWindow(hWnd);
    } else if (LOWORD(lParam) == WM_RBUTTONUP) {
        // Same menu the main-window right-click shows — ContextMenuHandler is
        // the single definition; this handler only supplies the anchor point.
        ContextMenuHandler::Show(hWnd, GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam));
    }
    return 0;
}

// =============================================================================
//  Restore window (double-click)
// =============================================================================

void TrayHandler::RestoreWindow(HWND hWnd) {
    ShowWindow(hWnd, SW_SHOW);
    ShowWindow(hWnd, SW_RESTORE);

    HWND hForegroundWnd = GetForegroundWindow();
    if (hForegroundWnd && hForegroundWnd != hWnd) {
        DWORD foregroundThreadId = GetWindowThreadProcessId(hForegroundWnd, nullptr);
        DWORD currentThreadId    = GetCurrentThreadId();

        AttachThreadInput(foregroundThreadId, currentThreadId, TRUE);
        SetForegroundWindow(hWnd);
        SetWindowPos(hWnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetActiveWindow(hWnd);
        SetFocus(hWnd);
        AttachThreadInput(foregroundThreadId, currentThreadId, FALSE);
    } else {
        SetForegroundWindow(hWnd);
    }
}

// =============================================================================
//  Command dispatch
// =============================================================================

void TrayHandler::DispatchCommand(HWND hWnd, int cmd) {
    switch (cmd) {

    // ── Restore / Help / Exit ─────────────────────────────────────────────────
    case 1:
        ShowWindow(hWnd, SW_SHOW);
        ShowWindow(hWnd, SW_RESTORE);
        SetForegroundWindow(hWnd);
        break;

    case 2:
        ShowWindow(hWnd, SW_SHOW);
        ShowWindow(hWnd, SW_RESTORE);
        SetForegroundWindow(hWnd);
        m_uiManager.getHelpWindow().Show();
        break;

    case 3:
        AppCommands::RemoveTrayIcon(hWnd);
        DestroyWindow(hWnd);
        break;

    // ── Boolean toggles ───────────────────────────────────────────────────────
    case 4:
        app.isKeepInBackground = !app.isKeepInBackground;
        Persistence::Registry::SaveSetting(Constants::Registry::KEEP_IN_BACKGROUND,
            static_cast<DWORD>(app.isKeepInBackground));
        break;

    case 5:
        app.isEnableRunOnStartup = !app.isEnableRunOnStartup;
        Persistence::Registry::SaveSetting(Constants::Registry::RUN_ON_STARTUP,
            static_cast<DWORD>(app.isEnableRunOnStartup));
        Persistence::Registry::EnableRunOnStartup(app.isEnableRunOnStartup);
        break;

    case 6:
        app.thumbnailEffectsEnabled = !app.thumbnailEffectsEnabled;
        Persistence::Registry::SaveSetting(Constants::Registry::THUMBNAIL_EFFECTS,
            static_cast<DWORD>(app.thumbnailEffectsEnabled));
        m_uiManager.RepaintAllPanels();
        break;

    case 7:
        app.historyFullModeEnabled = !app.historyFullModeEnabled;
        Persistence::Registry::SaveSetting(Constants::Registry::HISTORY_FULL_MODE,
            static_cast<DWORD>(app.historyFullModeEnabled));
        break;

    case 8:
        app.showOverlayInfoText = !app.showOverlayInfoText;
        Persistence::Registry::SaveSetting(Constants::Registry::OVERLAY_VISIBLE,
            static_cast<DWORD>(app.showOverlayInfoText));
        m_overlayManager.SetAllVisible(app.showOverlayInfoText);
        InvalidateRect(hWnd, nullptr, FALSE);
        break;

    case 9:
        app.openDirWndOnStart = !app.openDirWndOnStart;
        Persistence::Registry::SaveSetting(Constants::Registry::OPEN_DIRWND_ON_START,
            static_cast<DWORD>(app.openDirWndOnStart));
        break;

    case 13:
        app.overlayShowBackground = !app.overlayShowBackground;
        Persistence::Registry::SaveSetting(Constants::Registry::OVERLAY_SHOW_BG,
            static_cast<DWORD>(app.overlayShowBackground));
        InvalidateRect(hWnd, nullptr, FALSE);
        break;

    case 14:
        app.swapMouseButtons = !app.swapMouseButtons;
        Persistence::Registry::SaveSetting(Constants::Registry::SWAP_MOUSE_BUTTONS,
            static_cast<DWORD>(app.swapMouseButtons));
        break;

    case 15:
        app.invertWheelDirection = !app.invertWheelDirection;
        Persistence::Registry::SaveSetting(Constants::Registry::WHEEL_INVERT,
            static_cast<DWORD>(app.invertWheelDirection));
        break;

    case 16:
        app.invertWheelDirectionH = !app.invertWheelDirectionH;
        Persistence::Registry::SaveSetting(Constants::Registry::WHEEL_INVERT_H,
            static_cast<DWORD>(app.invertWheelDirectionH));
        break;

    case 25:
        app.startInFullscreen = !app.startInFullscreen;
        Persistence::Registry::SaveSetting(Constants::Registry::START_FULLSCREEN,
            static_cast<DWORD>(app.startInFullscreen));
        break;

    case 49:
        app.ctrlCEnabled = !app.ctrlCEnabled;
        Persistence::Registry::SaveSetting(Constants::Registry::CTRL_C_ENABLED,
            static_cast<DWORD>(app.ctrlCEnabled));
        break;

    case 63:
        app.contextMenuEnabled = !app.contextMenuEnabled;
        Persistence::Registry::SaveSetting(Constants::Registry::CONTEXT_MENU_ENABLED,
            static_cast<DWORD>(app.contextMenuEnabled));
        m_overlayManager.PostCenterMessage(hWnd,
            app.contextMenuEnabled ? L"Right-Click Menu: On" : L"Right-Click Menu: Off");
        break;

    case 60:
    case 61: {
        const int newStyle = (cmd == 60) ? 0 : 1;
        if (app.caretStyle != newStyle) {
            app.caretStyle = newStyle;
            Persistence::Registry::SaveSetting(Constants::Registry::INPUTBOX_CARET_STYLE,
                static_cast<DWORD>(app.caretStyle));
            m_overlayManager.PostCenterMessage(hWnd,
                newStyle == 0 ? L"Input Caret: Bar" : L"Input Caret: Underscore");
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        break;
    }

    case 50:
        app.thumbCopyEnabled = !app.thumbCopyEnabled;
        Persistence::Registry::SaveSetting(Constants::Registry::THUMB_COPY_ENABLED,
            static_cast<DWORD>(app.thumbCopyEnabled));
        break;

    case 51:
        app.thumbMoveEnabled = !app.thumbMoveEnabled;
        Persistence::Registry::SaveSetting(Constants::Registry::THUMB_MOVE_ENABLED,
            static_cast<DWORD>(app.thumbMoveEnabled));
        break;

    case 52:
        app.thumbDeleteEnabled = !app.thumbDeleteEnabled;
        Persistence::Registry::SaveSetting(Constants::Registry::THUMB_DELETE_ENABLED,
            static_cast<DWORD>(app.thumbDeleteEnabled));
        break;

    case 53:
        app.thumbPasteEnabled = !app.thumbPasteEnabled;
        Persistence::Registry::SaveSetting(Constants::Registry::THUMB_PASTE_ENABLED,
            static_cast<DWORD>(app.thumbPasteEnabled));
        break;

    // ── Integer input values ──────────────────────────────────────────────────
    case 62: {
        int v = UI::ThemedDialog::PromptInt(hWnd, L"Left-Click Zoom",
            L"Left-click zoom multiplier (1 = off .. 10):",
            static_cast<int>(app.zoomClickMultiplier + 0.5f), 1, 10,
            static_cast<int>(Constants::ZOOM_CLICK));
        if (v >= 0) {
            app.zoomClickMultiplier = static_cast<float>(v);
            Persistence::Registry::SaveSetting(Constants::Registry::ZOOM_CLICK_MULT,
                static_cast<DWORD>(v));
        }
        break;
    }
    case 17: {
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
    case 23: {
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
    case 24: {
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
    case 26: {
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
    case 27: {
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
    case 28: {
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
    case 29: {
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
    case 30: {
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
    case 31: {
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

    // ── View mode ─────────────────────────────────────────────────────────────
    default:
        if (cmd >= 18 && cmd <= 22) {
            int modeNum = cmd - 17;
            app.viewMode = static_cast<Constants::ViewModes::ViewMode>(modeNum);
            Persistence::Registry::SaveSetting(Constants::Registry::VIEW_MODE,
                static_cast<DWORD>(modeNum));
            InvalidateRect(hWnd, nullptr, FALSE);
        }

        // NOTE: every slideshow control (start/stop, interval, loop, shuffle,
        // transition type and transition mode) now lives in the viewer id space
        // and runs through InputManager::ExecuteCommand — see ContextMenuHandler.
        // Nothing slideshow-related is dispatched here any more.

        // ── Sort ──────────────────────────────────────────────────────────────
        else if (cmd >= 43 && cmd <= 47) {
            app.fileHandlerDefaultSortOrder   = cmd - 43;
            app.fileHandlerIsReverseSortOrder = false;
            Persistence::Registry::SaveSetting(Constants::Registry::SORT_ORDER,
                static_cast<DWORD>(app.fileHandlerDefaultSortOrder));
            Persistence::Registry::SaveSetting(Constants::Registry::SORT_REVERSE, 0u);
            ReSortPlaylistAndRebuildMap(hWnd);
        }
        else if (cmd == 48) {
            app.fileHandlerIsReverseSortOrder = !app.fileHandlerIsReverseSortOrder;
            Persistence::Registry::SaveSetting(Constants::Registry::SORT_REVERSE,
                static_cast<DWORD>(app.fileHandlerIsReverseSortOrder));
            ReSortPlaylistAndRebuildMap(hWnd);
        }
        break;

    // ── Export Settings ───────────────────────────────────────────────────────
    case 10: {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t defaultName[MAX_PATH];
        swprintf_s(defaultName, L"%s%04d%02d%02d%s",
                   Constants::SettingsFile::EXPORT_PREFIX,
                   st.wYear, st.wMonth, st.wDay,
                   Constants::SettingsFile::EXPORT_EXTENSION);

        std::wstring exportPath;
        {
            IFileSaveDialog *pfd = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, nullptr,
                                           CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd)))) {
                COMDLG_FILTERSPEC filters[] = {
                    {L"INI Settings", L"*.ini"},
                    {L"All Files",    L"*.*" }
                };
                pfd->SetFileTypes(ARRAYSIZE(filters), filters);
                pfd->SetDefaultExtension(L"ini");
                pfd->SetFileName(defaultName);
                if (SUCCEEDED(pfd->Show(hWnd))) {
                    IShellItem *psi = nullptr;
                    if (SUCCEEDED(pfd->GetResult(&psi))) {
                        PWSTR path = nullptr;
                        if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                            exportPath = path;
                            CoTaskMemFree(path);
                        }
                        psi->Release();
                    }
                }
                pfd->Release();
            }
        }
        if (!exportPath.empty()) {
            FILE *f = nullptr;
            if (_wfopen_s(&f, exportPath.c_str(), L"w,ccs=UTF-8") == 0 && f) {
                fwprintf(f, L"[QuickImageViewer]\n");
                fwprintf(f, L"%s=%d\n", Constants::Registry::KEEP_IN_BACKGROUND,   (int)app.isKeepInBackground);
                fwprintf(f, L"%s=%d\n", Constants::Registry::RUN_ON_STARTUP,        (int)app.isEnableRunOnStartup);
                fwprintf(f, L"%s=%d\n", Constants::Registry::THUMBNAIL_EFFECTS,     (int)app.thumbnailEffectsEnabled);
                fwprintf(f, L"%s=%d\n", Constants::Registry::HISTORY_FULL_MODE,     (int)app.historyFullModeEnabled);
                fwprintf(f, L"%s=%d\n", Constants::Registry::OVERLAY_VISIBLE,       (int)app.showOverlayInfoText);
                fwprintf(f, L"%s=%d\n", Constants::Registry::OPEN_DIRWND_ON_START,  (int)app.openDirWndOnStart);
                fwprintf(f, L"%s=%d\n", Constants::Registry::OVERLAY_SHOW_BG,       (int)app.overlayShowBackground);
                fwprintf(f, L"%s=%d\n", Constants::Registry::INPUTBOX_CARET_STYLE,  app.caretStyle);
                fwprintf(f, L"%s=%d\n", Constants::Registry::ZOOM_CLICK_MULT,       (int)app.zoomClickMultiplier);
                fwprintf(f, L"%s=%d\n", Constants::Registry::SWAP_MOUSE_BUTTONS,    (int)app.swapMouseButtons);
                fwprintf(f, L"%s=%d\n", Constants::Registry::CONTEXT_MENU_ENABLED,  (int)app.contextMenuEnabled);
                fwprintf(f, L"%s=%d\n", Constants::Registry::WHEEL_INVERT,          (int)app.invertWheelDirection);
                fwprintf(f, L"%s=%d\n", Constants::Registry::WHEEL_INVERT_H,        (int)app.invertWheelDirectionH);
                fwprintf(f, L"%s=%d\n", Constants::Registry::VRAM_CACHE_COUNT,      app.vramCacheCount);
                fwprintf(f, L"%s=%d\n", Constants::Registry::VIEW_MODE,             static_cast<int>(app.viewMode));
                fwprintf(f, L"%s=%d\n", Constants::Registry::BASE_WIDTH_KEY,        app.baseWidth);
                fwprintf(f, L"%s=%d\n", Constants::Registry::BASE_HEIGHT_KEY,       app.baseHeight);
                fwprintf(f, L"%s=%d\n", Constants::Registry::START_FULLSCREEN,      (int)app.startInFullscreen);
                fwprintf(f, L"%s=%d\n", Constants::Registry::HISTORY_MAX_DIRS,      app.historyMaxDirs);
                fwprintf(f, L"%s=%d\n", Constants::Registry::HISTORY_MAX_FAVS,      app.historyMaxFavs);
                fwprintf(f, L"%s=%d\n", Constants::Registry::DIR_THUMB_CACHE_MB,    app.dirThumbCacheMB);
                fwprintf(f, L"%s=%d\n", Constants::Registry::PRELOAD_LOOKASIDE,     app.preloadLookaside);
                fwprintf(f, L"%s=%d\n", Constants::Registry::MSG_CENTER_MS,         app.msgCenterDisplayMs);
                fwprintf(f, L"%s=%d\n", Constants::Registry::HISTORY_MAX_DIRS_SAVE, app.historyMaxDirsSave);
                fwprintf(f, L"%s=%d\n", Constants::Registry::SLIDESHOW_INTERVAL_MS, app.slideshow.intervalMs);
                fwprintf(f, L"%s=%d\n", Constants::Registry::SLIDESHOW_LOOP,         (int)app.slideshow.loop);
                fwprintf(f, L"%s=%d\n", Constants::Registry::SLIDESHOW_SHUFFLE,      (int)app.slideshow.shuffle);
                fwprintf(f, L"%s=%d\n", Constants::Registry::SLIDESHOW_TRANSITION,   static_cast<int>(app.slideshow.transition.type));
                fwprintf(f, L"%s=%d\n", Constants::Registry::SLIDESHOW_TRANS_SOURCE, app.slideshow.transition.source);
                fwprintf(f, L"%s=%d\n", Constants::Registry::SLIDESHOW_TRANS_ORDER,  app.slideshow.transition.order);
                fwprintf(f, L"%s=%u\n", Constants::Registry::SLIDESHOW_TRANS_LIST,   app.slideshow.transition.listMask);
                fwprintf(f, L"%s=%d\n", Constants::Registry::SORT_ORDER,             app.fileHandlerDefaultSortOrder);
                fwprintf(f, L"%s=%d\n", Constants::Registry::SORT_REVERSE,           (int)app.fileHandlerIsReverseSortOrder);
                fwprintf(f, L"%s=%d\n", Constants::Registry::CTRL_C_ENABLED,         (int)app.ctrlCEnabled);
                fwprintf(f, L"%s=%d\n", Constants::Registry::THUMB_COPY_ENABLED,     (int)app.thumbCopyEnabled);
                fwprintf(f, L"%s=%d\n", Constants::Registry::THUMB_MOVE_ENABLED,     (int)app.thumbMoveEnabled);
                fwprintf(f, L"%s=%d\n", Constants::Registry::THUMB_DELETE_ENABLED,   (int)app.thumbDeleteEnabled);
                fwprintf(f, L"%s=%d\n", Constants::Registry::THUMB_PASTE_ENABLED,    (int)app.thumbPasteEnabled);
                fwprintf(f, L"%s=%d\n", Constants::Registry::THEME_FACTOR,           (int)(app.themeFactor * 100.0f));
                fclose(f);
                UI::ThemedDialog::Message(hWnd, L"Settings exported successfully.", L"Export Settings");
            } else {
                UI::ThemedDialog::Message(hWnd, L"Failed to write the settings file.", L"Export Settings");
            }
        }
        break;
    }

    // ── Import Settings ───────────────────────────────────────────────────────
    case 11: {
        std::wstring importPath;
        {
            IFileOpenDialog *pfd = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                           CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd)))) {
                COMDLG_FILTERSPEC filters[] = {
                    {L"INI Settings", L"*.ini"},
                    {L"All Files",    L"*.*" }
                };
                pfd->SetFileTypes(ARRAYSIZE(filters), filters);
                DWORD opts = 0;
                pfd->GetOptions(&opts);
                pfd->SetOptions(opts | FOS_FILEMUSTEXIST);
                if (SUCCEEDED(pfd->Show(hWnd))) {
                    IShellItem *psi = nullptr;
                    if (SUCCEEDED(pfd->GetResult(&psi))) {
                        PWSTR path = nullptr;
                        if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                            importPath = path;
                            CoTaskMemFree(path);
                        }
                        psi->Release();
                    }
                }
                pfd->Release();
            }
        }
        if (importPath.empty()) break;
        if (!UI::ThemedDialog::Confirm(hWnd,
                L"Importing will overwrite all current settings. Continue?",
                L"Import Settings")) break;

        FILE *f = nullptr;
        if (_wfopen_s(&f, importPath.c_str(), L"r,ccs=UTF-8") != 0 || !f) {
            UI::ThemedDialog::Message(hWnd, L"Failed to read the settings file.", L"Import Settings");
            break;
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
            applyBool(Constants::Registry::KEEP_IN_BACKGROUND,  app.isKeepInBackground);
            applyBool(Constants::Registry::RUN_ON_STARTUP,       app.isEnableRunOnStartup);
            applyBool(Constants::Registry::THUMBNAIL_EFFECTS,    app.thumbnailEffectsEnabled);
            applyBool(Constants::Registry::HISTORY_FULL_MODE,    app.historyFullModeEnabled);
            applyBool(Constants::Registry::OVERLAY_VISIBLE,      app.showOverlayInfoText);
            applyBool(Constants::Registry::OPEN_DIRWND_ON_START, app.openDirWndOnStart);
            applyBool(Constants::Registry::OVERLAY_SHOW_BG,      app.overlayShowBackground);
            applyBool(Constants::Registry::SWAP_MOUSE_BUTTONS,   app.swapMouseButtons);
            applyBool(Constants::Registry::CONTEXT_MENU_ENABLED, app.contextMenuEnabled);
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
            if (wcscmp(key, Constants::Registry::DIR_THUMB_CACHE_MB) == 0) {
                app.dirThumbCacheMB = std::max(100, std::min(64000, val));
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
                app.slideshow.intervalMs = std::max(100, std::min(60000, val));
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
                app.themeFactor = static_cast<float>(val) / 100.0f;
                Persistence::Registry::SaveSetting(Constants::Registry::THEME_FACTOR,
                    static_cast<DWORD>(val));
            }
        }
        fclose(f);

        if (anyKey) {
            Persistence::Registry::LoadAllSettings(app);

            // Apply side effects
            Persistence::Registry::EnableRunOnStartup(app.isEnableRunOnStartup);
            m_overlayManager.SetAllVisible(app.showOverlayInfoText);
            AppCommands::changeAppThemeFactor(hWnd, app.themeFactor);
            ReSortPlaylistAndRebuildMap(hWnd);
            m_uiManager.RepaintAllPanels();
            InvalidateRect(hWnd, nullptr, FALSE);
            UI::ThemedDialog::Message(hWnd, L"Settings imported successfully.", L"Import Settings");
        } else {
            UI::ThemedDialog::Message(hWnd,
                L"The file appears to be empty or invalid.", L"Import Settings");
        }
        break;
    }

    // ── Restore Defaults ──────────────────────────────────────────────────────
    case 12: {
        if (!UI::ThemedDialog::Confirm(hWnd,
                L"All settings will be reset to their default values.\nThis cannot be undone.",
                L"Restore Defaults")) break;

        app.isKeepInBackground      = Constants::IS_KEEP_IN_BACKGROUND;
        app.isEnableRunOnStartup    = Constants::IS_ENABLE_RUN_ON_STARTUP;
        app.thumbnailEffectsEnabled = Constants::ThumbnailPanel::ThumbnailEffects::EFFECTS_MASTER_ENABLED;
        app.historyFullModeEnabled  = Constants::History::HISTORY_SHOW_FULL_HISTORY;
        app.showOverlayInfoText     = Constants::Overlay::DEFAULT_SHOW_OVERLAY;
        app.openDirWndOnStart       = Constants::IS_OPEN_DIRWND_ON_START;
        app.overlayShowBackground   = Constants::Overlay::IS_OVERLAY_SHOW_BACKGROUND;
        app.caretStyle              = Constants::InputBox::CARET_STYLE;
        app.zoomClickMultiplier     = Constants::ZOOM_CLICK;
        app.swapMouseButtons        = Constants::IS_SWAP_MOUSE_BUTTONS;
        app.contextMenuEnabled      = Constants::IS_CONTEXT_MENU_ENABLED;
        app.invertWheelDirection    = Constants::IS_MOUSE_VERTICAL_REVERSE_SCROLL_DIRECTION;
        app.invertWheelDirectionH   = Constants::IS_MOUSE_HORIZONTAL_REVERSE_SCROLL_DIRECTION;
        app.vramCacheCount          = Constants::IS_VRAM_CACHE_IMAGES_COUNT;
        app.viewMode                = Constants::ViewModes::defaultViewMode;
        app.baseWidth               = Constants::IS_BASE_WIDTH;
        app.baseHeight              = Constants::IS_BASE_HEIGHT;
        app.startInFullscreen       = false;
        app.historyMaxDirs          = Constants::History::IS_HISTORY_MAX_DIRS_TO_SHOW;
        app.historyMaxFavs          = Constants::History::IS_HISTORY_MAX_FAVORITES_TO_SHOW;
        app.dirThumbCacheMB         = Constants::IS_DIR_THUMB_CACHE_BUDGET_MB;
        app.preloadLookaside        = Constants::IS_PRELOAD_LOOKASIDE_COUNT;
        app.msgCenterDisplayMs      = static_cast<int>(Constants::Overlay::IS_MSG_CENTER_DISPLAY_MS);
        app.historyMaxDirsSave      = Constants::History::IS_HISTORY_MAX_DIRS_TO_SAVE;
        app.slideshow.intervalMs    = Constants::Slideshow::IS_INTERVAL_MS;
        app.slideshow.loop          = Constants::Slideshow::IS_LOOP;
        app.slideshow.shuffle       = Constants::Slideshow::IS_SHUFFLE;
        app.slideshow.transition.type           = TransitionType::Cut;
        app.slideshow.transition.source         = Constants::Slideshow::TransitionSource::NONE;
        app.slideshow.transition.order          = Constants::Slideshow::TransitionOrder::SEQUENTIAL;
        app.slideshow.transition.listMask       = 0xFFFFFFFEu; // every animated type
        app.slideshow.transition.seqIndex       = 0;
        app.fileHandlerDefaultSortOrder         = Constants::FileHandler::FILE_HANDLER_DEFAULT_SORT_ORDER;
        app.fileHandlerIsReverseSortOrder       = Constants::FileHandler::FILE_HANDLER_SORT_TYPE_IS_REVERSE;
        app.ctrlCEnabled                        = Constants::IS_CTRL_C_ENABLED;
        app.thumbCopyEnabled                    = Constants::IS_THUMB_COPY_ENABLED;
        app.thumbMoveEnabled                    = Constants::IS_THUMB_MOVE_ENABLED;
        app.thumbDeleteEnabled                  = Constants::IS_THUMB_DELETE_ENABLED;
        app.thumbPasteEnabled                   = Constants::IS_THUMB_PASTE_ENABLED;

        Persistence::Registry::SaveSetting(Constants::Registry::KEEP_IN_BACKGROUND,   static_cast<DWORD>(app.isKeepInBackground));
        Persistence::Registry::SaveSetting(Constants::Registry::RUN_ON_STARTUP,        static_cast<DWORD>(app.isEnableRunOnStartup));
        Persistence::Registry::SaveSetting(Constants::Registry::THUMBNAIL_EFFECTS,     static_cast<DWORD>(app.thumbnailEffectsEnabled));
        Persistence::Registry::SaveSetting(Constants::Registry::HISTORY_FULL_MODE,     static_cast<DWORD>(app.historyFullModeEnabled));
        Persistence::Registry::SaveSetting(Constants::Registry::OVERLAY_VISIBLE,       static_cast<DWORD>(app.showOverlayInfoText));
        Persistence::Registry::SaveSetting(Constants::Registry::OPEN_DIRWND_ON_START,  static_cast<DWORD>(app.openDirWndOnStart));
        Persistence::Registry::SaveSetting(Constants::Registry::OVERLAY_SHOW_BG,       static_cast<DWORD>(app.overlayShowBackground));
        Persistence::Registry::SaveSetting(Constants::Registry::INPUTBOX_CARET_STYLE,  static_cast<DWORD>(app.caretStyle));
        Persistence::Registry::SaveSetting(Constants::Registry::ZOOM_CLICK_MULT,       static_cast<DWORD>(app.zoomClickMultiplier));
        Persistence::Registry::SaveSetting(Constants::Registry::SWAP_MOUSE_BUTTONS,    static_cast<DWORD>(app.swapMouseButtons));
        Persistence::Registry::SaveSetting(Constants::Registry::CONTEXT_MENU_ENABLED,  static_cast<DWORD>(app.contextMenuEnabled));
        Persistence::Registry::SaveSetting(Constants::Registry::WHEEL_INVERT,          static_cast<DWORD>(app.invertWheelDirection));
        Persistence::Registry::SaveSetting(Constants::Registry::WHEEL_INVERT_H,        static_cast<DWORD>(app.invertWheelDirectionH));
        Persistence::Registry::SaveSetting(Constants::Registry::VRAM_CACHE_COUNT,      static_cast<DWORD>(app.vramCacheCount));
        Persistence::Registry::SaveSetting(Constants::Registry::VIEW_MODE,             static_cast<DWORD>(app.viewMode));
        Persistence::Registry::SaveSetting(Constants::Registry::BASE_WIDTH_KEY,        static_cast<DWORD>(app.baseWidth));
        Persistence::Registry::SaveSetting(Constants::Registry::BASE_HEIGHT_KEY,       static_cast<DWORD>(app.baseHeight));
        Persistence::Registry::SaveSetting(Constants::Registry::START_FULLSCREEN,      static_cast<DWORD>(app.startInFullscreen));
        Persistence::Registry::SaveSetting(Constants::Registry::HISTORY_MAX_DIRS,      static_cast<DWORD>(app.historyMaxDirs));
        Persistence::Registry::SaveSetting(Constants::Registry::HISTORY_MAX_FAVS,      static_cast<DWORD>(app.historyMaxFavs));
        Persistence::Registry::SaveSetting(Constants::Registry::DIR_THUMB_CACHE_MB,    static_cast<DWORD>(app.dirThumbCacheMB));
        Persistence::Registry::SaveSetting(Constants::Registry::PRELOAD_LOOKASIDE,     static_cast<DWORD>(app.preloadLookaside));
        Persistence::Registry::SaveSetting(Constants::Registry::MSG_CENTER_MS,         static_cast<DWORD>(app.msgCenterDisplayMs));
        Persistence::Registry::SaveSetting(Constants::Registry::HISTORY_MAX_DIRS_SAVE, static_cast<DWORD>(app.historyMaxDirsSave));
        Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_INTERVAL_MS, static_cast<DWORD>(app.slideshow.intervalMs));
        Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_LOOP,         static_cast<DWORD>(app.slideshow.loop));
        Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_SHUFFLE,      static_cast<DWORD>(app.slideshow.shuffle));
        Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_TRANSITION,   0u);
        Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_TRANS_SOURCE, 0u);
        Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_TRANS_ORDER,  0u);
        Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_TRANS_LIST,   0xFFFFFFFEu);
        Persistence::Registry::SaveSetting(Constants::Registry::SORT_ORDER,      static_cast<DWORD>(app.fileHandlerDefaultSortOrder));
        Persistence::Registry::SaveSetting(Constants::Registry::SORT_REVERSE,    static_cast<DWORD>(app.fileHandlerIsReverseSortOrder));
        Persistence::Registry::SaveSetting(Constants::Registry::CTRL_C_ENABLED,      static_cast<DWORD>(app.ctrlCEnabled));
        Persistence::Registry::SaveSetting(Constants::Registry::THUMB_COPY_ENABLED,   static_cast<DWORD>(app.thumbCopyEnabled));
        Persistence::Registry::SaveSetting(Constants::Registry::THUMB_MOVE_ENABLED,   static_cast<DWORD>(app.thumbMoveEnabled));
        Persistence::Registry::SaveSetting(Constants::Registry::THUMB_DELETE_ENABLED, static_cast<DWORD>(app.thumbDeleteEnabled));
        Persistence::Registry::SaveSetting(Constants::Registry::THUMB_PASTE_ENABLED,  static_cast<DWORD>(app.thumbPasteEnabled));

        Persistence::Registry::EnableRunOnStartup(app.isEnableRunOnStartup);
        m_overlayManager.SetAllVisible(app.showOverlayInfoText);
        m_uiManager.RepaintAllPanels();
        InvalidateRect(hWnd, nullptr, FALSE);
        UI::ThemedDialog::Message(hWnd,
            L"All settings have been restored to defaults.", L"Restore Defaults");
        break;
    }

    // ── Backup History & Favorites ────────────────────────────────────────────
    case 41: {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t defaultName[MAX_PATH];
        swprintf_s(defaultName, L"%s%04d%02d%02d.zip",
                   Constants::Backup::BACKUP_PREFIX,
                   st.wYear, st.wMonth, st.wDay);

        std::wstring zipPath;
        {
            IFileSaveDialog *pfd = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, nullptr,
                                           CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd)))) {
                COMDLG_FILTERSPEC filters[] = {
                    {L"ZIP Archive", L"*.zip"},
                    {L"All Files",   L"*.*" }
                };
                pfd->SetFileTypes(ARRAYSIZE(filters), filters);
                pfd->SetDefaultExtension(L"zip");
                pfd->SetFileName(defaultName);
                if (SUCCEEDED(pfd->Show(hWnd))) {
                    IShellItem *psi = nullptr;
                    if (SUCCEEDED(pfd->GetResult(&psi))) {
                        PWSTR pPath = nullptr;
                        if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pPath))) {
                            zipPath = pPath;
                            CoTaskMemFree(pPath);
                        }
                        psi->Release();
                    }
                }
                pfd->Release();
            }
        }
        if (zipPath.empty()) break;

        HistoryFoldersManager hfm;
        auto readBytes = [](const std::wstring &p) -> std::vector<char> {
            std::ifstream f(p, std::ios::binary);
            if (!f.is_open()) return {};
            return {std::istreambuf_iterator<char>(f), {}};
        };
        auto histBytes = readBytes(hfm.GetFilePath());
        auto favBytes  = readBytes(hfm.GetFavoritesFilePath());

        auto toUtf8 = [](const wchar_t *ws) -> std::string {
            int n = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
            std::string s(n - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, ws, -1, s.data(), n, nullptr, nullptr);
            return s;
        };
        std::string histEntry = toUtf8(Constants::History::HISTORY_FILE_NAME);
        std::string favEntry  = toUtf8(Constants::History::FAVORITES_FILE_NAME);

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
        break;
    }

    // ── Restore History & Favorites ───────────────────────────────────────────
    case 42: {
        if (!UI::ThemedDialog::Confirm(hWnd,
                L"This will overwrite your current history and favorites with the selected backup.\nContinue?",
                L"Restore Backup")) break;

        std::wstring zipPath;
        {
            IFileOpenDialog *pfd = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                           CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd)))) {
                COMDLG_FILTERSPEC filters[] = {
                    {L"ZIP Archive", L"*.zip"},
                    {L"All Files",   L"*.*" }
                };
                pfd->SetFileTypes(ARRAYSIZE(filters), filters);
                pfd->SetDefaultExtension(L"zip");
                if (SUCCEEDED(pfd->Show(hWnd))) {
                    IShellItem *psi = nullptr;
                    if (SUCCEEDED(pfd->GetResult(&psi))) {
                        PWSTR pPath = nullptr;
                        if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pPath))) {
                            zipPath = pPath;
                            CoTaskMemFree(pPath);
                        }
                        psi->Release();
                    }
                }
                pfd->Release();
            }
        }
        if (zipPath.empty()) break;

        std::vector<char> zipBytes;
        {
            std::ifstream fz(zipPath, std::ios::binary);
            if (!fz.is_open()) {
                UI::ThemedDialog::Message(hWnd,
                    L"Failed to open backup file.", L"Restore Backup");
                break;
            }
            zipBytes = {std::istreambuf_iterator<char>(fz), {}};
        }

        mz_zip_archive zip{};
        if (!mz_zip_reader_init_mem(&zip, zipBytes.data(), zipBytes.size(), 0)) {
            UI::ThemedDialog::Message(hWnd,
                L"The selected file is not a valid backup archive.", L"Restore Backup");
            break;
        }

        HistoryFoldersManager hfm;
        auto toUtf8 = [](const wchar_t *ws) -> std::string {
            int n = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
            std::string s(n - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, ws, -1, s.data(), n, nullptr, nullptr);
            return s;
        };
        std::string histEntry = toUtf8(Constants::History::HISTORY_FILE_NAME);
        std::string favEntry  = toUtf8(Constants::History::FAVORITES_FILE_NAME);

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

        bool histOk = extractEntry(histEntry, hfm.GetFilePath());
        bool favOk  = extractEntry(favEntry,  hfm.GetFavoritesFilePath());
        mz_zip_reader_end(&zip);

        if (histOk || favOk) {
            UI::LoadFolderHistoryFromDisk();
            UI::ThemedDialog::Message(hWnd, L"Backup restored successfully.", L"Restore Backup");
        } else {
            UI::ThemedDialog::Message(hWnd,
                L"No history or favorites entries were found in the archive.",
                L"Restore Backup");
        }
        break;
    }

    } // switch
}

} // namespace Input
