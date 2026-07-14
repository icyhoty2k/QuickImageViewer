#include "MouseHandler.h"
#include "AppState.h"
#include "AppCommands.h"
#include "../Platform/Constants.h"
#include "../Platform/ConstantsStrings.h"
#include "../Platform/FileHandler.h"
#include "../Overlays/OverlayManager.h"
#include "../UI/HistoryListWnd.h"
#include "../UI/UIManager.h"
#include "WicDecoder.h"
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include <shlobj_core.h>
#include <shtypes.h>

extern AppState app;


void MouseHandler::HandleButtonDown(HWND hWnd, UINT message, LPARAM lParam) {
    // Track RMB state
    if (message == WM_RBUTTONDOWN) {
        app.isRmbDown = true;
        SetCursor(LoadCursor(nullptr, MAKEINTRESOURCEW(Constants::Cursors::RMB_DOWN)));
    }

    // New logic: If RMB is down and we receive a Left Click
    if (message == WM_LBUTTONDOWN && app.isRmbDown) {
        if (!app.playlist.empty() && app.currentIndex >= 0) {
            const std::wstring &path = app.playlist[app.currentIndex];
            PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str());
            if (pidl) {
                app.isRmbDown = false;
                SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
                ILFree(pidl);
            }
        }
        return; // Handled
    }
    if (message == WM_MBUTTONDOWN) {
        app.isMidDragging = true;
        app.hasMidMoved = false;
        POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ClientToScreen(hWnd, &pt);
        app.lastMidMouse = pt;
        SetCapture(hWnd);
        return;
    }

    if (IsDragAction(message)) {
        if (app.isFullscreen) return;
        app.isWindowDragging = true;
        POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ClientToScreen(hWnd, &pt);
        app.lastWindowMouse = pt;
        SetCapture(hWnd);
    } else if (IsViewControlAction(message)) {
        if (app.viewport.isDragging) return;

        POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        RECT rc;
        GetClientRect(hWnd, &rc);
        float winW = (float) (rc.right - rc.left);
        float winH = (float) (rc.bottom - rc.top);

        // Compute the rendered image size exactly as the renderer does.
        float renderW = winW, renderH = winH; // fallback
        if (app.imgWidth > 0 && app.imgHeight > 0) {
            float imgW = (float) app.imgWidth;
            float imgH = (float) app.imgHeight;
            float ratioX = winW / imgW;
            float ratioY = winH / imgH;
            switch (app.viewMode) {
                case Constants::ViewModes::ViewMode::FitToView_PreserveAspectRatio:
                default:
                    renderW = imgW * std::min(ratioX, ratioY);
                    renderH = imgH * std::min(ratioX, ratioY);
                    break;
                case Constants::ViewModes::ViewMode::FitToWidth_DoNotPreserveAspectRatio:
                    renderW = winW;
                    renderH = imgH;
                    if (renderH > winH) renderH = winH;
                    break;
                case Constants::ViewModes::ViewMode::FitToHeight_DoNotPreserveAspectRatio:
                    renderH = winH;
                    renderW = imgW;
                    if (renderW > winW) renderW = winW;
                    break;
                case Constants::ViewModes::ViewMode::FitToWindow_DoNotPreserveAspectRatio:
                    renderW = winW;
                    renderH = winH;
                    break;
                case Constants::ViewModes::ViewMode::OriginalImageSize_PreserveAspectRatio:
                    renderW = imgW;
                    renderH = imgH;
                    break;
            }
        }
        const float z = (app.viewport.zoom <= 0.0f) ? 1.0f : app.viewport.zoom;
        renderW *= z;
        renderH *= z;

        // If the image already overflows the viewport in either axis, skip the
        // click-zoom and go straight to pan mode.
        bool imageOverflows = (renderW > winW + 0.5f) || (renderH > winH + 0.5f);

        // Show the appropriate cursor so the user knows what LMB will do.
        WORD cursorId = imageOverflows ? Constants::Cursors::LMB_PAN : Constants::Cursors::LMB_ZOOM;
        SetCursor(LoadCursor(nullptr, MAKEINTRESOURCEW(cursorId)));

        // Save state so ButtonUp can restore if we zoomed.
        app.savedZoom = app.viewport.zoom;
        app.savedOffsetX = app.viewport.offsetX;
        app.savedOffsetY = app.viewport.offsetY;
        app.lmbDidZoom = false;

        if (!imageOverflows) {
            // Image fits inside the viewport — apply the 3x click-zoom.
            float centerX = winW / 2.0f;
            float centerY = winH / 2.0f;
            float dx = (float) pt.x - centerX;
            float dy = (float) pt.y - centerY;

            app.viewport.zoom *= Constants::ZOOM_CLICK;

            // Keep the clicked pixel under the cursor.
            // Derivation: renderer puts image center at (winW/2 + offsetX).
            // Pixel at dx from window center is (dx - oldOffsetX) from image center.
            // After zoom*Z it moves to Z*(dx - oldOffsetX). To keep it at dx:
            //   newOffsetX = dx*(1 - Z) + Z*oldOffsetX
            const float Z = Constants::ZOOM_CLICK;
            app.viewport.offsetX = dx * (1.0f - Z) + Z * app.savedOffsetX;
            app.viewport.offsetY = dy * (1.0f - Z) + Z * app.savedOffsetY;

            app.lmbDidZoom = true;
            InvalidateRect(hWnd, nullptr, FALSE);
        }

        // Always start pan (drag) mode.
        app.viewport.lastMouse = pt;
        app.viewport.isDragging = true;
        SetCapture(hWnd);
    }
}

void MouseHandler::HandleButtonUp(HWND hWnd, UINT message, LPARAM /*lParam*/) {
    if (message == WM_RBUTTONUP) {
        app.isRmbDown = false;
        SetCursor(LoadCursor(nullptr, MAKEINTRESOURCEW(Constants::Cursors::DEFAULT)));
    }
    if (message == WM_MBUTTONUP) {
        if (!app.hasMidMoved) {
            // 1. Reset Zoom and Pan
            app.viewport.zoom = 1.0f;
            app.viewport.offsetX = 0.0f;
            app.viewport.offsetY = 0.0f;

            // 3. RESTORE OPACITY: Reset to full (255)
            app.opacity = 255;
            SetLayeredWindowAttributes(hWnd, 0, app.opacity, LWA_ALPHA);

            // 2. Calculate DPI-scaled dimensions
            int targetW = (int) (Constants::BASE_WIDTH * app.dpiScale);
            int targetH = (int) (Constants::BASE_HEIGHT * app.dpiScale);

            // 4. Center and RESIZE the window
            HMONITOR hMonitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = {sizeof(mi)};
            if (GetMonitorInfo(hMonitor, &mi)) {
                int monitorW = mi.rcMonitor.right - mi.rcMonitor.left;
                int monitorH = mi.rcMonitor.bottom - mi.rcMonitor.top;

                SetWindowPos(hWnd, NULL,
                             mi.rcMonitor.left + (monitorW - targetW) / 2,
                             mi.rcMonitor.top + (monitorH - targetH) / 2,
                             targetW, targetH,
                             SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            }

            InvalidateRect(hWnd, nullptr, FALSE);
        }
        app.isMidDragging = false;
        app.hasMidMoved = false;
        ReleaseCapture();
        return;
    }

    if (IsDragAction(message)) {
        app.isWindowDragging = false;
        ReleaseCapture();

        // Edge-snap: if the cursor landed near a screen edge, snap the window
        // to the corresponding half/full work-area zone (mirrors Aero Snap).
        if (!app.isFullscreen) {
            POINT cursor;
            GetCursorPos(&cursor);
            HMONITOR hMon = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = { sizeof(mi) };
            if (GetMonitorInfo(hMon, &mi)) {
                const RECT &wa  = mi.rcWork;
                const int snap  = Constants::WINDOW_SNAP_DISTANCE;
                bool nearLeft   = cursor.x <= wa.left  + snap;
                bool nearRight  = cursor.x >= wa.right  - snap;
                bool nearTop    = cursor.y <= wa.top    + snap;
                int halfW = (wa.right  - wa.left) / 2;
                int halfH = (wa.bottom - wa.top)  / 2;
                RECT target = {};
                bool doSnap = false;
                if      (nearLeft  && nearTop) { target = { wa.left,        wa.top, wa.left + halfW, wa.top + halfH }; doSnap = true; }
                else if (nearRight && nearTop) { target = { wa.left + halfW, wa.top, wa.right,        wa.top + halfH }; doSnap = true; }
                else if (nearLeft)             { target = { wa.left,        wa.top, wa.left + halfW, wa.bottom       }; doSnap = true; }
                else if (nearRight)            { target = { wa.left + halfW, wa.top, wa.right,        wa.bottom       }; doSnap = true; }
                else if (nearTop)              { target = wa;                                                            doSnap = true; }
                if (doSnap) {
                    SetWindowPos(hWnd, nullptr,
                                 target.left, target.top,
                                 target.right  - target.left,
                                 target.bottom - target.top,
                                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
                    InvalidateRect(hWnd, nullptr, FALSE);
                }
            }
        }
    } else if (IsViewControlAction(message)) {
        SetCursor(LoadCursor(nullptr, MAKEINTRESOURCEW(Constants::Cursors::DEFAULT)));

        // Only restore zoom/offset if we applied the click-zoom on press.
        // If the image was already overflowing and we only panned, keep
        // the current offset so the user's pan position is preserved.
        if (app.lmbDidZoom) {
            app.viewport.zoom = app.savedZoom;
            app.viewport.offsetX = app.savedOffsetX;
            app.viewport.offsetY = app.savedOffsetY;
        }
        app.lmbDidZoom = false;
        app.viewport.isDragging = false;
        ReleaseCapture();
        InvalidateRect(hWnd, nullptr, FALSE);
    }
}

void MouseHandler::HandleMouseMove(HWND hWnd, LPARAM lParam) {
    if (app.slideshow.running) {
        if (app.slideshow.cursorHidden) {
            ShowCursor(TRUE);
            app.slideshow.cursorHidden = false;
        }
        if (app.slideshow.cursorHideMs > 0 && !app.slideshow.paused) {
            KillTimer(hWnd, Constants::Slideshow::CURSOR_TIMER_ID);
            SetTimer(hWnd, Constants::Slideshow::CURSOR_TIMER_ID, app.slideshow.cursorHideMs, nullptr);
        }
    }

    if (app.isMidDragging) {
        POINT curMouse = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ClientToScreen(hWnd, &curMouse);
        if (!app.isFullscreen) {
            app.hasMidMoved = true;
            int dx = curMouse.x - app.lastMidMouse.x;
            int dy = curMouse.y - app.lastMidMouse.y;
            RECT rc;
            GetWindowRect(hWnd, &rc);
            int newW = std::max(200, static_cast<int>((rc.right - rc.left) + dx));
            int newH = std::max(150, static_cast<int>((rc.bottom - rc.top) + dy));
            SetWindowPos(hWnd, nullptr, 0, 0, newW, newH,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
        }
        app.lastMidMouse = curMouse;
        InvalidateRect(hWnd, nullptr, FALSE);
    } else if (app.viewport.isDragging) {
        // Keep the appropriate cursor during drag (Windows resets it on every move).
        WORD cursorId = app.lmbDidZoom ? Constants::Cursors::LMB_ZOOM : Constants::Cursors::LMB_PAN;
        SetCursor(LoadCursor(nullptr, MAKEINTRESOURCEW(cursorId)));

        POINT curMouse = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

        float dx = (float) (app.viewport.lastMouse.x - curMouse.x);
        float dy = (float) (app.viewport.lastMouse.y - curMouse.y);

        // Update Position using addition
        app.viewport.offsetX += dx;
        app.viewport.offsetY += dy;
        app.viewport.lastMouse = curMouse;

        // Constraint — must mirror the renderer's renderW/renderH exactly so the
        // pan limit matches the actual drawn edges of the image.
        if (app.imgWidth > 0 && app.imgHeight > 0) {
            RECT rc;
            GetClientRect(hWnd, &rc);
            float winW = (float) (rc.right - rc.left);
            float winH = (float) (rc.bottom - rc.top);

            float imgW = (float) app.imgWidth;
            float imgH = (float) app.imgHeight;
            float ratioX = winW / imgW;
            float ratioY = winH / imgH;

            // Match renderer viewMode logic
            float renderW, renderH;
            switch (app.viewMode) {
                case Constants::ViewModes::ViewMode::FitToView_PreserveAspectRatio:
                default:
                    renderW = imgW * std::min(ratioX, ratioY);
                    renderH = imgH * std::min(ratioX, ratioY);
                    break;
                case Constants::ViewModes::ViewMode::FitToWidth_DoNotPreserveAspectRatio:
                    renderW = winW;
                    renderH = imgH;
                    if (renderH > winH) renderH = winH;
                    break;
                case Constants::ViewModes::ViewMode::FitToHeight_DoNotPreserveAspectRatio:
                    renderH = winH;
                    renderW = imgW;
                    if (renderW > winW) renderW = winW;
                    break;
                case Constants::ViewModes::ViewMode::FitToWindow_DoNotPreserveAspectRatio:
                    renderW = winW;
                    renderH = winH;
                    break;
                case Constants::ViewModes::ViewMode::OriginalImageSize_PreserveAspectRatio:
                    renderW = imgW;
                    renderH = imgH;
                    break;
            }

            const float z = (app.viewport.zoom <= 0.0f) ? 1.0f : app.viewport.zoom;
            renderW *= z;
            renderH *= z;

            float maxOffX = std::max(0.0f, (renderW - winW) / 2.0f);
            float maxOffY = std::max(0.0f, (renderH - winH) / 2.0f);

            app.viewport.offsetX = std::clamp(app.viewport.offsetX, -maxOffX, maxOffX);
            app.viewport.offsetY = std::clamp(app.viewport.offsetY, -maxOffY, maxOffY);
        }

        InvalidateRect(hWnd, nullptr, FALSE);
    } else if (app.isWindowDragging) {
        POINT curMouse = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ClientToScreen(hWnd, &curMouse);
        int dx = curMouse.x - app.lastWindowMouse.x;
        int dy = curMouse.y - app.lastWindowMouse.y;
        RECT rc;
        GetWindowRect(hWnd, &rc);
        SetWindowPos(hWnd, nullptr, rc.left + dx, rc.top + dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        app.lastWindowMouse = curMouse;
    }
}

void MouseHandler::HandleMouseWheel(HWND hWnd, WPARAM wParam, LPARAM /*lParam*/) {
    const int rawDelta = GET_WHEEL_DELTA_WPARAM(wParam);
    const int delta = Constants::MOUSE_VERTICAL_REVERSE_SCROLL_DIRECTION ? -rawDelta : rawDelta;

    if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
        app.opacity = static_cast<BYTE>(
            delta > 0
                ? std::min(255, (int)app.opacity + Constants::OPACITY_STEP)
                : std::max(10,  (int)app.opacity - Constants::OPACITY_STEP));
        SetLayeredWindowAttributes(hWnd, 0, app.opacity, LWA_ALPHA);
        return;
    }

    if (app.playlist.empty()) return;

    bool isRmbDown = (GetKeyState(VK_RBUTTON) & 0x8000) != 0;

    if (isRmbDown) {
        app.viewport.zoom *= (delta > 0) ? Constants::ZOOM_STEP : (1.0f / Constants::ZOOM_STEP);
        InvalidateRect(hWnd, nullptr, FALSE);
    } else if (GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) {
        app.viewport.zoom *= (delta > 0) ? Constants::ZOOM_STEP : (1.0f / Constants::ZOOM_STEP);
        InvalidateRect(hWnd, nullptr, FALSE);
    } else {
        int step = (delta < 0) ? 1 : -1;
        int newIdx = (app.currentIndex + step + (int) app.playlist.size()) % (int) app.playlist.size();
        LoadImageIndex(hWnd, newIdx);
        InvalidateRect(hWnd, nullptr, FALSE);
    }
}

void MouseHandler::HandleMouseHWheel(HWND hWnd, WPARAM wParam, LPARAM /*lParam*/) {
    bool isRmbDown = (GetKeyState(VK_RBUTTON) & 0x8000) != 0;
    const int rawDelta = GET_WHEEL_DELTA_WPARAM(wParam);
    const int hDelta = Constants::MOUSE_HORIZONTAL_REVERSE_SCROLL_DIRECTION ? -rawDelta : rawDelta;

    if (isRmbDown) {
        RECT rc;
        GetWindowRect(hWnd, &rc);
        int currentW = rc.right - rc.left;
        int currentH = rc.bottom - rc.top;

        int resizeStep = (hDelta > 0) ? 20 : -20;
        int newW = currentW + resizeStep;
        int newH = static_cast<int>(std::round(
                currentH + resizeStep * (static_cast<float>(currentH) / currentW)));
        int newX = rc.left - (resizeStep / 2);
        int newY = rc.top  - (resizeStep / 2);

        SetWindowPos(hWnd, nullptr, newX, newY, newW, newH,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
        InvalidateRect(hWnd, nullptr, FALSE);
        return;
    }

    // Plain horizontal scroll: cycle through folder history
    static int s_histIdx = 0;
    static std::vector<std::wstring> s_histSnap;
    static std::wstring s_lastNavFolder;
    static int s_accumulator = 0;

    const auto &history = UI::GetFolderHistory();
    if (history.empty()) return;

    // Reset snapshot whenever the current folder changed via non-scroll means
    if (s_histSnap.empty() || history[0] != s_lastNavFolder) {
        s_histSnap.assign(history.begin(), history.end());
        s_histIdx = 0;
        s_lastNavFolder = history[0];
        s_accumulator = 0;
    }

    s_accumulator += hDelta;
    const int threshold = WHEEL_DELTA * Constants::MOUSE_HSCROLL_FOLDER_TICKS;
    if (std::abs(s_accumulator) < threshold) return;

    const int total = (int)s_histSnap.size();
    if (s_accumulator > 0) s_histIdx = (s_histIdx + 1) % total;
    else                    s_histIdx = (s_histIdx - 1 + total) % total;
    s_accumulator = 0;

    const std::wstring &folder = s_histSnap[s_histIdx];

    std::wstring displayName = folder;
    const size_t sep = folder.find_last_of(L"\\/");
    if (sep != std::wstring::npos) displayName = folder.substr(sep + 1);

    if (!UI::IsFolderValidForViewer(folder)) {
        // Dead folder — show warning but do not open; do not update s_lastNavFolder.
        auto &histWnd = uiManager.getHistoryListWindow();
        if (histWnd.IsVisible())
            InvalidateRect(histWnd.GetHwnd(), nullptr, FALSE);
        g_overlayManager.PostCenterMessage(hWnd,
            std::wstring(L"⚠ ") +
            std::to_wstring(s_histIdx + 1) + L"/" + std::to_wstring(total) + L" " + displayName);
        return;
    }

    OpenDirectory(hWnd, folder);
    s_lastNavFolder = folder;
    s_histSnap.clear(); // Reset snapshot so next scroll recaptures updated history order

    g_overlayManager.PostCenterMessage(hWnd,
        Constants::Messages::HISTORY_NAV_FOLDER +
        std::to_wstring(s_histIdx + 1) + L"/" + std::to_wstring(total) + L" " + displayName);
}

void MouseHandler::HandleDoubleClick(HWND hWnd) {
    SendMessageW(hWnd, WM_SETREDRAW, FALSE, 0);
    AppCommands::ToggleFullscreen(hWnd);
    SendMessageW(hWnd, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(hWnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_FRAME);
}
