// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#include "DpiAwareInit.h"
#include "Constants.h"
#include "../AppState.h"
#include "../Persistence/SessionFile.h" // LoadWindowRect — the remembered placement

#include <string>
#include <cwchar> // wcscmp — matching the stored display device name

namespace {
    // Passed through EnumDisplayMonitors, which cannot take a lambda capture.
    struct MonitorSearch {
        const wchar_t *wantedDevice = nullptr;
        RECT           work{};
        bool           found = false;
    };

    BOOL CALLBACK MatchMonitorProc(HMONITOR mon, HDC, LPRECT, LPARAM param) {
        auto *search = reinterpret_cast<MonitorSearch *>(param);

        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(mon, &mi)) return TRUE; // keep looking

        if (wcscmp(mi.szDevice, search->wantedDevice) != 0) return TRUE;

        search->work  = mi.rcWork;
        search->found = true;
        return FALSE; // stop
    }
}

// Is the named display still attached, and where is its work area now?
// Answered by enumeration rather than by remembering an HMONITOR: handles do
// not survive a process, let alone a display change.
bool FindMonitorWorkArea(const std::wstring &deviceName, RECT &outWork) {
    if (deviceName.empty()) return false;

    MonitorSearch search;
    search.wantedDevice = deviceName.c_str();
    EnumDisplayMonitors(nullptr, nullptr, MatchMonitorProc,
                        reinterpret_cast<LPARAM>(&search));

    if (!search.found) return false;
    outWork = search.work;
    return true;
}

HWND CreateViewerWindow(HINSTANCE hInstance, const wchar_t *className) {
    UINT sysDpi = GetDpiForSystem();
    int winW = MulDiv(app.baseWidth, sysDpi, 96);
    int winH = MulDiv(app.baseHeight, sysDpi, 96);

    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);

    int workW = mi.rcWork.right - mi.rcWork.left;
    int workH = mi.rcWork.bottom - mi.rcWork.top;

    HWND hWnd = CreateWindowExW(
            WS_EX_APPWINDOW,
            className, Constants::APP_TASKBAR_NAME,
            WS_POPUP,
            mi.rcWork.left + (workW - winW) / 2,
            mi.rcWork.top  + (workH - winH) / 2,
            winW, winH,
            nullptr, nullptr, hInstance, nullptr
            );

    // Step 2: If the actual monitor DPI differs, re-center within the work area.
    UINT actualDpi = GetDpiForWindow(hWnd);
    if (actualDpi != sysDpi) {
        int actualW = MulDiv(app.baseWidth, actualDpi, 96);
        int actualH = MulDiv(app.baseHeight, actualDpi, 96);

        GetMonitorInfoW(MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST), &mi);

        int posX = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - actualW) / 2;
        int posY = mi.rcWork.top  + (mi.rcWork.bottom - mi.rcWork.top  - actualH) / 2;

        SetWindowPos(hWnd, nullptr, posX, posY, actualW, actualH, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // Step 3: Restore the placement from the last exit, if the user asked for
    // that and the stored rect is one the user could actually reach.
    //
    // Applied LAST so the centred size above remains the fallback for every case
    // this declines: setting off, nothing stored, or a rect left behind by a
    // screen that has since been unplugged. Deliberately not clamped or nudged
    // onto the nearest monitor — a window that reappears somewhere the user did
    // not leave it is worse than one that opens centred, which is the behaviour
    // they had before switching the setting on.
    if (app.rememberWindowPosition) {
        int x = 0, y = 0, w = 0, h = 0;
        if (Persistence::Session::LoadWindowRect(x, y, w, h)) {
            // The screens may have been rearranged since the rect was written.
            // If the display it was on is still attached but no longer covers
            // those coordinates, honour the SCREEN rather than the numbers:
            // same size, centred on that monitor's work area.
            const std::wstring wantedDevice = Persistence::Session::LoadWindowMonitor();
            if (!wantedDevice.empty()) {
                RECT work{};
                if (FindMonitorWorkArea(wantedDevice, work)) {
                    const RECT stored{x, y, x + w, y + h};
                    RECT overlap{};
                    if (!IntersectRect(&overlap, &stored, &work)) {
                        // Named apart from the workW/workH above, which belong
                        // to the centred default and are still live here.
                        const int deviceW = work.right - work.left;
                        const int deviceH = work.bottom - work.top;
                        x = work.left + (deviceW - w) / 2;
                        y = work.top  + (deviceH - h) / 2;
                    }
                }
                // Device gone entirely: fall through. The rect is checked below
                // and, on a desktop that no longer has that screen, fails —
                // giving the centred default this function already applied.
            }

            if (IsUsableWindowRect(x, y, w, h)) {
                SetWindowPos(hWnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
            } else {
                // Keep the centred default above, and THROW THE BAD VALUE AWAY.
                // Left in place it would be re-read and re-rejected on every
                // launch for as long as the setting stays on, and the user would
                // have no way to tell that the feature was doing anything at
                // all. Discarding it means the next clean exit stores a good one
                // and the feature starts working again by itself.
                Persistence::Session::ClearWindowRect();
            }
        }
    }

    return hWnd;
}

bool IsUsableWindowRect(int x, int y, int width, int height) {
    // 1. A size the app is willing to accept at all.
    if (width  < Constants::WINDOW_SIZE_MIN || width  > Constants::WINDOW_SIZE_MAX) return false;
    if (height < Constants::WINDOW_SIZE_MIN || height > Constants::WINDOW_SIZE_MAX) return false;

    // 2. Not bigger than every screen put together. Legal as a typed setting on
    //    a huge desktop, nonsense as a restored rect on the machine in front of
    //    us — a display that was unplugged since the value was written.
    const int virtualW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int virtualH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (virtualW > 0 && width  > virtualW) return false;
    if (virtualH > 0 && height > virtualH) return false;

    // 3. Enough of it has to land somewhere the mouse can reach.
    const RECT stored{x, y, x + width, y + height};

    // MONITOR_DEFAULTTONULL, so a rect that is on no display at all fails here
    // rather than being silently snapped to the nearest one. MonitorFromRect
    // returns the monitor holding the LARGEST part of the rect, which is the one
    // worth measuring against.
    HMONITOR mon = MonitorFromRect(&stored, MONITOR_DEFAULTTONULL);
    if (!mon) return false;

    MONITORINFO mi = {sizeof(mi)};
    if (!GetMonitorInfoW(mon, &mi)) return false;

    RECT visible{};
    if (!IntersectRect(&visible, &stored, &mi.rcWork)) return false;

    if (visible.right - visible.left < Constants::WINDOW_MIN_VISIBLE_W) return false;
    if (visible.bottom - visible.top < Constants::WINDOW_MIN_VISIBLE_H) return false;

    return true;
}
