// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#include "ThemedTooltip.h"

#include <algorithm>
#include <dwmapi.h> // DwmSetWindowAttribute — corner preference

#include "../Platform/Constants.h"
#include "../Platform/ConstantsTheme.h"
#include "../AppState.h"

namespace UI {

    HWND         ThemedTooltip::s_hwnd = nullptr;
    HWND         ThemedTooltip::s_hOwner = nullptr;
    RECT         ThemedTooltip::s_anchor = {0, 0, 0, 0};
    HFONT        ThemedTooltip::s_font = nullptr;
    int          ThemedTooltip::s_fontDpi = 0;
    HBRUSH       ThemedTooltip::s_bgBrush = nullptr;
    COLORREF     ThemedTooltip::s_bgColor = 0;
    HBRUSH       ThemedTooltip::s_borderBrush = nullptr;
    COLORREF     ThemedTooltip::s_borderColor = 0;
    std::wstring ThemedTooltip::s_text;

    namespace {
        constexpr int PAD_X    = 8;
        constexpr int PAD_Y    = 6;
        constexpr int MAX_W    = 620; // wraps long link targets instead of overflowing
        constexpr int FLIP_GAP = 6;
        constexpr int FONT_PT  = 9;

        COLORREF BgColor() {
            return app.isDarkThemed
                           ? Constants::Theme::ThemedGray(
                                     Constants::Theme::Panel::BACKGROUND_INACTIVE * 2,
                                     app.themeFactor)
                           : GetSysColor(COLOR_WINDOW);
        }
        COLORREF TextColor() {
            return app.isDarkThemed ? RGB(220, 220, 220) : GetSysColor(COLOR_WINDOWTEXT);
        }
        COLORREF BorderColor() {
            return app.isDarkThemed ? RGB(90, 90, 90) : GetSysColor(COLOR_ACTIVEBORDER);
        }
    } // namespace

    const wchar_t *ThemedTooltip::ClassName() { return L"QIV_ThemedTooltip"; }

    void ThemedTooltip::EnsureClass() {
        static bool registered = false;
        if (registered) return;
        WNDCLASSEXW wc{sizeof(WNDCLASSEXW)};
        wc.lpfnWndProc = &ThemedTooltip::WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = nullptr; // never takes the cursor from the panel beneath
        wc.lpszClassName = ClassName();
        wc.style = CS_SAVEBITS; // transient popup — makes show/hide cheap
        RegisterClassExW(&wc);
        registered = true;
    }

    void ThemedTooltip::EnsureFont() {
        const int dpi = static_cast<int>(app.dpiScale * 96.0f);
        if (s_font && s_fontDpi == dpi) return; // reuse — the common case
        if (s_font) DeleteObject(s_font);       // DPI changed: rebuild once
        s_font = CreateFontW(-MulDiv(FONT_PT, dpi, 72), 0, 0, 0, FW_NORMAL,
                             FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
        s_fontDpi = dpi;
    }

    // Reuses the cached brush unless the theme actually produced a different
    // colour, in which case it is rebuilt once and cached again.
    HBRUSH ThemedTooltip::EnsureBrush(HBRUSH &brush, COLORREF &cached, COLORREF wanted) {
        if (brush && cached == wanted) return brush;
        if (brush) DeleteObject(brush);
        brush = CreateSolidBrush(wanted);
        cached = wanted;
        return brush;
    }

    bool ThemedTooltip::IsVisible() {
        return s_hwnd && IsWindowVisible(s_hwnd);
    }

    void ThemedTooltip::Show(HWND hOwner, const std::wstring &text, POINT ptScreen,
                             const RECT &anchorScreen) {
        if (text.empty() || !hOwner) { Hide(); return; }

        EnsureClass();
        // The owner may have been destroyed since the last Show — Windows takes
        // owned windows down with it, leaving this handle dead.
        if (s_hwnd && !IsWindow(s_hwnd)) { s_hwnd = nullptr; s_hOwner = nullptr; }

        if (!s_hwnd) {
            s_hwnd = CreateWindowExW(
                    WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                    ClassName(), nullptr, WS_POPUP,
                    0, 0, 0, 0, hOwner, nullptr,
                    GetModuleHandleW(nullptr), nullptr);
            if (!s_hwnd) return;
            s_hOwner = hOwner;
        } else if (s_hOwner != hOwner) {
            // Re-point ownership at whoever is showing it now, so the popup sits
            // above THIS panel. GWLP_HWNDPARENT on a WS_POPUP sets the owner, not
            // a parent — the window stays top-level.
            SetWindowLongPtrW(s_hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(hOwner));
            s_hOwner = hOwner;
        }

        s_text = text;
        EnsureFont();

        // --- measure -------------------------------------------------------
        const UINT dpi = static_cast<UINT>(app.dpiScale * 96.0f);
        const int padX = MulDiv(PAD_X, dpi, 96);
        const int padY = MulDiv(PAD_Y, dpi, 96);
        const int maxW = MulDiv(MAX_W, dpi, 96);

        HDC hdc = GetDC(s_hwnd);
        HFONT old = static_cast<HFONT>(SelectObject(hdc, s_font));
        RECT calc = {0, 0, maxW - padX * 2, 0};
        DrawTextW(hdc, s_text.c_str(), -1, &calc,
                  DT_CALCRECT | DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(hdc, old);
        ReleaseDC(s_hwnd, hdc);

        const int w = (calc.right - calc.left) + padX * 2;
        const int h = (calc.bottom - calc.top) + padY * 2;

        // --- keep it on the monitor the cursor is on -----------------------
        int x = ptScreen.x;
        int y = ptScreen.y;
        MONITORINFO mi{sizeof(mi)};
        if (GetMonitorInfoW(MonitorFromPoint(ptScreen, MONITOR_DEFAULTTONEAREST), &mi)) {
            x = std::min<int>(x, mi.rcWork.right - w);
            x = std::max<int>(x, mi.rcWork.left);
            // Flip above the cursor rather than let it clip at the bottom edge.
            if (y + h > mi.rcWork.bottom)
                y = std::max<int>(mi.rcWork.top, ptScreen.y - h - MulDiv(FLIP_GAP, dpi, 96));
        }

        // Follow the app-wide corner preference, same as FloatingPanelWnd and
        // ThemedDialog do — Ctrl+Shift+Numpad* toggles it and the popup must not
        // be the one square window left over.
        DWORD corner = app.cornerPreference;
        DwmSetWindowAttribute(s_hwnd, Constants::DWMWA_WINDOW_CORNER_PREFERENCES,
                              &corner, sizeof(corner));

        SetWindowPos(s_hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        InvalidateRect(s_hwnd, nullptr, TRUE);

        // Own the dismissal. Polling the cursor against the anchor rect works no
        // matter how the cursor leaves — sideways, off a window edge, or because
        // another window came forward — none of which reliably produce a final
        // mouse-move in the owner.
        s_anchor = anchorScreen;
        SetTimer(s_hwnd, TIMER_ID, POLL_MS, nullptr);
    }

    void ThemedTooltip::Hide() {
        if (!s_hwnd) return;
        KillTimer(s_hwnd, TIMER_ID);
        ShowWindow(s_hwnd, SW_HIDE);
        s_anchor = RECT{0, 0, 0, 0};
    }

    void ThemedTooltip::ApplyTheme() {
        // Optional: the caches detect their own staleness, so this only forces an
        // immediate repaint of a popup that happens to be on screen during a
        // theme switch. Nothing depends on it being called.
        if (s_hwnd) InvalidateRect(s_hwnd, nullptr, TRUE);
    }

    void ThemedTooltip::Destroy() {
        if (s_hwnd)        { DestroyWindow(s_hwnd);       s_hwnd = nullptr; s_hOwner = nullptr; }
        if (s_font)        { DeleteObject(s_font);        s_font = nullptr; s_fontDpi = 0; }
        if (s_bgBrush)     { DeleteObject(s_bgBrush);     s_bgBrush = nullptr; }
        if (s_borderBrush) { DeleteObject(s_borderBrush); s_borderBrush = nullptr; }
    }

    LRESULT CALLBACK ThemedTooltip::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
            // Never activate: showing the popup must not move focus off the panel
            // it is describing.
            case WM_MOUSEACTIVATE:
                return MA_NOACTIVATE;

            // Hit-test transparent, so the popup can never sit between the cursor
            // and the row it explains — that would stop the owner's WM_MOUSEMOVE
            // firing and leave the popup stuck on screen.
            case WM_NCHITTEST:
                return HTTRANSPARENT;

            case WM_ERASEBKGND:
                return 1; // WM_PAINT fills the whole client area

            case WM_TIMER: {
                if (wParam != TIMER_ID) break;
                POINT pt{};
                // Hide once the cursor is off the thing this popup describes, or
                // if the owner went away. An empty anchor means "no rule given" —
                // leave it to the caller rather than hiding immediately.
                if (s_anchor.right <= s_anchor.left) break;
                if (!GetCursorPos(&pt) || !PtInRect(&s_anchor, pt) ||
                    (s_hOwner && !IsWindow(s_hOwner)))
                    Hide();
                return 0;
            }

            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(hwnd, &ps);
                RECT rc;
                GetClientRect(hwnd, &rc);

                // Cached, and self-validating: the brush is only rebuilt if the
                // theme now yields a different colour. No allocation in the
                // steady state, and no theme-change hook needed to keep it honest.
                FillRect(hdc, &rc, EnsureBrush(s_bgBrush, s_bgColor, BgColor()));
                FrameRect(hdc, &rc, EnsureBrush(s_borderBrush, s_borderColor, BorderColor()));

                EnsureFont();
                const UINT dpi = static_cast<UINT>(app.dpiScale * 96.0f);
                RECT tr = rc;
                InflateRect(&tr, -MulDiv(PAD_X, dpi, 96), -MulDiv(PAD_Y, dpi, 96));

                HFONT old = static_cast<HFONT>(SelectObject(hdc, s_font));
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, TextColor());
                DrawTextW(hdc, s_text.c_str(), -1, &tr,
                          DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
                SelectObject(hdc, old);

                EndPaint(hwnd, &ps);
                return 0;
            }
            default: break;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

} // namespace UI
