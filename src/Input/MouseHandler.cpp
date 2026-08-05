// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#include "MouseHandler.h"
#include "AppState.h"
#include "AppCommands.h"
#include "Command.h"          // every gesture resolves to a Command — see below
#include "UI/AppMenu/AppMenu.h"
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

// =============================================================================
// A GESTURE THAT MEANS AN ACTION RESOLVES TO A Command AND GOES THROUGH
// InputManager::ExecuteCommand — exactly as a keystroke does.
//
// This file used to apply several of those actions itself (LoadImageIndex for
// the wheel, the zoom arithmetic inline, AppCommands::ToggleFullscreen on
// double-click). Each was byte-identical to the matching case in
// CommandExecuter.cpp, which made them two copies of one behaviour — and, once
// mirroring existed, meant the mouse silently did not mirror while the keyboard
// did. The sink has to be universal or the gate on it is a lie.
//
// What deliberately does NOT resolve to a Command: the continuous gestures —
// window drag, viewport pan, middle-drag resize. They are a stream of pixel
// deltas rather than a discrete action, there is no sensible enumerator for
// them, and forwarding one to another machine would be meaningless.
// =============================================================================

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
            InputManager::ExecuteCommand(hWnd, Command::ToggleHistory);
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
        app.isRmbDown = false;
        InputManager::ExecuteCommand(hWnd, Command::ShowInExplorer);
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
            ClampZoomToLimits(hWnd); // bounds the EFFECTIVE zoom, not the multiplier

            // Keep the clicked pixel under the cursor.
            // Derivation: renderer puts image center at (winW/2 + offsetX).
            // Pixel at dx from window center is (dx - oldOffsetX) from image center.
            // After zoom*Z it moves to Z*(dx - oldOffsetX). To keep it at dx:
            //   newOffsetX = dx*(1 - Z) + Z*oldOffsetX
            //
            // Z must be the factor ACTUALLY applied, not the requested multiplier
            // — if the clamp above capped the zoom, anchoring with the requested
            // value would slide the clicked pixel out from under the cursor.
            const float Z = (app.savedZoom > 0.0f)
                                ? (app.viewport.zoom / app.savedZoom)
                                : app.zoomClickMultiplier;
            app.viewport.offsetX = dx * (1.0f - Z) + Z * app.savedOffsetX;
            app.viewport.offsetY = dy * (1.0f - Z) + Z * app.savedOffsetY;

            app.lmbDidZoom = true;
            //USED FOR TESTING = false to see if clipping works
            ShowCursor(FALSE);
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
            UI::AppMenu::Show(hWnd, scr.x, scr.y);
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
        // A middle CLICK resets the window; a middle DRAG resized it and must
        // not also reset what it just changed.
        if (!app.hasMidMoved) {
            InputManager::ExecuteCommand(hWnd, Command::ResetWindowLayout);
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

    // ONLY STAMP THE CURSOR WHEN THE POINTER IS ACTUALLY OVER hWnd.
    //
    // RendererD2D::Render calls this after every Present, and a frame gets
    // produced no matter where the pointer sits — including over a thumbnail
    // strip or a floating panel, which are separate top-level windows that set
    // their own cursor from their own WM_MOUSEMOVE. Without this check the two
    // fought: the panel set the arrow on each mouse move, the main window
    // re-stamped the zoom cursor on each frame, and hovering a spawned panel
    // made the pointer flick between arrow and zoom for as long as frames kept
    // coming. GA_ROOT because WindowFromPoint can name a child window.
    HWND under = WindowFromPoint(pt);
    if (under != hWnd && GetAncestor(under, GA_ROOT) != hWnd) return false;

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
            //pan viewport
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
        InputManager::ExecuteCommand(
            hWnd, delta > 0 ? Command::OpacityUp : Command::OpacityDown);
        return;
    }

    if (app.playlist.empty()) return;

    bool isRmbDown = (GetKeyState(VK_RBUTTON) & 0x8000) != 0;

    if (isRmbDown) {
        app.rmbConsumed = true; // RMB+wheel zoom — suppress the context menu on RMB up
        InputManager::ExecuteCommand(hWnd, delta > 0 ? Command::ZoomIn : Command::ZoomOut);
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
        InputManager::ExecuteCommand(hWnd, delta > 0 ? Command::ZoomIn : Command::ZoomOut);
    } else {
        // Wheel DOWN advances, matching the old `step = (delta < 0) ? 1 : -1`.
        InputManager::ExecuteCommand(
            hWnd, delta < 0 ? Command::NextImage : Command::PrevImage);
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

    // Plain horizontal scroll: step through the History panel's folder list.
    // Only the notch accumulation lives here — the stepping itself goes through
    // UI::WalkHistoryFolder, the same function the PageUp/PageDown and
    // Insert/Delete keys use, so all three behave identically. It owns the
    // frozen snapshot that keeps a walk stable while opening a folder reorders
    // the MRU list underneath.
    static int s_accumulator = 0;

    s_accumulator += hDelta;
    const int threshold = WHEEL_DELTA * Constants::MOUSE_HSCROLL_FOLDER_TICKS;
    if (std::abs(s_accumulator) < threshold) return;

    const bool reverse = (s_accumulator < 0);
    s_accumulator = 0;

    // The wheel visits every row the panel shows, favorites included — the two
    // key pairs are the ones that split the list into halves.
    InputManager::ExecuteCommand(hWnd, reverse ? Command::PrevHistoryFolderAll
                                               : Command::NextHistoryFolderAll);
}

void MouseHandler::HandleDoubleClick(HWND hWnd) {
    // The redraw suppression stays here rather than moving into the command:
    // it exists because a double-click resizes the window while the mouse still
    // holds capture, which the keyboard path never does.
    SendMessageW(hWnd, WM_SETREDRAW, FALSE, 0);
    InputManager::ExecuteCommand(hWnd, Command::ToggleFullscreen);
    SendMessageW(hWnd, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(hWnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_FRAME);
}
