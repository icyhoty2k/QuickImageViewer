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
#include <filesystem> // OpenOverlayFolderInExplorer walks up to a live parent
#include <numeric>
#include <random>
#include "AppState.h" // Assuming this is the path
#include "../Platform/Constants.h"
#include "../Platform/ConstantsStrings.h"
#include "../Overlays/OverlayManager.h"
#include "../WicDecoder.h"
#include "../UI/UIManager.h"
#include "../UI/ThemedDialog.h"    // the cull resolve asks before it moves anything
#include "../Platform/FileHandler.h" // LoadImageIndex / ReloadCurrentDirectory
#include "../Dedicated/DedicatedInstance.h" // AppIconId / IsDedicatedProcess
#include "../Platform/AppLog.h"             // COMP_SLIDESHOW — start/stop go in the General log
#include <shellapi.h>  // ShellExecuteW — OpenOverlayFolderInExplorer
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
    if (!app.renderer || app.currentIndex < 0 ||
        app.currentIndex >= static_cast<int>(app.playlist.size()))
        return;

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

bool AppCommands::OpenOverlayFolderInExplorer(HWND hWnd) {
    if (app.folderOverlay == AppState::FolderOverlayState::None) return false;
    if (app.folderOverlayPath.empty()) return false;

    // Walk up until something that still exists is found. In the Missing state
    // the named folder is gone by definition, and opening its nearest surviving
    // parent is more use than doing nothing: that is where the user was.
    std::filesystem::path p(app.folderOverlayPath);
    std::error_code ec;
    while (!p.empty() && (!std::filesystem::is_directory(p, ec) || ec)) {
        std::filesystem::path parent = p.parent_path();
        if (parent == p) break; // reached the root
        p = parent;
        ec.clear();
    }

    ec.clear();
    if (!std::filesystem::is_directory(p, ec) || ec) return false;

    ShellExecuteW(hWnd, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return true;
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

        // Say so, the way CopyImageToClipboard below already does for the image
        // on screen. Ctrl+C is one keystroke with two targets — the picture when
        // the main window has focus, the strip's selection when a strip does —
        // and only one of them used to answer. A copy leaves no mark on the
        // thumbnails either (a CUT dims them; a copy changes nothing), so
        // without this the successful case and the refused case looked alike.
        std::wstring msg = cut ? Constants::Messages::CUT_FILES_PREFIX
                               : Constants::Messages::COPIED_FILES_PREFIX;
        if (paths.size() == 1)
            msg += paths[0].substr(paths[0].find_last_of(L"\\/") + 1);
        else
            msg += std::to_wstring(paths.size()) + Constants::Messages::CLIPBOARD_FILES_COUNT;
        g_overlayManager.PostCenterMessage(hWnd, msg);
    } else {
        GlobalFree(hDrop);
        if (hEffect) GlobalFree(hEffect);
        g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::CLIPBOARD_UNAVAILABLE,
                                           OverlayManager::MsgSeverity::Warning);
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

// =============================================================================
//  CULL MODE - keep / reject a folder down to size.
//
//  Nothing here writes to disk except ResolveCullRejects, and that one asks
//  first. Everything else moves an entry in a hash map.
// =============================================================================
void AppCommands::ToggleCullMode(HWND hWnd) {
    // LEAVING WITH REJECTS PENDING IS WHAT ASKS. There is no separate resolve
    // key: plain Enter is fullscreen and every modified Enter is already a
    // remote command, so the exit carries the question instead. A user who
    // cancels stays in the mode with their marks intact - a cancel means "not
    // yet", not "throw the last twenty minutes away".
    if (app.cullMode) {
        if (app.cullMarks.Count(Common::CullMarks::Mark::Reject) > 0) {
            if (ResolveCullRejects(hWnd) < 0) return;   // cancelled - stay in the mode
        }

        // ⚠ A MARK STILL STANDING MEANS THE JOB IS NOT DONE, so the mode does
        // not close over it. Resolve calls Forget on each file it actually
        // moved; anything left is either a move that FAILED - a name clash, a
        // read-only file - or a reject sitting in a folder the playlist has
        // since walked away from. Clearing unconditionally threw both away
        // silently, told the user "some files could not be moved", and left no
        // way to retry: the marks that message referred to were already gone.
        if (app.cullMarks.Count(Common::CullMarks::Mark::Reject) > 0) {
            g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::CULL_MARKS_REMAIN,
                                               OverlayManager::MsgSeverity::Warning);
            return;
        }

        app.cullMode = false;
        app.cullMarks.Clear();
        g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::CULL_MODE_OFF);
    } else {
        // ⚠ A RUNNING SLIDESHOW ADVANCES BY ITSELF, and this mode moves files.
        //
        // Marking is one key per picture with no confirmation per press, so a
        // timer stepping the playlist underneath means the K pressed for the
        // frame on screen lands on the one after it. Stopping beats refusing:
        // the two modes want the same keyboard for opposite reasons, and
        // somebody pressing Alt+Space during a slideshow has decided to stop
        // watching and start judging.
        if (app.slideshow.running) stopSlideshow(hWnd);

        app.cullMode = true;
        g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::CULL_MODE_ON);
    }
    // The badge sits on the filename line, so it has to be rebuilt on the way
    // in AND on the way out - or the last mark stays on screen over a mode that
    // is no longer running.
    g_overlayManager.RefreshCullBadge();
    InvalidateRect(hWnd, nullptr, FALSE);
}

void AppCommands::MarkCurrent(HWND hWnd, Common::CullMarks::Mark mark) {
    if (app.playlist.empty() || app.currentIndex < 0 ||
        app.currentIndex >= static_cast<int>(app.playlist.size())) {
        g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::CULL_NO_IMAGE,
                                           OverlayManager::MsgSeverity::Warning);
        return;
    }

    const std::wstring path = app.playlist[app.currentIndex];
    app.cullMarks.Toggle(path, mark);

    // WHAT IT REPORTS IS WHAT THE MARK NOW IS, not what was asked for. Pressing
    // X twice undoes the reject, and announcing "Reject" for the keystroke that
    // cleared one is how a folder gets culled wrong.
    const Common::CullMarks::Mark now = app.cullMarks.Of(path);
    std::wstring msg;
    if (now == Common::CullMarks::Mark::Keep) {
        msg = Constants::Messages::CULL_KEPT;
    } else if (now == Common::CullMarks::Mark::Reject) {
        msg = Constants::Messages::CULL_REJECTED;
    } else {
        msg = Constants::Messages::CULL_UNMARKED;
    }

    wchar_t counts[64];
    swprintf_s(counts, Constants::Messages::CULL_COUNTS_FMT,
               app.cullMarks.Count(Common::CullMarks::Mark::Keep),
               app.cullMarks.Count(Common::CullMarks::Mark::Reject));
    msg += counts;
    g_overlayManager.PostCenterMessage(hWnd, msg);

    // ADVANCE, BECAUSE THAT IS THE WHOLE POINT. A cull is one keystroke per
    // picture; making it two would double the work the feature exists to halve.
    //
    // The LAST image does not wrap. Everywhere else in this app Next wraps, and
    // that is right for browsing - but a cull that jumps back to frame one after
    // the final frame starts a second pass nobody asked for, over pictures that
    // are already marked.
    const int size = static_cast<int>(app.playlist.size());
    if (app.currentIndex + 1 < size) {
        LoadImageIndex(hWnd, app.currentIndex + 1);
    }
    // Both paths repaint the badge: the one that moved is showing a different
    // picture, and the one that did not just changed this picture's mark.
    g_overlayManager.RefreshCullBadge();
    InvalidateRect(hWnd, nullptr, FALSE);
}

// Returns the number moved, or -1 when the user said no.
//
// ⚠ THE -1 IS NOT A COUNT. The caller uses it to stay in cull mode with the
// marks intact. Returning 0 for a refusal would make "you cancelled" and "there
// was nothing to move" the same answer, and only one of those should drop the
// marks the user spent the session making.
int AppCommands::ResolveCullRejects(HWND hWnd) {
    const std::vector<std::wstring> rejects = app.cullMarks.Rejected(app.playlist);

    // ⚠ REJECTS OUTSIDE THE CURRENT FOLDER ARE NOT MOVED, AND NOW SAY SO.
    //
    // Rejected() lists only what is in the playlist on screen, deliberately:
    // moving a file the user cannot currently see is the wrong direction for a
    // mistake. But the difference used to be invisible - mark twenty in one
    // folder, wander into another, leave the mode, and the answer was "nothing
    // was rejected" over twenty standing marks.
    const int total = app.cullMarks.Count(Common::CullMarks::Mark::Reject);
    const int elsewhere = total - static_cast<int>(rejects.size());

    if (rejects.empty()) {
        g_overlayManager.PostCenterMessage(
            hWnd,
            elsewhere > 0 ? Constants::Messages::CULL_REJECTS_ELSEWHERE
                          : Constants::Messages::CULL_NOTHING_REJECTED,
            elsewhere > 0 ? OverlayManager::MsgSeverity::Warning
                          : OverlayManager::MsgSeverity::Normal);
        return 0;
    }

    wchar_t question[512];
    swprintf_s(question, Constants::Messages::CULL_CONFIRM_FMT,
               static_cast<int>(rejects.size()),
               rejects.size() == 1 ? L"" : L"s",
               Constants::Messages::CULL_REJECT_FOLDER);
    if (!UI::ThemedDialog::Confirm(hWnd, question,
                                   Constants::Messages::CULL_CONFIRM_CAPTION))
        return -1;

    // GROUPED BY DIRECTORY, one SHFileOperation per folder.
    //
    // A cull normally runs inside one folder, but "search everywhere" and a
    // playlist assembled from several folders both break that, and FO_MOVE takes
    // a single destination. Sending files from two folders into one _rejected is
    // not what "a subfolder of the folder they are in" means, and it silently
    // merges two shoots.
    std::vector<std::wstring> dirs;
    std::vector<std::vector<std::wstring>> groups;
    for (const std::wstring &p : rejects) {
        const size_t slash = p.find_last_of(L"\\/");
        if (slash == std::wstring::npos) continue;   // no directory part: nowhere to move it
        const std::wstring dir = p.substr(0, slash);

        int found = -1;
        for (int i = 0; i < static_cast<int>(dirs.size()); ++i) {
            if (dirs[i] == dir) { found = i; break; }
        }
        if (found < 0) {
            dirs.push_back(dir);
            groups.emplace_back();
            found = static_cast<int>(dirs.size()) - 1;
        }
        groups[found].push_back(p);
    }

    int moved = 0;
    bool anyFailed = false;
    for (int g = 0; g < static_cast<int>(dirs.size()); ++g) {
        const std::wstring dest = dirs[g] + L"\\" +
                                  Constants::Messages::CULL_REJECT_FOLDER;

        // Double-null-terminated, the way DeleteFilesToRecycleBin above builds
        // its own. FOF_NOCONFIRMMKDIR is what creates _rejected; making it here
        // by hand would leave an empty folder behind when the move then failed.
        std::wstring from;
        for (const std::wstring &p : groups[g]) {
            from += p;
            from += L'\0';
        }
        from += L'\0';

        std::wstring to = dest;
        to += L'\0';
        to += L'\0';

        SHFILEOPSTRUCTW op = {};
        op.hwnd = hWnd;
        op.wFunc = FO_MOVE;
        op.pFrom = from.c_str();
        op.pTo = to.c_str();
        // ⚠ NO FOF_SILENT AND NO FOF_NOERRORUI, unlike the recycle-bin helper
        // above. A move that hits a name clash or a read-only file has to say
        // so: this is the one step in the feature that changes the disk, and a
        // cull that quietly dropped four of forty is worse than one that stopped
        // and asked.
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR;

        const int rc = SHFileOperationW(&op);
        if (rc != 0 || op.fAnyOperationsAborted) {
            anyFailed = true;
            continue;   // the other folders still get their turn
        }
        for (const std::wstring &p : groups[g]) {
            app.cullMarks.Forget(p);
            ++moved;
        }
    }

    if (anyFailed) {
        g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::CULL_MOVE_FAILED,
                                           OverlayManager::MsgSeverity::Warning);
    } else {
        wchar_t done[192];
        swprintf_s(done, Constants::Messages::CULL_MOVED_FMT, moved,
                   Constants::Messages::CULL_REJECT_FOLDER);
        std::wstring msg = done;
        if (elsewhere > 0) {
            wchar_t rest[96];
            swprintf_s(rest, Constants::Messages::CULL_ELSEWHERE_FMT, elsewhere);
            msg += rest;
        }
        g_overlayManager.PostCenterMessage(hWnd, msg);
    }

    // The playlist now names files that are no longer where it says. Reloading
    // the folder is what F5 already does; walking it again by hand here would be
    // a second, differently-wrong version of the same code.
    if (moved > 0) ReloadCurrentDirectory(hWnd);
    return moved;
}

bool AppCommands::ClipboardHasFiles() {
    return IsClipboardFormatAvailable(CF_HDROP) == TRUE;
}

bool AppCommands::CopyTextToClipboard(HWND hWnd, const std::wstring &text) {
    if (text.empty()) return false;
    if (!OpenClipboard(hWnd)) return false;

    EmptyClipboard();

    // +1 for the terminating null — GlobalAlloc'd memory handed to
    // SetClipboardData must carry it, the string's size() does not.
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!hMem) {
        CloseClipboard();
        return false;
    }

    void *ptr = GlobalLock(hMem);
    if (!ptr) {
        GlobalFree(hMem);
        CloseClipboard();
        return false;
    }
    memcpy(ptr, text.c_str(), bytes);
    GlobalUnlock(hMem);

    // The clipboard OWNS hMem once this succeeds — freeing it then is a
    // double-free. Free it only on the failure path, where ownership never
    // transferred. This is the line the three hand-written copies disagreed on.
    const bool ok = SetClipboardData(CF_UNICODETEXT, hMem) != nullptr;
    if (!ok) GlobalFree(hMem);

    CloseClipboard();
    return ok;
}

// What file is the picture on screen? See the header for why this is not simply
// app.playlist[app.currentIndex].
//
// `ShowImageOnce <path>` is the one interjection that answers Ok: it names a
// file that was already on this machine, and ownsTempFile=false is exactly the
// flag that keeps this code from deleting it. It is genuinely what is on screen,
// so it is genuinely the right answer.
//
// The playlist can be EMPTY while an interjection shows, so the bounds check
// cannot come first — it would report "nothing open" with a picture plainly on
// the screen.
AppCommands::CurrentImage AppCommands::GetCurrentImagePath(std::wstring &pathOut) {
    pathOut.clear();

    if (app.interject.showing) {
        if (app.interject.ownsTempFile) return CurrentImage::Streamed;
        if (app.interject.path.empty()) return CurrentImage::None;
        pathOut = app.interject.path;
        return CurrentImage::Ok;
    }

    if (app.currentIndex < 0 ||
        app.currentIndex >= static_cast<int>(app.playlist.size()))
        return CurrentImage::None;

    if (app.playlist[app.currentIndex].empty()) return CurrentImage::None;

    pathOut = app.playlist[app.currentIndex];
    return CurrentImage::Ok;
}

// Posts the message for a non-Ok result. Both current-image commands fail the
// same two ways and say the same two things, so the wording lives once.
static void ReportNoCurrentImage(HWND hWnd, AppCommands::CurrentImage what) {
    const wchar_t *msg = (what == AppCommands::CurrentImage::Streamed)
                                 ? Constants::Messages::IMAGE_IS_STREAMED
                                 : Constants::Messages::NO_IMAGE_ON_SCREEN;
    g_overlayManager.PostCenterMessage(hWnd, msg, OverlayManager::MsgSeverity::Warning);
}

// Ctrl+Shift+C — the full path of the image on screen, as text.
void AppCommands::CopyImagePathToClipboard(HWND hWnd) {
    std::wstring path;
    const CurrentImage what = GetCurrentImagePath(path);
    if (what != CurrentImage::Ok) {
        // This is a key the user just pressed, and a silent return is
        // indistinguishable from a copy that worked.
        ReportNoCurrentImage(hWnd, what);
        return;
    }

    if (!CopyTextToClipboard(hWnd, path)) {
        g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::CLIPBOARD_UNAVAILABLE,
                                           OverlayManager::MsgSeverity::Warning);
        return;
    }

    // The NAME, not the path, in the confirmation. The path is on the clipboard
    // where it was asked for; a full path in the centre of the screen wraps
    // across three lines and says nothing the user did not just ask for.
    const std::wstring name = path.substr(path.find_last_of(L"\\/") + 1);
    g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::COPIED_PATH_PREFIX + name);
}

bool AppCommands::RevealInExplorer(const std::wstring &path) {
    if (path.empty()) return false;

    // Checked here rather than trusted — see the header. GetFileAttributesW
    // rather than fs::exists: this is a single syscall on a path the caller
    // already has, and it answers for directories too, which the settings and
    // log reveals rely on.
    const DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return false;

    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str());
    if (!pidl) return false;

    SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
    ILFree(pidl);
    return true;
}

bool AppCommands::OpenPathWith(HWND hWnd, const std::wstring &path) {
    if (path.empty()) return false;

    OPENASINFO oi = {};
    oi.pcszFile = path.c_str();
    oi.pcszClass = nullptr; // choose by the file's own extension

    // OAIF_EXEC launches whatever the user picks. OAIF_HIDE_REGISTRATION hides
    // the "always use this app" checkbox ON PURPOSE:
    //
    // qIV is itself an image viewer, and for most users it IS the registered
    // handler for these extensions. A tick left in that box while glancing at a
    // one-off "open this in Paint" hands .jpg to Paint permanently, and the
    // symptom — double-clicking a picture stops opening qIV — appears later,
    // somewhere else, with nothing to connect it back to this dialog. Changing
    // file associations is Explorer's job and is one right-click away there.
    oi.oaifInFlags = OAIF_EXEC | OAIF_HIDE_REGISTRATION;

    // Modal, and deliberately so — the user asked for a chooser and is waiting
    // at it. Nothing is held across it: the caller passes a path BY VALUE, so
    // the playlist rebuilding underneath (a folder watcher tick, a remote
    // command) cannot leave this pointing at a freed string.
    const HRESULT hr = SHOpenWithDialog(hWnd, &oi);

    // PRESSING CANCEL IS A FAILURE HRESULT. SHOpenWithDialog answers
    // HRESULT_FROM_WIN32(ERROR_CANCELLED) when the user closes the chooser
    // without picking anything, so a plain SUCCEEDED() test reports "could not
    // open the chooser" every time someone changes their mind — an error
    // message for the ordinary way out of a dialog. Cancel is a normal ending
    // and says nothing.
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return true;

    return SUCCEEDED(hr);
}

// Ctrl+Shift+O — hand the picture on screen to another program.
void AppCommands::OpenCurrentImageWith(HWND hWnd) {
    std::wstring path;
    const CurrentImage what = GetCurrentImagePath(path);
    if (what != CurrentImage::Ok) {
        ReportNoCurrentImage(hWnd, what);
        return;
    }

    // A path by VALUE into the modal call — see OpenPathWith.
    if (!OpenPathWith(hWnd, path))
        g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::OPEN_WITH_FAILED,
                                           OverlayManager::MsgSeverity::Warning);
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

    // The attribute is per-window: without this, every panel already on screen
    // keeps the corners it was created with, and the toggle appears to work only
    // on the main window until each panel is closed and reopened.
    uiManager.NotifyCornerChanged();
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
    // THE SINGLE STOP CHOKEPOINT — the toggle, and the end-of-playlist auto-stop
    // in AppMain's WM_TIMER, both come through here, so one line covers every way
    // a show ends. Written FIRST, while app.slideshow still describes the show
    // that is ending; everything below this resets it.
    //
    // Guarded on `running` because this function is also reachable as a no-op
    // (a stop when nothing was playing), and a log that records shows which
    // never started is a log that cannot be trusted about the ones that did.
    //
    // The condition is checked before the string is built: with the log off this
    // costs one atomic load, which matters because a dedicated screen calls this
    // every time a playlist ends.
    if (app.slideshow.running && AppLog::IsEnabled()) {
        AppLog::Info(AppLog::COMP_SLIDESHOW,
                     L"stopped at image " + std::to_wstring(app.currentIndex + 1) +
                     L" of " + std::to_wstring(app.playlist.size()));
    }

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

// Clamped here as well as at every caller, because this is the last place the
// value passes through before it reaches SetTimer — and a zero or negative
// period would arm a timer that fires as fast as the message loop allows.
void AppCommands::applySlideshowInterval(HWND hWnd, int ms) {
    ms = std::max(Constants::Slideshow::INTERVAL_MIN_MS,
                  std::min(Constants::Slideshow::INTERVAL_MAX_MS, ms));
    app.slideshow.intervalMs = ms;

    // A PAUSED show is deliberately left alone. Its timer is already killed,
    // and resuming arms a fresh one from intervalMs — which is now the new
    // value. Re-arming here would restart a slideshow the user paused.
    if (app.slideshow.running && !app.slideshow.paused)
        SetTimer(hWnd, Constants::Slideshow::TIMER_ID, static_cast<UINT>(ms), nullptr);
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

        // Interval and playlist size are recorded, not just the fact of starting.
        // The screen this log exists for runs unattended, and "it is showing the
        // same picture all morning" is answered by the interval, while "it only
        // ever shows three of them" is answered by the count — neither is
        // recoverable after the fact from anywhere else.
        if (AppLog::IsEnabled()) {
            AppLog::Info(AppLog::COMP_SLIDESHOW,
                         L"started — " + std::to_wstring(app.playlist.size()) +
                         L" images, " + std::to_wstring(app.slideshow.intervalMs) +
                         L" ms interval" +
                         (app.slideshow.shuffle ? L", shuffled" : L""));
        }
    } else {
        // --- Stop (whether playing or paused) ---
        stopSlideshow(hWnd);
    }
}

void AppCommands::CopyImageToClipboard(HWND hWnd) {
    if (!app.wicFactory || app.currentIndex < 0 ||
        app.currentIndex >= static_cast<int>(app.playlist.size()))
        return;
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
        // The success path above has always announced itself, so a silent
        // failure here reads as "copied" — the one wrong conclusion to leave
        // the user with before they paste over something.
        g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::CLIPBOARD_UNAVAILABLE,
                                           OverlayManager::MsgSeverity::Warning);
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
