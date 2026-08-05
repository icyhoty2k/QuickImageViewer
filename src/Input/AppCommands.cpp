// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

// file: AppCommands.cpp
// header: AppCommands.h
#include "AppCommands.h"
#include "../../resources/resource.h"
#include <dwmapi.h>
#include <uxtheme.h>
#include <commctrl.h>
// comctl32 is linked from CMakeLists.txt — see the note there. It used to arrive
// via a #pragma comment(lib) on this line, which is why it was absent from the
// build file's library list.
#include <algorithm>
#include <numeric>
#include <random>
#include "AppState.h" // Assuming this is the path
#include "../Platform/Constants.h"
#include "../Platform/ConstantsStrings.h"
#include "../Overlays/OverlayManager.h"
#include "../WicDecoder.h"
#include "../UI/UIManager.h"
#include "../Dedicated/DedicatedInstance.h" // AppIconId / IsDedicatedProcess
#include <shlobj_core.h>
#include <shobjidl.h> // IDesktopWallpaper / CLSID_DesktopWallpaper

extern AppState app;

// Applies the current file as the desktop wallpaper via IDesktopWallpaper
// (Win8+). Monitor id nullptr = every monitor. The file is handed to the shell
// as-is, so formats Windows cannot decode itself (SVG, QOI, EXR, …) will fail —
// that is reported instead of silently doing nothing.
void AppCommands::SetDesktopWallpaper(HWND hWnd, int position) {
    if (app.playlist.empty() || app.currentIndex < 0 ||
        app.currentIndex >= static_cast<int>(app.playlist.size()))
        return;

    // Index-aligned with Constants::Wallpaper::FILL..SPAN.
    static constexpr DESKTOP_WALLPAPER_POSITION kPositions[] = {
        DWPOS_FILL, DWPOS_FIT, DWPOS_STRETCH, DWPOS_TILE, DWPOS_CENTER, DWPOS_SPAN
    };
    static_assert(std::size(kPositions) == Constants::Wallpaper::COUNT,
                  "wallpaper position table out of sync with Constants::Wallpaper");
    if (position < 0 || position >= Constants::Wallpaper::COUNT) return;

    const std::wstring &path = app.playlist[app.currentIndex];

    // Reports which COM call failed and its HRESULT — a bare "failed" tells us
    // nothing when the shell refuses a file.
    auto fail = [&](const wchar_t *stage, HRESULT hr) {
        wchar_t buf[64];
        swprintf_s(buf, L"  [%s 0x%08X]", stage, static_cast<unsigned>(hr));
        g_overlayManager.PostCenterMessage(hWnd,
            std::wstring(Constants::Messages::WALLPAPER_FAILED) + buf);
    };

    IDesktopWallpaper *pdw = nullptr;
    // CLSCTX_ALL — the shell hosts this object out-of-proc on some systems, so
    // INPROC_SERVER alone can fail with REGDB_E_CLASSNOTREG.
    HRESULT hr = CoCreateInstance(CLSID_DesktopWallpaper, nullptr,
                                  CLSCTX_ALL, IID_PPV_ARGS(&pdw));
    if (FAILED(hr) || !pdw) {
        fail(L"create", hr);
        return;
    }

    // Position is cosmetic — if it is rejected, still try to set the image.
    pdw->SetPosition(kPositions[position]);

    hr = pdw->SetWallpaper(nullptr, path.c_str()); // nullptr = all monitors
    pdw->Release();

    if (SUCCEEDED(hr)) {
        g_overlayManager.PostCenterMessage(hWnd,
            std::wstring(Constants::Messages::WALLPAPER_SET) +
            Constants::Messages::WALLPAPER_NAMES[position]);
        return;
    }

    // ── Fallback: the classic SPI_SETDESKWALLPAPER path ──────────────────────
    // Style lives in HKCU\Control Panel\Desktop and must be written BEFORE the
    // SystemParametersInfo call, which is what makes the change take effect.
    //   WallpaperStyle: 0=center/tile 2=stretch 6=fit 10=fill 22=span
    //   TileWallpaper : "1" only for Tile
    {
        static constexpr const wchar_t *kStyle[] = {
            L"10", L"6", L"2", L"0", L"0", L"22" // Fill Fit Stretch Tile Center Span
        };
        static_assert(std::size(kStyle) == Constants::Wallpaper::COUNT,
                      "wallpaper style table out of sync with Constants::Wallpaper");
        const wchar_t *tile = (position == Constants::Wallpaper::TILE) ? L"1" : L"0";

        HKEY hKey = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", 0,
                          KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
            const wchar_t *style = kStyle[position];
            RegSetValueExW(hKey, L"WallpaperStyle", 0, REG_SZ,
                           reinterpret_cast<const BYTE *>(style),
                           static_cast<DWORD>((wcslen(style) + 1) * sizeof(wchar_t)));
            RegSetValueExW(hKey, L"TileWallpaper", 0, REG_SZ,
                           reinterpret_cast<const BYTE *>(tile),
                           static_cast<DWORD>((wcslen(tile) + 1) * sizeof(wchar_t)));
            RegCloseKey(hKey);
        }

        // Non-const buffer: SPI_SETDESKWALLPAPER takes PVOID.
        std::wstring mutablePath = path;
        if (SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, mutablePath.data(),
                                  SPIF_UPDATEINIFILE | SPIF_SENDCHANGE)) {
            g_overlayManager.PostCenterMessage(hWnd,
                std::wstring(Constants::Messages::WALLPAPER_SET) +
                Constants::Messages::WALLPAPER_NAMES[position]);
            return;
        }
    }

    fail(L"set", hr);
}

void AppCommands::SaveImageToDisk(HWND hWnd) {
    if (!app.renderer || app.playlist.empty() || app.currentIndex < 0) return;

    const std::wstring &srcPath = app.playlist[app.currentIndex];

    // Default filename: keep original stem, no extension (dialog appends from filter).
    std::wstring defaultName;
    {
        size_t slash = srcPath.find_last_of(L"\\/");
        std::wstring nameOnly = (slash != std::wstring::npos)
                                    ? srcPath.substr(slash + 1)
                                    : srcPath;
        size_t dot = nameOnly.find_last_of(L'.');
        if (dot != std::wstring::npos) nameOnly = nameOnly.substr(0, dot);
        defaultName = nameOnly;
    }

    IFileSaveDialog *pfd = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd))))
        return;

    constexpr size_t nFmt = std::size(Constants::Save::FORMATS);
    COMDLG_FILTERSPEC filters[nFmt];
    for (size_t i = 0; i < nFmt; ++i) {
        filters[i].pszName = Constants::Save::FORMATS[i].description;
        filters[i].pszSpec = Constants::Save::FORMATS[i].pattern;
    }
    pfd->SetFileTypes(static_cast<UINT>(nFmt), filters);
    pfd->SetFileTypeIndex(1);
    pfd->SetDefaultExtension(Constants::Save::DEFAULT_EXT);
    pfd->SetTitle(L"Save Image");
    pfd->SetFileName(defaultName.c_str());

    {
        size_t slash = srcPath.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            IShellItem *psi = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(srcPath.substr(0, slash).c_str(),
                                                      nullptr, IID_PPV_ARGS(&psi)))) {
                pfd->SetFolder(psi);
                psi->Release();
            }
        }
    }

    if (SUCCEEDED(pfd->Show(hWnd))) {
        IShellItem *psi = nullptr;
        if (SUCCEEDED(pfd->GetResult(&psi))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                std::wstring savePath(path);
                CoTaskMemFree(path);
                HRESULT hr = app.renderer->SaveCurrentImageWithEffects(savePath);
                if (FAILED(hr)) {
                    wchar_t errBuf[128];
                    swprintf_s(errBuf, L"HRESULT: 0x%08X", static_cast<unsigned>(hr));
                    TaskDialog(hWnd, nullptr, L"QuickImageViewer",
                               L"Failed to save image", errBuf,
                               TDCBF_OK_BUTTON, TD_ERROR_ICON, nullptr);
                }
            }
            psi->Release();
        }
    }
    pfd->Release();
}

void AppCommands::ToggleFullscreen(HWND hWnd) {
    if (!app.isFullscreen) {
        GetWindowRect(hWnd, &app.savedWindowRect);
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfo(MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST), &mi);

        SetWindowPos(hWnd, HWND_TOPMOST,
                     mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOCOPYBITS);

        DWMNCRENDERINGPOLICY policy = DWMNCRP_DISABLED;
        DwmSetWindowAttribute(hWnd, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
        DWORD noRound = DWMWCP_DONOTROUND; // temporary override — does NOT update app.cornerPreference
        DwmSetWindowAttribute(hWnd, Constants::DWMWA_WINDOW_CORNER_PREFERENCES, &noRound, sizeof(noRound));
        MARGINS margins = {0, 0, 0, 0};
        DwmExtendFrameIntoClientArea(hWnd, &margins);

        app.isFullscreen = true;
    } else {
        SetWindowPos(hWnd, HWND_NOTOPMOST,
                     app.savedWindowRect.left,
                     app.savedWindowRect.top,
                     app.savedWindowRect.right - app.savedWindowRect.left,
                     app.savedWindowRect.bottom - app.savedWindowRect.top,
                     SWP_FRAMECHANGED | SWP_NOCOPYBITS);

        DWMNCRENDERINGPOLICY policy = DWMNCRP_ENABLED;
        DwmSetWindowAttribute(hWnd, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
        changeAppCornerPreference(hWnd, app.cornerPreference); // restore saved preference
        MARGINS margins = {1, 1, 1, 1};
        DwmExtendFrameIntoClientArea(hWnd, &margins);

        app.isFullscreen = false;
    }
}

void AppCommands::ResetWindowLayoutAndEffects(HWND hWnd) {
    // --- Viewport / window ---
    app.ResetWindowState(hWnd);

    // --- All image effects ---
    app.ResetEffects();

    app.UpdateRendererColorEffects(hWnd);
}

void AppCommands::ApplyDisplayAwake(HWND hWnd) {
    // The request we currently hold. SetThreadExecutionState has no "query"
    // form, so the only way to keep it balanced is to remember what was asked
    // for and never re-issue the same thing.
    static bool s_armed = false;

    // Hidden to the tray means nothing is on screen to keep lit — holding the
    // display awake then would just be a machine that never sleeps.
    const bool want = app.keepDisplayAwake && IsWindowVisible(hWnd);
    if (want == s_armed) return;

    // ES_CONTINUOUS alone CLEARS the standing request; OR'd with
    // ES_DISPLAY_REQUIRED it sets one that lasts until cleared. No
    // ES_SYSTEM_REQUIRED: keeping the screen lit is the point, and a display
    // that is on already prevents sleep.
    if (SetThreadExecutionState(want ? (ES_CONTINUOUS | ES_DISPLAY_REQUIRED)
                                     : ES_CONTINUOUS) == 0) {
        // Documented failure return is 0. Leave s_armed alone so the next call
        // retries rather than believing a request it does not hold.
        return;
    }
    s_armed = want;
}

void AppCommands::AddTrayIcon(HWND hWnd) {
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = hWnd;
    nid.uID = ID_TRAY_APP_ICON;
    // NIF_ICON: Shows the icon
    // NIF_MESSAGE: Sends our custom WM_TRAYICON to our WndProc
    // NIF_TIP: Shows a tooltip on hover
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;

    // A dedicated instance always shows the dedicated icon; the main app borrows
    // it while a slideshow runs so a presenting window is recognisable too.
    const UINT iconId = (Dedicated::IsDedicatedProcess() || app.slideshow.running)
                            ? IDI_APP_ICON_DEDICATED
                            : IDI_APP_ICON;
    nid.hIcon = LoadIcon(GetModuleHandle(nullptr), MAKEINTRESOURCE(iconId));

    // Name the instance in the tooltip — with several dedicated copies in the
    // tray, identical icons are otherwise impossible to tell apart.
    std::wstring tip = L"QuickImageViewer";
    if (Dedicated::IsDedicatedProcess()) {
        const std::wstring &n = Dedicated::State().config.name;
        tip += n.empty() ? L" [Dedicated]" : (L" [" + n + L"]");
    }
    if (app.slideshow.running) tip += L" [Slideshow]";
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);


    if (!Shell_NotifyIconW(NIM_MODIFY, &nid)) {
        if (!Shell_NotifyIconW(NIM_ADD, &nid))
            return;
    }
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);

    // Keep the taskbar button icon in sync with the tray icon.
    HICON hSmall = reinterpret_cast<HICON>(
        LoadImageW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(iconId),
                   IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    SendMessageW(hWnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(nid.hIcon));
    SendMessageW(hWnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hSmall ? hSmall : nid.hIcon));
}


void AppCommands::RemoveTrayIcon(HWND hWnd) {
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = hWnd;
    nid.uID = ID_TRAY_APP_ICON;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}


void AppCommands::CopyFilesToClipboard(HWND hWnd, const std::vector<std::wstring> &paths, bool cut) {
    if (paths.empty()) return;

    // Build a double-null-terminated multi-path string for CF_HDROP.
    size_t totalChars = 1; // final terminating null
    for (const auto &p: paths) totalChars += p.size() + 1;
    const size_t dropSize = sizeof(DROPFILES) + totalChars * sizeof(wchar_t);
    HGLOBAL hDrop = GlobalAlloc(GHND, dropSize);
    if (!hDrop) return;

    auto *df = static_cast<DROPFILES *>(GlobalLock(hDrop));
    if (!df) {
        GlobalFree(hDrop);
        return;
    }
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    wchar_t *dst = reinterpret_cast<wchar_t *>(df + 1);
    for (const auto &p: paths) {
        wmemcpy(dst, p.c_str(), p.size() + 1);
        dst += p.size() + 1;
    }
    *dst = L'\0'; // final double-null
    GlobalUnlock(hDrop);

    UINT cfEffect = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    HGLOBAL hEffect = GlobalAlloc(GHND, sizeof(DWORD));
    if (hEffect) {
        auto *pEff = static_cast<DWORD *>(GlobalLock(hEffect));
        if (pEff) {
            *pEff = cut ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
            GlobalUnlock(hEffect);
        } else {
            GlobalFree(hEffect);
            hEffect = nullptr;
        }
    }

    if (OpenClipboard(hWnd)) {
        EmptyClipboard();
        if (!SetClipboardData(CF_HDROP, hDrop))
            GlobalFree(hDrop);
        if (hEffect && cfEffect) {
            if (!SetClipboardData(cfEffect, hEffect))
                GlobalFree(hEffect);
        } else if (hEffect) {
            GlobalFree(hEffect);
        }
        CloseClipboard();
    } else {
        GlobalFree(hDrop);
        if (hEffect) GlobalFree(hEffect);
    }
}

void AppCommands::CopyFileToClipboard(HWND hWnd, const std::wstring &path, bool cut) {
    CopyFilesToClipboard(hWnd, {path}, cut);
}

void AppCommands::DeleteFilesToRecycleBin(const std::vector<std::wstring> &paths) {
    if (paths.empty()) return;
    // Build a double-null-terminated multi-path string.
    std::wstring from;
    for (const auto &p: paths) {
        from += p;
        from += L'\0';
    }
    from += L'\0';
    SHFILEOPSTRUCTW op = {};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
    SHFileOperationW(&op);
}

void AppCommands::DeleteFileToRecycleBin(const std::wstring &path) {
    DeleteFilesToRecycleBin({path});
}

bool AppCommands::ClipboardHasFiles() {
    return IsClipboardFormatAvailable(CF_HDROP) == TRUE;
}

void AppCommands::PasteFilesFromClipboard(HWND hWnd, const std::wstring &targetDir) {
    if (targetDir.empty()) return;
    if (!OpenClipboard(hWnd)) return;

    // Determine whether the clipboard contents were cut (move) or copied.
    bool isCut = false;
    UINT cfEffect = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    if (cfEffect) {
        HGLOBAL hEff = GetClipboardData(cfEffect);
        if (hEff) {
            auto *pEff = static_cast<const DWORD *>(GlobalLock(hEff));
            if (pEff) {
                isCut = (*pEff & DROPEFFECT_MOVE) != 0;
                GlobalUnlock(hEff);
            }
        }
    }

    HGLOBAL hDrop = GetClipboardData(CF_HDROP);
    if (!hDrop) {
        CloseClipboard();
        return;
    }

    HDROP hd = static_cast<HDROP>(GlobalLock(hDrop));
    if (!hd) {
        CloseClipboard();
        return;
    }

    UINT count = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
    std::wstring from;
    for (UINT i = 0; i < count; ++i) {
        UINT len = DragQueryFileW(hd, i, nullptr, 0);
        if (!len) continue;
        std::wstring buf(len + 1, L'\0');
        DragQueryFileW(hd, i, buf.data(), len + 1);
        buf.resize(len);
        from += buf;
        from += L'\0';
    }
    GlobalUnlock(hDrop);
    CloseClipboard();

    if (from.empty()) return;
    from += L'\0'; // double-null

    std::wstring to = targetDir + L'\0' + L'\0';

    SHFILEOPSTRUCTW op = {};
    op.hwnd = hWnd;
    op.wFunc = isCut ? FO_MOVE : FO_COPY;
    op.pFrom = from.c_str();
    op.pTo = to.c_str();
    op.fFlags = FOF_ALLOWUNDO;
    SHFileOperationW(&op);
}


void AppCommands::changeAppCornerPreference(HWND hWnd, DWORD cornerStyle) {
    app.cornerPreference = cornerStyle;

    if (hWnd) {
        DwmSetWindowAttribute(hWnd, Constants::DWMWA_WINDOW_CORNER_PREFERENCES, &app.cornerPreference, sizeof(app.cornerPreference));
        SetWindowPos(hWnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
}

void AppCommands::changeAppThemeToDarkMode(HWND hWnd, bool isDarkThemed) {
    app.isDarkThemed = isDarkThemed;

    // 1. Update the process-wide theme (Menus, standard UI controls)
    HMODULE hUxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (hUxtheme) {
        // Ordinal 135: 0 = Default, 1 = AllowDark, 2 = ForceDark, 3 = ForceLight
        using fnSetPreferredAppMode = int(WINAPI*)(int);
        auto SetPreferredAppMode = (fnSetPreferredAppMode) GetProcAddress(hUxtheme, MAKEINTRESOURCEA(135));

        if (SetPreferredAppMode) {
            SetPreferredAppMode(isDarkThemed ? 2 : 3);
        }

        // Ordinal 136 flushes the cached theme so the OS redraws menus using the new mode
        using fnFlushMenuThemes = void(WINAPI*)();
        auto FlushMenuThemes = (fnFlushMenuThemes) GetProcAddress(hUxtheme, MAKEINTRESOURCEA(136));

        if (FlushMenuThemes) {
            FlushMenuThemes();
        }

        FreeLibrary(hUxtheme);
    }

    // 2. Update the specific window's DWM frame (Title bar and context menu ownership)
    if (hWnd) {
        BOOL darkMode = isDarkThemed ? TRUE : FALSE;
        DwmSetWindowAttribute(hWnd, Constants::DWMWA_DARK_MODE, &darkMode, sizeof(darkMode));

        // Force the OS to redraw the non-client area so title bar changes apply instantly
        SetWindowPos(hWnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
}

void AppCommands::changeAppThemeFactor(HWND hWnd, float newFactor) {
    app.themeFactor = std::clamp(newFactor,
                                 Constants::Theme::THEME_FACTOR_MIN,
                                 Constants::Theme::THEME_FACTOR_MAX);

    // Auto-switch dark/light at the 0.5 midpoint
    bool shouldBeDark = app.themeFactor < 0.5f;
    if (app.isDarkThemed != shouldBeDark)
        changeAppThemeToDarkMode(hWnd, shouldBeDark);

    // Update renderer background color
    if (app.renderer)
        app.renderer->SetThemeFactor(app.themeFactor);

    // Repaint main window (WS_CHILD panels are covered by RDW_ALLCHILDREN)
    if (hWnd)
        RedrawWindow(hWnd, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);

    // Floating panels are WS_POPUP — not reached by RDW_ALLCHILDREN.
    // Update their DWM title-bar attrs and repaint their client areas.
    uiManager.NotifyThemeChanged();
}

void AppCommands::changeAppBackdropType(HWND hWnd, DWORD newType) {
    app.backdropType = newType;
    if (hWnd)
        DwmSetWindowAttribute(hWnd, Constants::DWMWA_SYSTEMBACKDROP_TYPE_ATTR,
                              &app.backdropType, sizeof(app.backdropType));
}

void AppCommands::stopSlideshow(HWND hWnd) {
    KillTimer(hWnd, Constants::Slideshow::TIMER_ID);
    KillTimer(hWnd, Constants::Slideshow::CURSOR_TIMER_ID);
    if (app.slideshow.cursorHidden) {
        ShowCursor(TRUE);
        app.slideshow.cursorHidden = false;
    }
    // Also undo a startup -hideMouse. Stopping the slideshow is the moment
    // someone has walked up to reconfigure the screen, so the pointer must come
    // back — otherwise a dedicated instance is unusable without a restart.
    if (app.cursorHiddenAtStartup) {
        ShowCursor(TRUE);
        app.cursorHiddenAtStartup = false;
    }
    // Restore overlay panels to their pre-slideshow state
    app.showOverlayInfoText = app.slideshow.savedOverlayVisible;
    g_overlayManager.SetAllVisible(app.showOverlayInfoText);
    InvalidateRect(hWnd, nullptr, FALSE);

    app.slideshow.running = false;
    app.slideshow.paused = false;
    app.slideshow.shuffleOrder.clear();
    app.slideshow.shufflePos = 0;
    AddTrayIcon(hWnd);
}

void AppCommands::toggleSlideshow(HWND hWnd) {
    if (!app.slideshow.running) {
        // --- Start ---
        if (!app.playlist.empty() && app.slideshow.shuffle) {
            int n = static_cast<int>(app.playlist.size());
            app.slideshow.shuffleOrder.resize(n);
            std::iota(app.slideshow.shuffleOrder.begin(), app.slideshow.shuffleOrder.end(), 0);
            std::shuffle(app.slideshow.shuffleOrder.begin(), app.slideshow.shuffleOrder.end(),
                         std::mt19937{std::random_device{}()});
            app.slideshow.shufflePos = 0;
        }
        // Save and hide info panels
        app.slideshow.savedOverlayVisible = app.showOverlayInfoText;
        app.showOverlayInfoText = false;
        g_overlayManager.SetAllVisible(false);

        SetTimer(hWnd, Constants::Slideshow::TIMER_ID, app.slideshow.intervalMs, nullptr);
        if (app.slideshow.cursorHideMs > 0)
            SetTimer(hWnd, Constants::Slideshow::CURSOR_TIMER_ID, app.slideshow.cursorHideMs, nullptr);
        app.slideshow.running = true;
        app.slideshow.paused = false;
        AddTrayIcon(hWnd);
    } else {
        // --- Stop (whether playing or paused) ---
        stopSlideshow(hWnd);
    }
}

void AppCommands::CopyImageToClipboard(HWND hWnd) {
    if (app.playlist.empty() || app.currentIndex < 0 || !app.wicFactory) return;
    const std::wstring &path = app.playlist[app.currentIndex];

    DecodedImage img;
    if (FAILED(WicDecoder::DecodeImage(path, img)) || !img.bitmap) return;

    Microsoft::WRL::ComPtr<IWICFormatConverter> conv;
    if (FAILED(app.wicFactory->CreateFormatConverter(&conv))) return;
    if (FAILED(conv->Initialize(img.bitmap.Get(), GUID_WICPixelFormat32bppBGR,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
        return;

    UINT w = img.width, h = img.height;
    UINT stride = w * 4;

    // Create a top-down DIB section and copy WIC pixels directly into it.
    // Putting CF_BITMAP on the clipboard lets Windows synthesize CF_DIB and
    // CF_DIBV5 automatically — paste targets receive a standard bottom-up DIB
    // regardless of whether they request CF_BITMAP or CF_DIB.
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = static_cast<LONG>(w);
    bi.bmiHeader.biHeight = -static_cast<LONG>(h); // top-down matches WIC pixel order
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void *bits = nullptr;
    HDC hdc = GetDC(nullptr);
    HBITMAP hBmp = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (!hBmp || !bits) return;

    if (FAILED(conv->CopyPixels(nullptr, stride, stride * h, static_cast<BYTE*>(bits)))) {
        DeleteObject(hBmp);
        return;
    }

    // Build CF_HDROP so Ctrl+V in Explorer pastes the file itself.
    const std::wstring &filePath = path;
    SIZE_T pathBytes = (filePath.size() + 2) * sizeof(wchar_t); // path + \0 + list \0
    HGLOBAL hDrop = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + pathBytes);
    if (!hDrop) {
        DeleteObject(hBmp);
        return;
    }
    auto *df = static_cast<DROPFILES *>(GlobalLock(hDrop));
    if (!df) {
        GlobalFree(hDrop);
        DeleteObject(hBmp);
        return;
    }
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    memcpy(reinterpret_cast<BYTE *>(df) + sizeof(DROPFILES),
           filePath.c_str(), filePath.size() * sizeof(wchar_t));
    // remaining bytes are already zero (GMEM_ZEROINIT) → double-null terminator
    GlobalUnlock(hDrop);

    if (OpenClipboard(hWnd)) {
        EmptyClipboard();
        // CF_BITMAP: image editors (Paint, Photoshop, Word, …)
        if (!SetClipboardData(CF_BITMAP, hBmp))
            DeleteObject(hBmp); // OS owns hBmp on success — only delete on failure
        // CF_HDROP: Explorer paste copies the actual file
        if (!SetClipboardData(CF_HDROP, hDrop))
            GlobalFree(hDrop);
        CloseClipboard();
        g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::COPIED_TO_CLIPBOARD);
    } else {
        DeleteObject(hBmp);
        GlobalFree(hDrop);
    }
}

void AppCommands::pauseResumeSlideshow(HWND hWnd) {
    if (!app.slideshow.running) return;
    if (!app.slideshow.paused) {
        // --- Pause ---
        KillTimer(hWnd, Constants::Slideshow::TIMER_ID);
        KillTimer(hWnd, Constants::Slideshow::CURSOR_TIMER_ID);
        if (app.slideshow.cursorHidden) {
            ShowCursor(TRUE);
            app.slideshow.cursorHidden = false;
        }
        app.slideshow.paused = true;
    } else {
        // --- Resume ---
        SetTimer(hWnd, Constants::Slideshow::TIMER_ID, app.slideshow.intervalMs, nullptr);
        if (app.slideshow.cursorHideMs > 0)
            SetTimer(hWnd, Constants::Slideshow::CURSOR_TIMER_ID, app.slideshow.cursorHideMs, nullptr);
        app.slideshow.paused = false;
    }
}
