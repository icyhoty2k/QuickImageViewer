#include "MouseHandler.h"
#include "AppState.h"
#include "AppCommands.h"
#include "ContextMenuHandler.h"
#include "../Platform/Constants.h"
#include "../Platform/ConstantsStrings.h"
#include "../Platform/FileHandler.h"
#include "../Overlays/OverlayManager.h"
#include "../UI/FloatingPanels/HistoryListWnd.h"
#include "../UI/UIManager.h"
#include "WicDecoder.h"
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include <shlobj_core.h>
#include <shtypes.h>
#include <shellapi.h>

extern AppState app;

namespace {
    // True when the Missing/Empty overlay is visible and pt lies on its path line.
    bool HitTestOverlayPath(const POINT &pt) {
        if (app.folderOverlay == AppState::FolderOverlayState::None ||
            app.folderOverlayPath.empty())
            return false;
        const D2D1_RECT_F &r = app.folderOverlayPathRect;
        return static_cast<float>(pt.x) >= r.left && static_cast<float>(pt.x) <= r.right &&
               static_cast<float>(pt.y) >= r.top && static_cast<float>(pt.y) <= r.bottom;
    }

    // Open the overlay path in Explorer. If the directory itself is gone
    // (Missing state), walk up to the nearest parent that still exists.
    void OpenOverlayPathInExplorer(HWND hWnd) {
        fs::path p(app.folderOverlayPath);
        std::error_code ec;
        while (!p.empty() && (!fs::is_directory(p, ec) || ec)) {
            fs::path parent = p.parent_path();
            if (parent == p) break; // reached the root
            p = parent;
            ec.clear();
        }
        if (fs::is_directory(p, ec) && !ec)
            ShellExecuteW(hWnd, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void MouseHandler::HandleButtonDown(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    // New logic: Capture XButton 1
    if (message == WM_XBUTTONDOWN) {
        if (GET_XBUTTON_WPARAM(wParam) == XBUTTON1) {
            uiManager.Toggle(uiManager.getHistoryListWindow());
            return;
        }
    }
    // Clickable path in the Missing/Empty overlay — opens Explorer.
    if (message == WM_LBUTTONDOWN) {
        POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (HitTestOverlayPath(pt)) {
            OpenOverlayPathInExplorer(hWnd);
            return;
        }
    }

    // Track RMB state
    if (message == WM_RBUTTONDOWN) {
        app.isRmbDown = true;
        // Arm the context-menu click detector: remember the down point (screen
        // space) and assume a pure click until a drag/combo proves otherwise.
        app.rmbConsumed = false;
        POINT dp = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ClientToScreen(hWnd, &dp);
        app.rmbDownPt = dp;

    }

    // New logic: If RMB is down and we receive a Left Click
    if (message == WM_LBUTTONDOWN && app.isRmbDown) {
        app.rmbConsumed = true; // RMB+LMB combo — not a plain right-click
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

        // Mirror the renderer's renderW/renderH exactly.
        float renderW = winW, renderH = winH;
        if (app.imgWidth > 0 && app.imgHeight > 0) {
            GetRenderSize(winW, winH, (float)app.imgWidth, (float)app.imgHeight,
                          app.viewMode, app.viewport.zoom, renderW, renderH);
        }

        bool imageOverflows = (renderW > winW + 0.5f) || (renderH > winH + 0.5f);

        SetCursor(imageOverflows ? Constants::Cursors::CURR_GRAB : Constants::Cursors::CURR_ZOOM);

        // Save state so ButtonUp can restore if we zoomed.
        app.savedZoom = app.viewport.zoom;
        app.savedOffsetX = app.viewport.offsetX;
        app.savedOffsetY = app.viewport.offsetY;
        app.lmbDidZoom = false;

        if (!imageOverflows && app.zoomClickMultiplier > Constants::ZOOM_CLICK_MIN) {
            // Image fits inside the viewport — apply the click-zoom (1 = off).
            float centerX = winW / 2.0f;
            float centerY = winH / 2.0f;
            float dx = (float) pt.x - centerX;
            float dy = (float) pt.y - centerY;

            app.viewport.zoom *= app.zoomClickMultiplier;

            // Keep the clicked pixel under the cursor.
            // Derivation: renderer puts image center at (winW/2 + offsetX).
            // Pixel at dx from window center is (dx - oldOffsetX) from image center.
            // After zoom*Z it moves to Z*(dx - oldOffsetX). To keep it at dx:
            //   newOffsetX = dx*(1 - Z) + Z*oldOffsetX
            const float Z = app.zoomClickMultiplier;
            app.viewport.offsetX = dx * (1.0f - Z) + Z * app.savedOffsetX;
            app.viewport.offsetY = dy * (1.0f - Z) + Z * app.savedOffsetY;

            app.lmbDidZoom = true;
            ShowCursor(TRUE);
            RECT clipRc;
            GetClientRect(hWnd, &clipRc);
            MapWindowPoints(hWnd, nullptr, (POINT*)&clipRc, 2);
            ClipCursor(&clipRc);
            InvalidateRect(hWnd, nullptr, FALSE);
        }

        // Always start pan (drag) mode.
        app.viewport.lastMouse = pt;
        app.viewport.isDragging = true;
        SetCapture(hWnd);
    }
}

void MouseHandler::HandleButtonUp(HWND hWnd, UINT message, WPARAM, LPARAM) {
    //not user for now butt keep for future implementations
    // if (message == WM_XBUTTONUP) {
    //     if (GET_XBUTTON_WPARAM(wParam) == XBUTTON1) {
    //         // Action for button release if required
    //         return;
    //     }
    // }
    if (message == WM_RBUTTONUP) {
        app.isRmbDown = false;

        // Pure right-click (no window drag, no RMB+wheel/LMB combo) → context menu.
        if (app.contextMenuEnabled && !app.rmbConsumed) {
            // Tear down whatever RBUTTONDOWN armed, then show the menu at the
            // cursor and stop — skip the drag/edge-snap path below.
            //   swap=true : RMB is the window-drag button.
            //   swap=false: RMB is the view-control button (click-zoom/pan), so
            //               undo any click-zoom exactly like the view-control up path.
            app.isWindowDragging = false;
            if (app.lmbDidZoom) {
                app.viewport.zoom    = app.savedZoom;
                app.viewport.offsetX = app.savedOffsetX;
                app.viewport.offsetY = app.savedOffsetY;
            }
            ShowCursor(TRUE);
            ClipCursor(nullptr);            app.lmbDidZoom = false;
            app.viewport.isDragging = false;
            ReleaseCapture();
            UpdateHoverCursor(hWnd);
            InvalidateRect(hWnd, nullptr, FALSE);
            POINT scr;
            GetCursorPos(&scr);
            Input::ContextMenuHandler::Show(hWnd, scr.x, scr.y);
            return;
        }

        // RMB was held for pan/drag (not a context-menu click): update cursor for
        // the new non-dragging state.
        app.viewport.isDragging = false;
        ReleaseCapture();
        ShowCursor(TRUE);
        ClipCursor(nullptr);
        UpdateHoverCursor(hWnd);
        InvalidateRect(hWnd, nullptr, FALSE);
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
            int targetW = (int) (app.baseWidth  * app.dpiScale);
            int targetH = (int) (app.baseHeight * app.dpiScale);

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
            ShowCursor(TRUE);
            ClipCursor(nullptr);
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
            MONITORINFO mi = {sizeof(mi)};
            if (GetMonitorInfo(hMon, &mi)) {
                const RECT &wa = mi.rcWork;
                const int snap = Constants::WINDOW_SNAP_DISTANCE;
                bool nearLeft = cursor.x <= wa.left + snap;
                bool nearRight = cursor.x >= wa.right - snap;
                bool nearTop = cursor.y <= wa.top + snap;
                int halfW = (wa.right - wa.left) / 2;
                int halfH = (wa.bottom - wa.top) / 2;
                RECT target = {};
                bool doSnap = false;
                if (nearLeft && nearTop) {
                    target = {wa.left, wa.top, wa.left + halfW, wa.top + halfH};
                    doSnap = true;
                } else if (nearRight && nearTop) {
                    target = {wa.left + halfW, wa.top, wa.right, wa.top + halfH};
                    doSnap = true;
                } else if (nearLeft) {
                    target = {wa.left, wa.top, wa.left + halfW, wa.bottom};
                    doSnap = true;
                } else if (nearRight) {
                    target = {wa.left + halfW, wa.top, wa.right, wa.bottom};
                    doSnap = true;
                } else if (nearTop) {
                    target = wa;
                    doSnap = true;
                }
                if (doSnap) {
                    SetWindowPos(hWnd, nullptr,
                                 target.left, target.top,
                                 target.right - target.left,
                                 target.bottom - target.top,
                                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
                    InvalidateRect(hWnd, nullptr, FALSE);
                }
            }
        }
    } else if (IsViewControlAction(message)) {
        SetCursor(Constants::Cursors::CURR_DEFAULT);

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
        ShowCursor(TRUE);
        ClipCursor(nullptr);
        InvalidateRect(hWnd, nullptr, FALSE);
    }
}

bool MouseHandler::UpdateHoverCursor(HWND hWnd) {
    if (app.isContextMenuOpen) return false;
    if (app.slideshow.running && app.slideshow.cursorHidden) return false;
    if (app.isMidDragging || app.viewport.isDragging || app.isWindowDragging) return false;

    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(hWnd, &pt);

    if (HitTestOverlayPath(pt)) {
        SetCursor(Constants::Cursors::CURR_GRAB);
        return true;
    }

    if (app.imgWidth > 0 && app.imgHeight > 0) {
        RECT rc;
        GetClientRect(hWnd, &rc);
        float winW = (float)(rc.right - rc.left);
        float winH = (float)(rc.bottom - rc.top);
        float renderW, renderH;
        GetRenderSize(winW, winH, (float)app.imgWidth, (float)app.imgHeight,
                      app.viewMode, app.viewport.zoom, renderW, renderH);
        if ((renderW > winW + 0.5f) || (renderH > winH + 0.5f)) {
            SetCursor(Constants::Cursors::CURR_CLICK);
            return true;
        }
    }

    SetCursor(Constants::Cursors::CURR_ZOOM);
    return true;
}

void MouseHandler::HandleMouseMove(HWND hWnd, LPARAM lParam) {
    if (app.isContextMenuOpen) return;

    // Context-menu gating: once the cursor travels past the tolerance while RMB
    // is held, the gesture is a drag (window move / pan) — not a click — so the
    // right-click menu must not fire on release.
    if (app.isRmbDown && !app.rmbConsumed) {
        POINT cur = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ClientToScreen(hWnd, &cur);
        if (std::abs(cur.x - app.rmbDownPt.x) > Constants::CONTEXT_MENU_DRAG_TOLERANCE ||
            std::abs(cur.y - app.rmbDownPt.y) > Constants::CONTEXT_MENU_DRAG_TOLERANCE)
            app.rmbConsumed = true;
    }

    UpdateHoverCursor(hWnd);

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
        SetCursor(app.lmbDidZoom ? Constants::Cursors::CURR_ZOOM : Constants::Cursors::CURR_GRAB);

        POINT curMouse = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

        float dx = (float) (curMouse.x - app.viewport.lastMouse.x);
        float dy = (float) (curMouse.y - app.viewport.lastMouse.y);

        // Update Position — inverse during click-zoom (mouse direction mirrors image movement)
        if (app.lmbDidZoom) {
            app.viewport.offsetX -= dx*(app.zoomClickMultiplier-1.0f);
            app.viewport.offsetY -= dy*(app.zoomClickMultiplier-1.0f);;
        } else {
            app.viewport.offsetX += dx;
            app.viewport.offsetY += dy;
        }
        app.viewport.lastMouse = curMouse;

        // Constraint — must mirror the renderer's renderW/renderH exactly so the
        // pan limit matches the actual drawn edges of the image.
        if (app.imgWidth > 0 && app.imgHeight > 0) {
            RECT rc;
            GetClientRect(hWnd, &rc);
            float winW = (float) (rc.right - rc.left);
            float winH = (float) (rc.bottom - rc.top);

            float renderW, renderH;
            GetRenderSize(winW, winH, (float)app.imgWidth, (float)app.imgHeight,
                          app.viewMode, app.viewport.zoom, renderW, renderH);

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
    const int delta = app.invertWheelDirection ? -rawDelta : rawDelta;

    if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
        app.opacity = static_cast<BYTE>(
            delta > 0
                ? std::min(255, (int) app.opacity + Constants::OPACITY_STEP)
                : std::max(10, (int) app.opacity - Constants::OPACITY_STEP));
        SetLayeredWindowAttributes(hWnd, 0, app.opacity, LWA_ALPHA);
        return;
    }

    if (app.playlist.empty()) return;

    bool isRmbDown = (GetKeyState(VK_RBUTTON) & 0x8000) != 0;

    if (isRmbDown) {
        app.rmbConsumed = true; // RMB+wheel zoom — suppress the context menu on RMB up
        app.viewport.zoom *= (delta > 0) ? Constants::ZOOM_STEP : (1.0f / Constants::ZOOM_STEP);
        InvalidateRect(hWnd, nullptr, FALSE);
        // Update cursor for overflow state — UpdateHoverCursor bails on isDragging.
        if (app.imgWidth > 0 && app.imgHeight > 0) {
            RECT rc;
            GetClientRect(hWnd, &rc);
            float winW = (float)(rc.right - rc.left);
            float winH = (float)(rc.bottom - rc.top);
            float renderW, renderH;
            GetRenderSize(winW, winH, (float)app.imgWidth, (float)app.imgHeight,
                          app.viewMode, app.viewport.zoom, renderW, renderH);
            SetCursor((renderW > winW + 0.5f) || (renderH > winH + 0.5f)
                          ? Constants::Cursors::CURR_CLICK
                          : Constants::Cursors::CURR_ZOOM);
        }
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
    const int hDelta = app.invertWheelDirectionH ? -rawDelta : rawDelta;

    if (isRmbDown) {
        app.rmbConsumed = true; // RMB+horizontal-wheel resize — suppress context menu on RMB up
        RECT rc;
        GetWindowRect(hWnd, &rc);
        int currentW = rc.right - rc.left;
        int currentH = rc.bottom - rc.top;

        int resizeStep = (hDelta > 0) ? 20 : -20;
        int newW = currentW + resizeStep;
        int newH = static_cast<int>(std::round(
                currentH + resizeStep * (static_cast<float>(currentH) / currentW)));
        int newX = rc.left - (resizeStep / 2);
        int newY = rc.top - (resizeStep / 2);

        SetWindowPos(hWnd, nullptr, newX, newY, newW, newH,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
        InvalidateRect(hWnd, nullptr, FALSE);
        return;
    }

    // Plain horizontal scroll: cycle through the navigation snapshot.
    // Snapshot is taken from the raw backing array (not display-capped).
    // Refreshed explicitly when the user presses Ctrl+Tab in the history panel.
    static int s_histIdx = 0;
    static int s_accumulator = 0;
    static int s_snapVersion = -1;

    if (UI::GetNavigationSnapshot().empty())
        UI::CaptureNavigationSnapshot();

    const auto &snap = UI::GetNavigationSnapshot();
    if (snap.empty()) return;

    // Reset index whenever the snapshot was refreshed via Ctrl+Tab
    const int currentVersion = UI::GetNavigationSnapshotVersion();
    if (currentVersion != s_snapVersion) {
        s_histIdx = 0;
        s_snapVersion = currentVersion;
    }

    s_accumulator += hDelta;
    const int threshold = WHEEL_DELTA * Constants::MOUSE_HSCROLL_FOLDER_TICKS;
    if (std::abs(s_accumulator) < threshold) return;

    const int total = (int) snap.size();
    if (s_accumulator > 0) s_histIdx = (s_histIdx + 1) % total;
    else s_histIdx = (s_histIdx - 1 + total) % total;
    s_accumulator = 0;

    const std::wstring folder = snap[s_histIdx]; // copy — snap must not be used after OpenDirectory

    std::wstring displayName = folder;
    const size_t sep = folder.find_last_of(L"\\/");
    if (sep != std::wstring::npos) displayName = folder.substr(sep + 1);

    if (!UI::IsFolderValidForViewer(folder)) {
        auto &histWnd = uiManager.getHistoryListWindow();
        if (histWnd.IsVisible())
            InvalidateRect(histWnd.GetHwnd(), nullptr, FALSE);
        g_overlayManager.PostCenterMessage(hWnd,
                                           std::wstring(L"⚠ ") +
                                           std::to_wstring(s_histIdx + 1) + L"/" + std::to_wstring(total) + L" " + displayName);
        return;
    }

    OpenDirectory(hWnd, folder);

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
