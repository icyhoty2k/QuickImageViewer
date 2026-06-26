#include "MouseHandler.h"
#include "AppState.h"
#include "Constants.h"
#include "WicDecoder.h"
#include <windowsx.h>
#include <algorithm>

extern AppState g_app;

void MouseHandler::HandleButtonDown(HWND hWnd, UINT message, LPARAM lParam) {
    if (message == WM_MBUTTONDOWN) {
        g_app.isMidDragging = true;
        g_app.hasMidMoved = false;
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ClientToScreen(hWnd, &pt);
        g_app.lastMidMouse = pt;
        SetCapture(hWnd);
        return;
    }

    if (IsDragAction(message)) {
        if (g_app.isFullscreen) return;
        g_app.isWindowDragging = true;
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ClientToScreen(hWnd, &pt);
        g_app.lastWindowMouse = pt;
        SetCapture(hWnd);
    }
    else if (IsViewControlAction(message)) {
        SetCursor(NULL);
        g_app.savedZoom = g_app.viewport.zoom;
        // Do NOT save offsetX/Y here if we want to keep the current position as the base
        g_app.viewport.zoom *= Config::ZOOM_CLICK;
        g_app.viewport.lastMouse = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        g_app.viewport.isDragging = true;
        SetCapture(hWnd);
        InvalidateRect(hWnd, nullptr, FALSE);
    }
}

void MouseHandler::HandleButtonUp(HWND hWnd, UINT message, LPARAM lParam) {
    if (message == WM_MBUTTONUP) {
        if (!g_app.hasMidMoved) {
            g_app.viewport.zoom = 1.0f;
            g_app.viewport.offsetX = 0.0f;
            g_app.viewport.offsetY = 0.0f;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        g_app.isMidDragging = false;
        g_app.hasMidMoved = false;
        ReleaseCapture();
        return;
    }

    if (IsDragAction(message)) {
        g_app.isWindowDragging = false;
        ReleaseCapture();
    }
    else if (IsViewControlAction(message)) {
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        // Reset ONLY the zoom level, keep the new pan offsets!
        g_app.viewport.zoom = g_app.savedZoom;
        g_app.viewport.isDragging = false;
        ReleaseCapture();
        InvalidateRect(hWnd, nullptr, FALSE);
    }
}

void MouseHandler::HandleMouseMove(HWND hWnd, LPARAM lParam) {
    if (g_app.isMidDragging) {
        POINT curMouse = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ClientToScreen(hWnd, &curMouse);
        g_app.hasMidMoved = true;
        if (!g_app.isFullscreen) {
            int dx = curMouse.x - g_app.lastMidMouse.x;
            int dy = curMouse.y - g_app.lastMidMouse.y;
            RECT rc; GetWindowRect(hWnd, &rc);
            SetWindowPos(hWnd, nullptr, 0, 0, (rc.right - rc.left) + dx, (rc.bottom - rc.top) + dy,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
        }
        g_app.lastMidMouse = curMouse;
        InvalidateRect(hWnd, nullptr, FALSE);
    }
    else if (g_app.viewport.isDragging) {
        POINT curMouse = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

        // Calculate raw movement
        float dx = (float)(curMouse.x - g_app.viewport.lastMouse.x);
        float dy = (float)(curMouse.y - g_app.viewport.lastMouse.y);

        // Update offsets by delta
        g_app.viewport.offsetX -= dx;
        g_app.viewport.offsetY -= dy;
        g_app.viewport.lastMouse = curMouse;

        // Constraint Logic: Ensure the viewport stays within bounds
        RECT rc; GetClientRect(hWnd, &rc);
        float winW = (float)(rc.right - rc.left);
        float winH = (float)(rc.bottom - rc.top);

        float base = std::min(winW / (float)g_app.imgWidth, winH / (float)g_app.imgHeight);
        float renderW = (float)g_app.imgWidth * base * g_app.viewport.zoom;
        float renderH = (float)g_app.imgHeight * base * g_app.viewport.zoom;

        float maxOffX = std::max(0.0f, (renderW - winW) / 2.0f);
        float maxOffY = std::max(0.0f, (renderH - winH) / 2.0f);

        g_app.viewport.offsetX = std::max(-maxOffX, std::min(maxOffX, g_app.viewport.offsetX));
        g_app.viewport.offsetY = std::max(-maxOffY, std::min(maxOffY, g_app.viewport.offsetY));

        InvalidateRect(hWnd, nullptr, FALSE);
    }
    else if (g_app.isWindowDragging) {
        POINT curMouse = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ClientToScreen(hWnd, &curMouse);
        int dx = curMouse.x - g_app.lastWindowMouse.x;
        int dy = curMouse.y - g_app.lastWindowMouse.y;
        RECT rc; GetWindowRect(hWnd, &rc);
        SetWindowPos(hWnd, nullptr, rc.left + dx, rc.top + dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        g_app.lastWindowMouse = curMouse;
    }
}

void MouseHandler::HandleMouseWheel(HWND hWnd, WPARAM wParam, LPARAM lParam) {
    if (g_app.playlist.empty()) return;
    int zDelta = GET_WHEEL_DELTA_WPARAM(wParam);
    if (GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) {
        g_app.viewport.zoom *= (zDelta > 0) ? Config::ZOOM_STEP : (1.0f / Config::ZOOM_STEP);
    } else {
        int step = (zDelta < 0) ? 1 : -1;
        int newIdx = (g_app.currentIndex + step + (int)g_app.playlist.size()) % (int)g_app.playlist.size();
        LoadImageIndex(hWnd, newIdx);
    }
    InvalidateRect(hWnd, nullptr, FALSE);
}