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
        float winW = (float)(rc.right - rc.left);
        float winH = (float)(rc.bottom - rc.top);

        // Compute the rendered image size exactly as the renderer does.
        float renderW = winW, renderH = winH; // fallback
        if (app.imgWidth > 0 && app.imgHeight > 0) {
            float imgW   = (float)app.imgWidth;
            float imgH   = (float)app.imgHeight;
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
                    renderH = imgH; if (renderH > winH) renderH = winH;
                    break;
                case Constants::ViewModes::ViewMode::FitToHeight_DoNotPreserveAspectRatio:
                    renderH = winH;
                    renderW = imgW; if (renderW > winW) renderW = winW;
                    break;
                case Constants::ViewModes::ViewMode::FitToWindow_DoNotPreserveAspectRatio:
                    renderW = winW; renderH = winH;
                    break;
                case Constants::ViewModes::ViewMode::OriginalImageSize_PreserveAspectRatio:
                    renderW = imgW; renderH = imgH;
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
        SetLmbCursor(imageOverflows);

        // Save state so ButtonUp can restore if we zoomed.
        app.savedZoom    = app.viewport.zoom;
        app.savedOffsetX = app.viewport.offsetX;
        app.savedOffsetY = app.viewport.offsetY;
        app.lmbDidZoom   = false;

        if (!imageOverflows) {
            // Image fits inside the viewport — apply the 3x click-zoom.
            float centerX = winW / 2.0f;
            float centerY = winH / 2.0f;
            float dx = (float)pt.x - centerX;
            float dy = (float)pt.y - centerY;

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
        app.viewport.lastMouse  = pt;
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
    } else if (IsViewControlAction(message)) {
        SetCursor(LoadCursor(nullptr, MAKEINTRESOURCEW(Constants::Cursors::DEFAULT)));

        // Only restore zoom/offset if we applied the click-zoom on press.
        // If the image was already overflowing and we only panned, keep
        // the current offset so the user's pan position is preserved.
        if (app.lmbDidZoom) {
            app.viewport.zoom    = app.savedZoom;
            app.viewport.offsetX = app.savedOffsetX;
            app.viewport.offsetY = app.savedOffsetY;
        }
        app.lmbDidZoom          = false;
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
        // Keep the appropriate cursor during drag (Windows resets it on every WM_MOUSEMOVE).
        SetLmbCursor(!app.lmbDidZoom); // lmbDidZoom=true means zoom mode → not overflowing

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
            float winW = (float)(rc.right - rc.left);
            float winH = (float)(rc.bottom - rc.top);

            float imgW = (float)app.imgWidth;
            float imgH = (float)app.imgHeight;
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
    } else {
        // Hover — no button held. Update LMB cursor so the user can see what
        // a click will do before they press the button.
        if (app.imgWidth > 0 && app.imgHeight > 0 && !app.isRmbDown) {
            RECT rc;
            GetClientRect(hWnd, &rc);
            float winW = (float)(rc.right - rc.left);
            float winH = (float)(rc.bottom - rc.top);
            float imgW   = (float)app.imgWidth;
            float imgH   = (float)app.imgHeight;
            float ratioX = winW / imgW;
            float ratioY = winH / imgH;
            float renderW, renderH;
            switch (app.viewMode) {
                case Constants::ViewModes::ViewMode::FitToView_PreserveAspectRatio:
                default:
                    renderW = imgW * std::min(ratioX, ratioY);
                    renderH = imgH * std::min(ratioX, ratioY);
                    break;
                case Constants::ViewModes::ViewMode::FitToWidth_DoNotPreserveAspectRatio:
                    renderW = winW; renderH = imgH; if (renderH > winH) renderH = winH;
                    break;
                case Constants::ViewModes::ViewMode::FitToHeight_DoNotPreserveAspectRatio:
                    renderH = winH; renderW = imgW; if (renderW > winW) renderW = winW;
                    break;
                case Constants::ViewModes::ViewMode::FitToWindow_DoNotPreserveAspectRatio:
                    renderW = winW; renderH = winH;
                    break;
                case Constants::ViewModes::ViewMode::OriginalImageSize_PreserveAspectRatio:
                    renderW = imgW; renderH = imgH;
                    break;
            }
            const float z = (app.viewport.zoom <= 0.0f) ? 1.0f : app.viewport.zoom;
            renderW *= z;
            renderH *= z;
            bool imageOverflows = (renderW > winW + 0.5f) || (renderH > winH + 0.5f);
            SetLmbCursor(imageOverflows);
        }
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
