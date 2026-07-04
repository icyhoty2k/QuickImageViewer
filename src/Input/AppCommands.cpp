// file: AppCommands.cpp
// header: AppCommands.h
#include "AppCommands.h"
#include "../../resources/resource.h"
#include <dwmapi.h>

#include "AppState.h" // Assuming this is the path
// ... include other necessary headers

extern AppState app;

void AppCommands::SaveImageToDisk(HWND hWnd) {
    if (!app.renderer || app.playlist.empty() || app.currentIndex < 0) return;

    const std::wstring &srcPath = app.playlist[app.currentIndex];

    // Build default filename: original basename + "_edited.png"
    std::wstring defaultName;
    {
        size_t slash = srcPath.find_last_of(L"\\/");
        std::wstring nameOnly = (slash != std::wstring::npos)
                                    ? srcPath.substr(slash + 1)
                                    : srcPath;
        size_t dot = nameOnly.find_last_of(L'.');
        if (dot != std::wstring::npos) nameOnly = nameOnly.substr(0, dot);
        defaultName = nameOnly + L"_edited.png";
    }

    wchar_t outBuf[MAX_PATH * 2] = {};
    wcsncpy_s(outBuf, defaultName.c_str(), _TRUNCATE);

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hWnd;
    ofn.lpstrFilter = L"PNG Image\0*.png\0All Files\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = outBuf;
    ofn.nMaxFile = ARRAYSIZE(outBuf);
    ofn.lpstrDefExt = L"png";
    ofn.lpstrTitle = L"Save image with effects";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    wchar_t initDir[MAX_PATH] = {};
    {
        size_t slash = srcPath.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            wcsncpy_s(initDir, srcPath.substr(0, slash).c_str(), _TRUNCATE);
    }
    ofn.lpstrInitialDir = initDir;

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
        DWORD corner = 1; // DWMWCP_DONOTROUND
        DwmSetWindowAttribute(hWnd, Constants::DWMWA_WINDOW_CORNER_PREFERENCES, &corner, sizeof(corner));
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
        DWORD corner = 2; // DWMWCP_ROUND
        DwmSetWindowAttribute(hWnd, Constants::DWMWA_WINDOW_CORNER_PREFERENCES, &corner, sizeof(corner));
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

    // Use your existing app icon
    nid.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_APP_ICON));
    wcscpy_s(nid.szTip, L"QuickImageViewer"); // Hover text

    Shell_NotifyIconW(NIM_ADD, &nid);
}

void AppCommands::RemoveTrayIcon(HWND hWnd) {
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = hWnd;
    nid.uID = ID_TRAY_APP_ICON;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}
