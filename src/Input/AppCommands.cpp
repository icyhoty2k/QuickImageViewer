// file: AppCommands.cpp
// header: AppCommands.h
#include "AppCommands.h"
#include "../../resources/resource.h"
#include <dwmapi.h>
#include <uxtheme.h>
#include <algorithm>
#include <numeric>
#include <random>
#include "AppState.h" // Assuming this is the path
#include "../Platform/Constants.h"
#include "../Platform/ConstantsStrings.h"
#include "../Overlays/OverlayManager.h"
#include "../WicDecoder.h"
#include <shlobj_core.h>

extern AppState app;

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

    std::wstring outBuf(Constants::MAX_FILE_PATH, L'\0');
    wcsncpy_s(outBuf.data(), Constants::MAX_FILE_PATH, defaultName.c_str(), _TRUNCATE);

    // Build the filter string from Constants::Save::FORMATS.
    // OPENFILENAMEW requires pairs of "Description\0*.ext\0" with a final \0.
    std::wstring filterStr;
    for (const auto &fmt : Constants::Save::FORMATS) {
        filterStr += fmt.description; filterStr += L'\0';
        filterStr += fmt.pattern;     filterStr += L'\0';
    }
    filterStr += L'\0';

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hWnd;
    ofn.lpstrFilter = filterStr.c_str();
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = outBuf.data();
    ofn.nMaxFile = Constants::MAX_FILE_PATH;
    ofn.lpstrDefExt = Constants::Save::DEFAULT_EXT;
    ofn.lpstrTitle = L"Save image";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    std::wstring initDir;
    {
        size_t slash = srcPath.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            initDir = srcPath.substr(0, slash);
    }
    ofn.lpstrInitialDir = initDir.empty() ? nullptr : initDir.c_str();

    if (GetSaveFileNameW(&ofn)) {
        HRESULT hr = app.renderer->SaveCurrentImageWithEffects(outBuf);
        if (FAILED(hr)) {
            wchar_t errBuf[128];
            swprintf_s(errBuf, L"Failed to save image.\nHRESULT: 0x%08X",
                       static_cast<unsigned>(hr));
            MessageBoxW(hWnd, errBuf, L"QuickImageViewer", MB_OK | MB_ICONERROR);
        }
    }
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

void AppCommands::AddTrayIcon(HWND hWnd) {
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = hWnd;
    nid.uID = ID_TRAY_APP_ICON;
    // NIF_ICON: Shows the icon
    // NIF_MESSAGE: Sends our custom WM_TRAYICON to our WndProc
    // NIF_TIP: Shows a tooltip on hover
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;

    UINT iconId = app.isDedicated ? IDI_APP_ICON_DEDICATED : IDI_APP_ICON;
    nid.hIcon = LoadIcon(GetModuleHandle(nullptr), MAKEINTRESOURCE(iconId));
    wcscpy_s(nid.szTip, app.isDedicated ? L"QuickImageViewer [Dedicated]" : L"QuickImageViewer");


    if (!Shell_NotifyIconW(NIM_MODIFY, &nid)) {
        if (!Shell_NotifyIconW(NIM_ADD, &nid))
            return;
    }
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}


void AppCommands::RemoveTrayIcon(HWND hWnd) {
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = hWnd;
    nid.uID = ID_TRAY_APP_ICON;
    Shell_NotifyIconW(NIM_DELETE, &nid);
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
    app.themeFactor = std::clamp(newFactor, 0.0f, 1.0f);

    // Auto-switch dark/light at the 0.5 midpoint
    bool shouldBeDark = app.themeFactor < 0.5f;
    if (app.isDarkThemed != shouldBeDark)
        changeAppThemeToDarkMode(hWnd, shouldBeDark);

    // Update renderer background color
    if (app.renderer)
        app.renderer->SetThemeFactor(app.themeFactor);

    // Repaint main window + all child panels
    InvalidateRect(hWnd, nullptr, FALSE);
    EnumChildWindows(hWnd, [](HWND hwnd, LPARAM) -> BOOL {
        InvalidateRect(hwnd, nullptr, FALSE);
        return TRUE;
    }, 0);
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
    // Restore overlay panels to their pre-slideshow state
    app.showOverlayInfoText = app.slideshow.savedOverlayVisible;
    g_overlayManager.SetAllVisible(app.showOverlayInfoText);
    InvalidateRect(hWnd, nullptr, FALSE);

    app.slideshow.running = false;
    app.slideshow.paused  = false;
    app.slideshow.shuffleOrder.clear();
    app.slideshow.shufflePos = 0;
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
        app.slideshow.paused  = false;
    } else {
        // --- Stop (whether playing or paused) ---
        stopSlideshow(hWnd);
    }
}

void AppCommands::CopyImageToClipboard(HWND hWnd) {
    if (app.playlist.empty() || app.currentIndex < 0 || !app.wicFactory) return;
    const std::wstring& path = app.playlist[app.currentIndex];

    DecodedImage img;
    if (FAILED(WicDecoder::DecodeImage(path, img)) || !img.bitmap) return;

    Microsoft::WRL::ComPtr<IWICFormatConverter> conv;
    if (FAILED(app.wicFactory->CreateFormatConverter(&conv))) return;
    if (FAILED(conv->Initialize(img.bitmap.Get(), GUID_WICPixelFormat32bppBGR,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return;

    UINT w = img.width, h = img.height;
    UINT stride = w * 4;

    // Create a top-down DIB section and copy WIC pixels directly into it.
    // Putting CF_BITMAP on the clipboard lets Windows synthesize CF_DIB and
    // CF_DIBV5 automatically — paste targets receive a standard bottom-up DIB
    // regardless of whether they request CF_BITMAP or CF_DIB.
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = static_cast<LONG>(w);
    bi.bmiHeader.biHeight      = -static_cast<LONG>(h); // top-down matches WIC pixel order
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC hdc = GetDC(nullptr);
    HBITMAP hBmp = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (!hBmp || !bits) return;

    if (FAILED(conv->CopyPixels(nullptr, stride, stride * h, static_cast<BYTE*>(bits)))) {
        DeleteObject(hBmp);
        return;
    }

    // Build CF_HDROP so Ctrl+V in Explorer pastes the file itself.
    const std::wstring& filePath = path;
    SIZE_T pathBytes = (filePath.size() + 2) * sizeof(wchar_t); // path + \0 + list \0
    HGLOBAL hDrop = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + pathBytes);
    if (!hDrop) { DeleteObject(hBmp); return; }
    auto* df = static_cast<DROPFILES*>(GlobalLock(hDrop));
    if (!df) { GlobalFree(hDrop); DeleteObject(hBmp); return; }
    df->pFiles = sizeof(DROPFILES);
    df->fWide  = TRUE;
    memcpy(reinterpret_cast<BYTE*>(df) + sizeof(DROPFILES),
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
