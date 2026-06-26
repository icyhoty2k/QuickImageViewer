#include "MouseHandler.h"
#include "AppState.h"
#include "Constants.h"
#include "WicDecoder.h"
#include <windowsx.h>
#include <algorithm>
#include <cmath>

extern AppState g_app;

static LPARAM s_lastClickParam = 0;

void MouseHandler::HandleClickStart(HWND hWnd, UINT message, LPARAM lParam) {
    s_lastClickParam = lParam;

    // 1. If it's a drag action, trigger it immediately (no timer)
    if (IsDragAction(message)) {
        HandleButtonDown(hWnd, message, lParam);
        return;
    }

    // 2. Otherwise, check if it's the zoom button
    bool isZoomButton = (Config::SWAP_MOUSE_BUTTONS ? (message == WM_RBUTTONDOWN) : (message == WM_LBUTTONDOWN));

    if (isZoomButton) {
        SetTimer(hWnd, 1, 150, nullptr);
    } else {
        HandleButtonDown(hWnd, message, lParam);
    }
}

void MouseHandler::HandleClickTimer(HWND hWnd) {
    KillTimer(hWnd, 1);
    if (GetKeyState(VK_LBUTTON) & 0x8000 || GetKeyState(VK_RBUTTON) & 0x8000) {
        // Trigger the zoom logic
        // We pass the saved param
        HandleButtonDown(hWnd, WM_LBUTTONDOWN, s_lastClickParam);
    }
}
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
        if (g_app.viewport.isDragging) return;
        SetCursor(NULL);

        // 1. Save state
        g_app.savedZoom = g_app.viewport.zoom;
        g_app.savedOffsetX = g_app.viewport.offsetX;
        g_app.savedOffsetY = g_app.viewport.offsetY;

        // 2. Get mouse position and window center
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        RECT rc;
        GetClientRect(hWnd, &rc);
        float centerX = (rc.right - rc.left) / 2.0f;
        float centerY = (rc.bottom - rc.top) / 2.0f;

        // 3. Calculate how far the mouse is from the center
        float dx = (float)pt.x - centerX;
        float dy = (float)pt.y - centerY;

        // 4. Apply temporary zoom
        g_app.viewport.zoom *= Config::ZOOM_CLICK;

        // 5. Shift the offset to keep the clicked point at the center
        // We adjust the offset by the distance moved, scaled by the zoom difference
        g_app.viewport.offsetX = (g_app.savedOffsetX - dx) ;
        g_app.viewport.offsetY = (g_app.savedOffsetY - dy) ;

        // 6. Start dragging
        g_app.viewport.lastMouse = pt;
        g_app.viewport.isDragging = true;
        SetCapture(hWnd);
        InvalidateRect(hWnd, nullptr, FALSE);
    }
}

void MouseHandler::HandleButtonUp(HWND hWnd, UINT message, LPARAM lParam) {
    if (message == WM_MBUTTONUP) {
        if (!g_app.hasMidMoved) {
            // 1. Reset Zoom and Pan
            g_app.viewport.zoom = 1.0f;
            g_app.viewport.offsetX = 0.0f;
            g_app.viewport.offsetY = 0.0f;

            // 2. CENTER WINDOW ON SCREEN
            int screenW = GetSystemMetrics(SM_CXSCREEN);
            int screenH = GetSystemMetrics(SM_CYSCREEN);

            RECT rc;
            GetWindowRect(hWnd, &rc);
            int winW = rc.right - rc.left;
            int winH = rc.bottom - rc.top;

            // Move the window to the exact center of the primary monitor
            SetWindowPos(hWnd, NULL,
                (screenW - winW) / 2,
                (screenH - winH) / 2,
                0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

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

        // Restore zoom and pan
        g_app.viewport.zoom = g_app.savedZoom;
        g_app.viewport.offsetX = g_app.savedOffsetX;
        g_app.viewport.offsetY = g_app.savedOffsetY;

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

        float dx = (float)(g_app.viewport.lastMouse.x - curMouse.x);
        float dy = (float)(g_app.viewport.lastMouse.y - curMouse.y);

        // Update Position using addition
        g_app.viewport.offsetX += dx;
        g_app.viewport.offsetY += dy;
        g_app.viewport.lastMouse = curMouse;

        // Constraint Logic
        if (g_app.imgWidth > 0 && g_app.imgHeight > 0) {
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
        }

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