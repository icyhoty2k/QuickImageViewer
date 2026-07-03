#include "MouseHandler.h"
#include "AppState.h"
#include "../Platform/Constants.h"
#include "../Platform/FileHandler.h"
#include "WicDecoder.h"
#include <windowsx.h>
#include <algorithm>
#include <shlobj_core.h>
#include <shtypes.h>

extern AppState app;


void MouseHandler::HandleButtonDown(HWND hWnd, UINT message, LPARAM lParam) {
    // Track RMB state
    if (message == WM_RBUTTONDOWN) {
        app.isRmbDown = true;
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
        SetCursor(NULL);

        // 1. Save state
        app.savedZoom = app.viewport.zoom;
        app.savedOffsetX = app.viewport.offsetX;
        app.savedOffsetY = app.viewport.offsetY;

        // 2. Get mouse position and window center
        POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        RECT rc;
        GetClientRect(hWnd, &rc);
        float centerX = (rc.right - rc.left) / 2.0f;
        float centerY = (rc.bottom - rc.top) / 2.0f;

        // 3. Calculate how far the mouse is from the center
        float dx = (float) pt.x - centerX;
        float dy = (float) pt.y - centerY;

        // 4. Apply temporary zoom
        app.viewport.zoom *= Constants::ZOOM_CLICK;

        // 5. Shift the offset to keep the clicked point at the center
        // We adjust the offset by the distance moved, scaled by the zoom difference
        app.viewport.offsetX = (app.savedOffsetX - dx);
        app.viewport.offsetY = (app.savedOffsetY - dy);

        // 6. Start dragging
        app.viewport.lastMouse = pt;
        app.viewport.isDragging = true;
        SetCapture(hWnd);
        InvalidateRect(hWnd, nullptr, FALSE);
    }
}

void MouseHandler::HandleButtonUp(HWND hWnd, UINT message, LPARAM /*lParam*/) {
    if (message == WM_RBUTTONUP) {
        app.isRmbDown = false;
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
    } else if (IsViewControlAction(message)) {
        SetCursor(LoadCursor(nullptr, IDC_ARROW));

        // Restore zoom and pan
        app.viewport.zoom = app.savedZoom;
        app.viewport.offsetX = app.savedOffsetX;
        app.viewport.offsetY = app.savedOffsetY;

        app.viewport.isDragging = false;
        ReleaseCapture();
        InvalidateRect(hWnd, nullptr, FALSE);
    }
}

void MouseHandler::HandleMouseMove(HWND hWnd, LPARAM lParam) {
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
        POINT curMouse = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

        float dx = (float) (app.viewport.lastMouse.x - curMouse.x);
        float dy = (float) (app.viewport.lastMouse.y - curMouse.y);

        // Update Position using addition
        app.viewport.offsetX += dx;
        app.viewport.offsetY += dy;
        app.viewport.lastMouse = curMouse;

        // Constraint Logic
        if (app.imgWidth > 0 && app.imgHeight > 0) {
            RECT rc;
            GetClientRect(hWnd, &rc);
            float winW = (float) (rc.right - rc.left);
            float winH = (float) (rc.bottom - rc.top);

            float base = std::min(winW / (float) app.imgWidth, winH / (float) app.imgHeight);
            float renderW = (float) app.imgWidth * base * app.viewport.zoom;
            float renderH = (float) app.imgHeight * base * app.viewport.zoom;

            float maxOffX = std::max(0.0f, (renderW - winW) / 2.0f);
            float maxOffY = std::max(0.0f, (renderH - winH) / 2.0f);

            app.viewport.offsetX = std::max(-maxOffX, std::min(maxOffX, app.viewport.offsetX));
            app.viewport.offsetY = std::max(-maxOffY, std::min(maxOffY, app.viewport.offsetY));
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
    if (app.playlist.empty()) return;

    // Check if RMB is held down (0x8000 indicates the key is down)
    bool isRmbDown = (GetKeyState(VK_RBUTTON) & 0x8000) != 0;

    int zDelta = GET_WHEEL_DELTA_WPARAM(wParam);

    if (isRmbDown) {
        // Zoom logic when RMB is held
        app.viewport.zoom *= (zDelta > 0) ? Constants::ZOOM_STEP : (1.0f / Constants::ZOOM_STEP);
        InvalidateRect(hWnd, nullptr, FALSE);
    } else if (GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) {
        // Existing Ctrl+Scroll zoom logic
        app.viewport.zoom *= (zDelta > 0) ? Constants::ZOOM_STEP : (1.0f / Constants::ZOOM_STEP);
        InvalidateRect(hWnd, nullptr, FALSE);
    } else {
        // Default: Image navigation
        int step = (zDelta < 0) ? 1 : -1;
        int newIdx = (app.currentIndex + step + (int) app.playlist.size()) % (int) app.playlist.size();
        LoadImageIndex(hWnd, newIdx);
        InvalidateRect(hWnd, nullptr, FALSE);
    }
}
