// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#include "FloatingPanelWnd.h"
#include "Dedicated/DedicatedInstance.h" // AppIconId — one icon source for the process
#include "../../Platform/Constants.h"
#include "../../AppState.h"
#include <dwmapi.h>
#include <windowsx.h>

namespace UI {
    void FloatingPanelWnd::InitFloating(HINSTANCE hInstance, HWND hParent,
                                        LPCWSTR className, LPCWSTR title,
                                        int pixelW, int pixelH,
                                        UINT classStyle) {
        m_hParent = hParent;

        WNDCLASSW wc = {};
        wc.style = classStyle;
        wc.lpfnWndProc = IPanelWindow::WindowRouter;
        wc.hInstance = hInstance;
        wc.hCursor = Constants::Cursors::CURR_DEFAULT;
        // Panels carry the same icon as the app, so a dedicated instance stays
        // visually distinct all the way down to its floating windows.
        wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCEW(Dedicated::AppIconId()));
        wc.lpszClassName = className;
        RegisterClassW(&wc); // silently fails if already registered — that is fine

        CreateWindowExW(
                WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
                className, title,
                WS_POPUP | WS_CAPTION | WS_BORDER,
                0, 0, pixelW, pixelH,
                hParent, nullptr, hInstance, this);

        if (!m_hWnd) return;

        SetLayeredWindowAttributes(m_hWnd, 0, Constants::THUMBNAIL_PANEL_WINDOW_OPACITY, LWA_ALPHA);

        BOOL darkMode = app.isDarkThemed ? TRUE : FALSE;
        DwmSetWindowAttribute(m_hWnd, Constants::DWMWA_DARK_MODE, &darkMode, sizeof(darkMode));

        DWORD corner = app.cornerPreference;
        DwmSetWindowAttribute(m_hWnd, Constants::DWMWA_WINDOW_CORNER_PREFERENCES, &corner, sizeof(corner));

        COLORREF capColor = Constants::Theme::ThemedGray(
            Constants::Theme::Panel::BACKGROUND_INACTIVE, app.themeFactor);
        DwmSetWindowAttribute(m_hWnd, Constants::DWMWA_CAPTION_COLOR_ATTR, &capColor, sizeof(capColor));

        SetWindowPos(m_hWnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }

    COLORREF FloatingPanelWnd::GetBgColor() const {
        const float base = (GetFocus() == m_hWnd)
                               ? Constants::Theme::Panel::BACKGROUND_ACTIVE
                               : Constants::Theme::Panel::BACKGROUND_INACTIVE;
        return Constants::Theme::ThemedGray(base, app.themeFactor);
    }

    LRESULT FloatingPanelWnd::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_XBUTTONDOWN) {
            if (m_hParent) {
                // Forward the event to the parent so MouseHandler can process it
                SendMessageW(m_hParent, WM_XBUTTONDOWN, wParam, lParam);
                return 0;
            }
        }
        if (message == WM_SETFOCUS) {
            InvalidateRect(m_hWnd, nullptr, FALSE);
            OnSetFocus();
            return 0;
        }
        if (message == WM_KILLFOCUS) {
            InvalidateRect(m_hWnd, nullptr, FALSE);
            OnKillFocus();
            return 0;
        }
        // WM_SYSKEYDOWN as well as WM_KEYDOWN, and the reason is not obvious:
        //
        //   • F10 pressed ALONE arrives as WM_SYSKEYDOWN. Windows reserves it as
        //     the menu-activation key, so it never appears as an ordinary
        //     keypress — which meant F10 toggled its panel from the main window
        //     and did nothing at all once that panel had focus.
        //   • Every ALT combination arrives the same way. Alt+W/A/S/D (snap),
        //     Alt+Q/E/Z/C (quarter-snap) and Alt+X (reset window) were therefore
        //     dead in every panel, silently.
        //
        // AppMain's WndProc has always handled both for the main window. Panels
        // forward to it, so they have to recognise the same pair or the two
        // disagree about which keys exist.
        if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;

            if (!OnKeyDown(wParam, ctrl, shift, alt)) {
                // Forwarded as the SAME message it arrived as. Re-labelling a
                // WM_SYSKEYDOWN as WM_KEYDOWN would strip the context bit in
                // lParam that says whether Alt was held — and the resolver reads
                // lParam for the Right-Shift scancode, so the original must
                // survive intact.
                PostMessageW(m_hParent, message, wParam, lParam);
            }

            // System keys still go to DefWindowProc, exactly as the main window
            // does it, so Alt+F4 keeps working. Neither the main window nor a
            // panel has a menu bar, so F10's own default behaviour has nothing
            // to open.
            //
            // ⚠ ALT+SPACE IS THE EXCEPTION, handled by the WM_SYSCHAR case
            // below. This comment used to say Alt+Space "keeps working" - true
            // when it was Windows' key to handle, wrong now that it is ours.
            return (message == WM_SYSKEYDOWN)
                       ? DefWindowProcW(m_hWnd, message, wParam, lParam)
                       : 0;
        }
        // ⚠ THE ALT+SPACE BEEP, THE PANEL COPY.
        //
        // The block above forwarded the keypress to the main window, which
        // toggles cull mode - and then handed the same WM_SYSKEYDOWN to THIS
        // window's DefWindowProc, which is right for Alt+F4 and wrong for
        // Alt+Space: TranslateMessage turns it into a WM_SYSCHAR, DefWindowProc
        // answers that by opening the system menu, and a panel is
        // WS_POPUP | WS_CAPTION | WS_BORDER with no WS_SYSMENU. The failure
        // mode for "there is no menu" is MessageBeep, once per toggle.
        //
        // AppMain carries the identical guard for the main window, and
        // ThumbnailPanelWnd for the strips. All three are needed: the beep
        // follows whichever window has focus.
        if (message == WM_SYSCHAR && wParam == VK_SPACE)
            return 0;

        if (message == WM_MBUTTONUP) {
            if (!OnMButtonUp(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)))
                Hide();
            return 0;
        }
        // Scrolling, before the panel sees anything. A panel that returns no
        // view from ScrollViewAt is unaffected — except for the horizontal
        // wheel, which is swallowed for every panel whether it scrolls or not.
        //
        // THAT SWALLOW IS THE POINT, not a side effect. Unhandled, WM_MOUSEHWHEEL
        // travels on and reaches the main window, whose horizontal wheel changes
        // FOLDER — so a stray thumb-wheel nudge while reading the Help panel used
        // to move the viewer somewhere else entirely, with the panel still on top
        // hiding that it had happened.
        {
            LRESULT r = 0;
            if (HandleScrollMessage(message, wParam, lParam, r)) return r;
        }

        return HandlePanelMessage(message, wParam, lParam);
    }

    void FloatingPanelWnd::OnScrolled() {
        if (m_hWnd) InvalidateRect(m_hWnd, nullptr, FALSE);
    }

    bool FloatingPanelWnd::HandleScrollMessage(UINT message, WPARAM wParam,
                                               LPARAM lParam, LRESULT &resultOut) {
        resultOut = 0;

        switch (message) {
            // ── Wheels ──────────────────────────────────────────────────────
            //
            // The pointer is in SCREEN coordinates for both wheel messages —
            // unlike every other mouse message here — so it is converted before
            // ScrollViewAt sees it. Getting that wrong picks the wrong view on a
            // multi-list panel and silently scrolls the other one.
            case WM_MOUSEWHEEL:
            case WM_MOUSEHWHEEL: {
                const bool horizontal = (message == WM_MOUSEHWHEEL);

                POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ScreenToClient(m_hWnd, &pt);

                ScrollView *sv = ScrollViewAt(pt);
                if (!sv) {
                    // Nothing to scroll. The vertical wheel is left to the panel
                    // (some use it for their own purposes); the horizontal one is
                    // eaten regardless — see the note in HandleMessage.
                    return horizontal;
                }

                const int linePx = ScrollLinePx(*sv);
                if (linePx <= 0) return horizontal;

                if (horizontal) sv->ScrollBy(HWheelDeltaToPixels(wParam, linePx), 0);
                else            sv->ScrollBy(0, WheelDeltaToPixels(wParam, linePx));
                OnScrolled();
                return true;
            }

            // ── Grab a thumb, or page the track ─────────────────────────────
            case WM_LBUTTONDOWN: {
                POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ScrollView *sv = ScrollViewAt(pt);
                if (!sv) return false;

                // Thumbs before tracks, and both before the panel's own rows:
                // a bar is the smallest target on any panel and must not be
                // stolen by whatever is drawn beside it.
                if (PtInRect(&sv->vThumb, pt) || PtInRect(&sv->hThumb, pt)) {
                    const bool horiz = PtInRect(&sv->hThumb, pt) != 0;
                    m_scrollDragView   = sv;
                    m_scrollDragHoriz  = horiz;
                    m_scrollDragGrabPx = horiz ? (pt.x - sv->hThumb.left)
                                         : (pt.y - sv->vThumb.top);
                    SetCapture(m_hWnd);
                    return true;
                }
                if (PtInRect(&sv->vTrack, pt)) {
                    sv->ScrollBy(0, pt.y < sv->vThumb.top ? -sv->Height() : sv->Height());
                    OnScrolled();
                    return true;
                }
                if (PtInRect(&sv->hTrack, pt)) {
                    sv->ScrollBy(pt.x < sv->hThumb.left ? -sv->Width() : sv->Width(), 0);
                    OnScrolled();
                    return true;
                }
                return false;
            }

            // ── Drag, hover highlight, and the cursor over a bar ────────────
            case WM_MOUSEMOVE: {
                if (m_scrollDragView) {
                    if (m_scrollDragHoriz) m_scrollDragView->DragToX(GET_X_LPARAM(lParam), m_scrollDragGrabPx);
                    else                   m_scrollDragView->DragToY(GET_Y_LPARAM(lParam), m_scrollDragGrabPx);
                    // HELD LIT FOR THE WHOLE DRAG. The pointer leaves the thumb
                    // constantly while dragging — that is what dragging IS — and
                    // a highlight that tracked containment would flicker off the
                    // moment the drag became useful.
                    m_scrollDragView->vThumbHot = !m_scrollDragHoriz;
                    m_scrollDragView->hThumbHot =  m_scrollDragHoriz;
                    OnScrolled();
                    return true;
                }

                // HOVERING A BAR IS CONSUMED HERE, not left to WM_SETCURSOR
                // alone. Several panels call SetCursor unconditionally in their
                // own WM_MOUSEMOVE — "over my link? hand : arrow" — which runs
                // AFTER the base has already answered WM_SETCURSOR and quietly
                // puts the arrow back. The result was a scrollbar that showed a
                // hand in some windows and not others, which is exactly the
                // inconsistency this base class exists to remove.
                //
                // Swallowing the message also stops those panels updating their
                // hover highlight while the pointer is over the bar. That is the
                // right answer anyway: the pointer is not over a row, so no row
                // should light up.
                POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                if (ScrollView *sv = ScrollViewAt(pt)) {
                    // THE HOVER HIGHLIGHT, and it is maintained on EVERY move,
                    // not only the ones over a bar — the clearing half matters
                    // as much as the lighting half. Panels used to do this
                    // themselves, which stopped working the moment the base
                    // began consuming moves over the bar: the highlight could be
                    // set but never unset, or never set at all.
                    const bool vHot = PtInRect(&sv->vThumb, pt) != 0;
                    const bool hHot = PtInRect(&sv->hThumb, pt) != 0;
                    if (vHot != sv->vThumbHot || hHot != sv->hThumbHot) {
                        sv->vThumbHot = vHot;
                        sv->hThumbHot = hHot;
                        // Invalidate directly rather than through OnScrolled:
                        // nothing scrolled, and a panel that overrides that hook
                        // for scroll-specific work should not see a hover as one.
                        if (m_hWnd) InvalidateRect(m_hWnd, nullptr, FALSE);
                    }

                    if (PtInRect(&sv->vTrack, pt) || PtInRect(&sv->hTrack, pt) ||
                        vHot || hHot) {
                        SetCursor(Constants::Cursors::CURR_CLICK);
                        return true;
                    }
                }
                return false;
            }

            case WM_LBUTTONUP:
                if (!m_scrollDragView) return false;
                ReleaseCapture();       // WM_CAPTURECHANGED clears the state
                return true;

            // A drag can end without the button coming up — Alt+Tab, a message
            // box, another window taking capture. Clearing HERE rather than only
            // on WM_LBUTTONUP is what stops a panel being left permanently in
            // drag mode, scrolling on every later mouse-move with no button held.
            case WM_CAPTURECHANGED:
                if (!m_scrollDragView) return false;
                // The drag held the highlight on regardless of where the pointer
                // was; dropping it here stops a thumb staying lit after a drag
                // that ended with the cursor somewhere else entirely.
                m_scrollDragView->vThumbHot = false;
                m_scrollDragView->hThumbHot = false;
                if (m_hWnd) InvalidateRect(m_hWnd, nullptr, FALSE);
                m_scrollDragView  = nullptr;
                m_scrollDragHoriz = false;
                return true;

            // ── Cursor ──────────────────────────────────────────────────────
            case WM_SETCURSOR: {
                if (LOWORD(lParam) != HTCLIENT) return false;
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(m_hWnd, &pt);

                ScrollView *sv = ScrollViewAt(pt);
                if (!sv) return false;

                // A HAND OVER EITHER BAR, everywhere. The panels disagreed about
                // this — one deliberately showed an arrow over its track on the
                // grounds that a hand suggests a button. One rule is worth more
                // than either argument: the bar IS clickable, the track pages
                // and the thumb drags, so the hand is telling the truth.
                if (PtInRect(&sv->vTrack, pt) || PtInRect(&sv->hTrack, pt) ||
                    PtInRect(&sv->vThumb, pt) || PtInRect(&sv->hThumb, pt)) {
                    SetCursor(Constants::Cursors::CURR_CLICK);
                    resultOut = TRUE;
                    return true;
                }
                return false;
            }

            default:
                return false;
        }
    }
} // namespace UI
